#include "kernel.h"
#include "pci.h"
#include <beans.h>

////////////////////////////////
// Syscall table
////////////////////////////////
#define SYS_FN(name) static uintptr_t syscall_ ## name(CPUState* state, PerCPU* cpu)
typedef uintptr_t SyscallFn(CPUState* state, PerCPU* cpu);

#ifdef __x86_64__
#include "threads.h"

#define GET_RETURN(s) (s)->rax
#define GET_PARAM0(s) (s)->rdi
#define GET_PARAM1(s) (s)->rsi
#define GET_PARAM2(s) (s)->rdx
#define GET_PARAM3(s) (s)->r10
#define GET_PARAM4(s) (s)->r8
#define GET_PARAM5(s) (s)->r9
#define GET_PARAM6(s) (s)->r12
#define GET_PARAM7(s) (s)->r13
#else
#error "TODO: Syscall parameters aren't available for this arch"
#endif

#define SYS_PARAM0 GET_PARAM0(state)
#define SYS_PARAM1 GET_PARAM1(state)
#define SYS_PARAM2 GET_PARAM2(state)
#define SYS_PARAM3 GET_PARAM3(state)
#define SYS_PARAM4 GET_PARAM4(state)
#define SYS_PARAM5 GET_PARAM5(state)
#define SYS_PARAM6 GET_PARAM6(state)
#define SYS_PARAM7 GET_PARAM7(state)

#define KCHECK(pred, code) if ((pred) == 0) { return code; }
#define KVALIDATE(pred) if (res = (pred), res < 0) { return res; }

// copies from userland (and only userland), returns false if it can't
static bool ingest_usermem(void* dst, uintptr_t src, size_t size) {
    memcpy(dst, (const void*) src, size);
    return true;
}

// copies to userland (and only userland), returns false if it can't
static bool egest_usermem(uintptr_t dst, void* src, size_t size) {
    memcpy((void*) dst, src, size);
    return true;
}

static uintptr_t translate_vaddr(Env* env, uintptr_t vaddr) {
    uintptr_t page_aligned = vaddr & -PAGE_SIZE;
    uintptr_t page_offset = (vaddr & PAGE_SIZE - 1);

    VMem_Cursor cursor = vmem_node_lookup(env, page_aligned);
    if (cursor.node == NULL) {
        // literally no pages
        return 0;
    }

    // check if we're in range
    VMem_PageDesc* desc  = &cursor.node->vals[cursor.index];
    uintptr_t start_addr = cursor.node->keys[cursor.index];
    uintptr_t end_addr   = start_addr + desc->size;
    if (!desc->valid || page_aligned < start_addr || page_aligned >= end_addr) {
        // we're in the gap between page descriptor
        return 0;
    }

    VMem_WorkingSet* ws = &env->addr_space.working_set;
    uintptr_t in_space_addr = page_aligned;
    if (desc->vmo != 0) {
        // Translate address into VMO space
        size_t offset = page_aligned - start_addr;
        in_space_addr = desc->offset + offset;

        if (desc->vmo->paddr) {
            // physical addresses don't get cached in the working set, we're
            // better off just not putting entries into a hash map.
            return desc->vmo->paddr + in_space_addr + page_offset;
        }

        // TODO(NeGate): implement pager behavior
        ws = &desc->vmo->pages;
    }

    uintptr_t paddr = vmem_translate(ws, in_space_addr);
    if (paddr == 0) {
        paddr = vmem_try_commit(env, desc, page_aligned, start_addr, end_addr);
    }
    return paddr ? paddr + page_offset : 0;
}

static bool copy_across_spaces(Env* dst, uintptr_t dst_vaddr, const char* src_vaddr, size_t size) {
    uintptr_t end_dst = dst_vaddr + size;
    while (dst_vaddr != end_dst) {
        // EoP is End-of-Page
        size_t eop_dst = (dst_vaddr + PAGE_SIZE) & -PAGE_SIZE;
        if (eop_dst > end_dst) { eop_dst = end_dst; }
        // Copy subregion of the page
        uintptr_t dst_paddr = translate_vaddr(dst, dst_vaddr);
        memcpy(paddr2kaddr(dst_paddr), src_vaddr, eop_dst - dst_vaddr);
        // Advance to the next page
        src_vaddr += eop_dst - dst_vaddr;
        dst_vaddr = eop_dst;
    }
    return true;
}

static _Noreturn void yield_thread(CPUState* state, PerCPU* cpu, Thread* thread) {
    uintptr_t cr3 = kaddr2paddr(thread->parent->addr_space.hw_tables);

    state->interrupt_num = 32;
    uintptr_t new_cr3 = x86_irq_int_handler(state, cr3, cpu);
    do_context_switch(state, new_cr3, thread->utcb_addr);
}

static int get_obj_with_rights(Env* env, KObjectID id, int tag, KAccessRights exp, KObject** out_obj) {
    exp |= KACCESS_READ;

    KAccessRights rights;
    KObject* obj = env_get_handle(env, id, &rights);
    KCHECK(obj, RESULT_NO_HANDLE);
    KCHECK(obj->tag == tag, RESULT_WRONG_HANDLE);
    KCHECK((rights & exp) == exp, RESULT_BAD_PERMISSION);
    *out_obj = obj;
    return 0;
}
#define GET_OBJ_WITH_RIGHTS(env, id, tag, exp, out_obj) get_obj_with_rights(env, id, tag, exp, (KObject**) out_obj)

static KHandle ingest_user_handle(uintptr_t ptr) {
    KHandle res = 0;
    if (ptr && ingest_usermem(&res, ptr, sizeof(KHandle))) {
        return res;
    }
    return 0;
}

void transfer_time(PerCPU* cpu, Thread* curr, Thread* next, CPUState* state) {
    // save out state
    curr->state = *state;

    next->client.start_time = curr->client.start_time;
    next->client.v_time     = curr->client.v_time;
    next->client.v_deadline = curr->client.v_deadline;
    next->client.is_blocked = false;
    next->wait_obj = NULL;

    // replace curr thread in scheduler state
    cpu->current_thread = next;
}

static int mailbox_xfer(CPUState* state, PerCPU* cpu, Thread* curr, Thread* next, MSG_Tag tag) {
    // the sender thread is now blocked
    transfer_time(cpu, curr, next, state);

    KCHECK(memmap_translate(next->parent->addr_space.hw_tables, next->utcb_addr, NULL), RESULT_BAD_MEMORY);
    UTCB* dst_utcb = (UTCB*) next->utcb_addr;

    // transfer inline args (MR0, MR1)
    GET_RETURN(&next->state) = SYS_PARAM1;
    GET_PARAM4(&next->state) = SYS_PARAM4;
    GET_PARAM5(&next->state) = SYS_PARAM5;
    GET_PARAM6(&next->state) = SYS_PARAM6;
    GET_PARAM7(&next->state) = SYS_PARAM7;

    arch_set_address_space(next->parent);

    // copy remaining untyped regs into UTCB
    if (tag.untyped > 4 || tag.typed > 0) {
        Env* env = curr->parent;

        u64 utcb_paddr;
        // can't cross page boundary
        KCHECK((curr->utcb_addr & (PAGE_SIZE - 1)) + sizeof(UTCB) > PAGE_SIZE, RESULT_BAD_MEMORY);
        // translate UTCB, should be resident in memory if we just wrote to it
        KCHECK(memmap_translate(curr->parent->addr_space.hw_tables, curr->utcb_addr, &utcb_paddr), RESULT_BAD_MEMORY);

        UTCB* src_utcb = paddr2kaddr(utcb_paddr);
        FOR_N(i, 4, tag.untyped) {
            dst_utcb->mr[i] = src_utcb->mr[i];
        }

        // copy handles
        FOR_N(i, 0, tag.typed) {
            KObject* obj = env_get_handle(env, src_utcb->hr[i], NULL);
            KCHECK(obj, RESULT_NO_HANDLE);
            env_grant_rights(next->parent, 0, obj);

            dst_utcb->hr[i] = src_utcb->hr[i];
        }
    }

    return 0;
}

#include "syscall_wrappers.h"

////////////////////////////////
// Syscall impls
////////////////////////////////
static uintptr_t syscall_test(CPUState* state, PerCPU* cpu, uint64_t a0) {
    printf("SYS_test(%p)\n", a0);
    return 0;
}

static uintptr_t syscall_debug_log(CPUState* state, PerCPU* cpu, KHandle vmo_handle, size_t len) {
    Env* env = cpu->current_thread->parent;
    KObject_VMO* vmo = env_get_handle(env, vmo_handle, NULL);
    KCHECK(vmo, RESULT_NO_HANDLE);
    KCHECK(vmo->super.tag == KOBJECT_VMO, RESULT_WRONG_HANDLE);

    if (len > vmo->size) {
        len = vmo->size;
    }

    for (size_t i = 0; i < len; i += PAGE_SIZE) {
        size_t limit = i + PAGE_SIZE;
        if (limit > len) { limit = len; }

        uintptr_t actual_page = vmem_translate(&vmo->pages, i);
        kassert(actual_page, "TODO: uncommited page (%p => %p)", i, actual_page);

        char* page = paddr2kaddr(actual_page);
        FOR_N(j, i, limit) {
            _putchar(page[j]);
        }
    }
    return 0;
}

static uintptr_t syscall_thread_create(CPUState* state, PerCPU* cpu, KHandle env_handle, uintptr_t ip, uintptr_t sp, uintptr_t arg, uintptr_t utcb, int flags) {
    KCHECK((utcb & 63) == 0, RESULT_BAD_MEMORY);
    KCHECK((utcb & 4095) + sizeof(UTCB) > 4096, RESULT_BAD_MEMORY);

    int res;
    Env* env = cpu->current_thread->parent;

    Env* t_env = env;
    if (env_handle) {
        KVALIDATE(GET_OBJ_WITH_RIGHTS(env, env_handle, KOBJECT_ENV, KACCESS_WRITE, &t_env));
    }

    if ((flags & THREAD_FLAGS_GRANT) && arg) {
        KAccessRights rights;
        KObject* obj = env_get_handle(env, arg, &rights);
        KCHECK(obj, RESULT_BAD_PERMISSION);

        // import argument as handle
        env_grant_rights(t_env, KACCESS_WRITE, obj);
    }

    Thread* thread = thread_create(t_env, (ThreadEntryFn*) ip, arg, sp);
    KCHECK(thread, RESULT_NO_MEM);
    thread->utcb_addr = utcb;

    if ((flags & THREAD_FLAGS_SUSPEND) == 0) {
        thread_resume(thread, NULL);
    }

    return env_grant_rights(env, KACCESS_WRITE, &thread->super);
}

static uintptr_t syscall_thread_control(CPUState* state, PerCPU* cpu, KHandle thread_handle, int request, uint64_t arg, Rawptr ptr) {
    Env* env = cpu->current_thread->parent;
    Thread* thread = cpu->current_thread;

    bool self = true;
    if (thread_handle && thread_handle != thread->super.id) {
        thread = env_get_handle(env, thread_handle, NULL);
        KCHECK(thread, RESULT_NO_HANDLE);
        KCHECK(thread->super.tag == KOBJECT_THREAD, RESULT_WRONG_HANDLE);

        // thread_control on another thread
        self = false;
    }

    switch (request) {
        case THREAD_CTRL_EXIT: {
            KCHECK(self, RESULT_BAD_PERMISSION);
            thread->client.is_dead = true;
            yield_thread(state, cpu, thread);
        }

        case THREAD_CTRL_SLEEP: {
            KCHECK(self, RESULT_BAD_PERMISSION);
            sched_wait(arg);
            yield_thread(state, cpu, thread);
        }

        default: return RESULT_BAD_PERMISSION;
    }
    return 0;
}

static uintptr_t syscall_env_create(CPUState* state, PerCPU* cpu) {
    Env* parent = cpu->current_thread->parent;
    Env* env    = env_create();
    return env_grant_rights(parent, KACCESS_WRITE, &env->super);
}

static uintptr_t syscall_mem_map(CPUState* state, PerCPU* cpu, KHandle env_handle, MapControl ctrl, KHandle vmo_handle, uintptr_t offset, size_t size, int flags) {
    int res;
    size_t page_aligned_size = (size + PAGE_SIZE - 1) & -PAGE_SIZE;
    KCHECK(page_aligned_size, 0);

    uint32_t mem_flags = 0;
    if (ctrl.prot & PROT_W) { mem_flags |= VMEM_PAGE_WRITE; }
    if (ctrl.prot & PROT_X) { mem_flags |= VMEM_PAGE_EXEC;  }

    Env* env = cpu->current_thread->parent;
    Env* map_env = env;
    if (env_handle != 0) {
        KVALIDATE(GET_OBJ_WITH_RIGHTS(env, env_handle, KOBJECT_ENV, KACCESS_WRITE, &map_env));
    }

    KObject_VMO* vmo = NULL;
    if (vmo_handle != 0) {
        // TODO(NeGate): validate permissions correctly when mapping VMOs
        KVALIDATE(GET_OBJ_WITH_RIGHTS(env, vmo_handle, KOBJECT_VMO, 0, &vmo));

        if (map_env != env) {
            env_grant_rights(map_env, 0, &vmo->super);
        }
    }

    return vmem_map(map_env, vmo, ctrl.addr << 4ull, offset, page_aligned_size, mem_flags);
}

static uintptr_t syscall_mem_unmap(CPUState* state, PerCPU* cpu, KHandle env_handle, uintptr_t vaddr, size_t size) {
    Env* env = cpu->current_thread->parent;

    // we can't have multiple writers on the interval tree at once
    // but we don't need a TLB shootdown until page writing.
    rwlock_lock_exclusive(&env->addr_space.lock);
    // TODO(NeGate): actually remove the PTEs
    arch_tlb_shootdown(env);
    // TODO(NeGate): actually recycle the memory from those PTEs
    rwlock_unlock_exclusive(&env->addr_space.lock);
    return 0;
}

static uintptr_t syscall_mem_translate(CPUState* state, PerCPU* cpu, KHandle env_handle, Rawptr addr) {
    int res;
    Env* env = cpu->current_thread->parent;
    if (env_handle) {
        // TODO(NeGate): viewing physical addresses shouldn't be unprivileged
        KVALIDATE(GET_OBJ_WITH_RIGHTS(env, env_handle, KOBJECT_ENV, 0, &env));
    }
    return translate_vaddr(env, (uintptr_t) addr);
}

static uintptr_t syscall_vmo_create(CPUState* state, PerCPU* cpu, size_t size) {
    Env* env = cpu->current_thread->parent;
    KObject_VMO* vmo_ptr = vmo_create_physical(0, size, VMEM_PAGE_WRITE);
    return env_grant_rights(env, KACCESS_WRITE, &vmo_ptr->super);
}

static uintptr_t syscall_vmo_get_size(CPUState* state, PerCPU* cpu, KHandle vmo_handle) {
    Env* env = cpu->current_thread->parent;
    KObject_VMO* vmo = env_get_handle(env, SYS_PARAM0, NULL);
    KCHECK(vmo, RESULT_NO_HANDLE);
    KCHECK(vmo->super.tag == KOBJECT_VMO, RESULT_WRONG_HANDLE);
    kprintf("AAA %zu\n", vmo->size);
    return vmo->size;
}

static uintptr_t syscall_event_create(CPUState* state, PerCPU* cpu) {
    Env* parent = cpu->current_thread->parent;
    KObject_Event* event = event_create();
    return env_grant_rights(parent, 0, &event->super);
}

static uintptr_t syscall_event_op(CPUState* state, PerCPU* cpu, int op, KHandle event_handle) {
    Env* env = cpu->current_thread->parent;
    KObject_Event* event = env_get_handle(env, event_handle, NULL);
    KCHECK(event, RESULT_NO_HANDLE);
    KCHECK(event->super.tag == KOBJECT_EVENT, RESULT_WRONG_HANDLE);

    if (op == EVENT_OP_WAIT) {
        bool res = event_wait(event, cpu->current_thread);
        if (!res) {
            // Continue running, we signalled already
            return 0;
        }
        yield_thread(state, cpu, cpu->current_thread);
    } else if (op == EVENT_OP_SIGNAL) {
        Thread* next = event_signal(event);
        if (next != NULL) {
            next->client.is_blocked = false;
            next->wait_obj = NULL;
            thread_resume(next, cpu);
        }
        return 0;
    } else {
        return RESULT_BAD_PARAM;
    }
}

static uintptr_t syscall_pci_peek_device(CPUState* state, PerCPU* cpu, size_t index, Rawptr out_key) {
    Env* env = cpu->current_thread->parent;
    if (index >= pci_dev_count) {
        return RESULT_NO_HANDLE;
    }

    Env* env_null = NULL;
    PCI_Device* dev = pci_devs[index];
    if (atomic_compare_exchange_strong(&dev->parent, &env_null, env)) {
        u32 key = (dev->vendor_id << 16ull) | dev->device_id;

        egest_usermem((uintptr_t) out_key, &key, sizeof(u32));
        return env_grant_rights(env, KACCESS_WRITE, &dev->super);
    }
    return 0;
}

static uintptr_t syscall_pci_claim_device(CPUState* state, PerCPU* cpu, KHandle dev_handle, Rawptr out_dev_unsafe) {
    Env* env = cpu->current_thread->parent;

    PCI_Device* dev = env_get_handle(env, dev_handle, NULL);
    KCHECK(dev, RESULT_NO_HANDLE);
    KCHECK(dev->super.tag == KOBJECT_DEV_PCI, RESULT_WRONG_HANDLE);

    // validate the memory of out_dev
    PCI_Desc* out_dev  = out_dev_unsafe;
    out_dev->vend_prod = (dev->vendor_id << 16ull) | dev->device_id;
    out_dev->bar_count = dev->bar_count;
    FOR_N(i, 0, 6) {
        out_dev->sizes[i] = 0;
        out_dev->bars[i]  = NULL_HANDLE;
    }

    kprintf("PCI %p\n", dev);
    for (size_t i = 0; i < dev->bar_count; i++) {
        if ((dev->bar[i].value & 1) == 0) {
            continue; // I/O bar
        }

        Raw_BAR* bar = &dev->bar[i];
        size_t size = ~bar->size + 1;
        uintptr_t addr = (bar->value >> 4) << 4;
        uint8_t type = (bar->value >> 1) & 3;

        kassert(type == 0 || type == 2, "TODO: Unsupported BAR (%d)", type);
        if (type == 2) { // 64bit BAR
            KCHECK(i < dev->bar_count, RESULT_NO_BAR);
            addr |= ((uintptr_t) dev->bar[i].value) << 32ull;
        }

        VMem_Flags flags = (bar->value >> 3) & 1 ? VMEM_PAGE_WRITETHRU : VMEM_PAGE_UNCACHED;
        KObject_VMO* vmo_ptr = vmo_create_physical(addr, size, flags);
        kprintf("BAR[%zu] %p %#zx\n", i, addr, size);

        out_dev->sizes[i] = 0;
        out_dev->bars[i]  = env_grant_rights(env, 0, &vmo_ptr->super);

        i += (type == 2); // 64bit BAR
    }

    return 0;
}

static uintptr_t syscall_pci_read_config_32(CPUState* state, PerCPU* cpu, KHandle dev_handle, size_t offset) {
    Env* env = cpu->current_thread->parent;
    PCI_Device* dev = env_get_handle(env, dev_handle, NULL);
    KCHECK(dev, RESULT_NO_HANDLE);
    KCHECK(dev->super.tag == KOBJECT_DEV_PCI, RESULT_WRONG_HANDLE);

    return pci_read_u32(dev->bus, dev->device, dev->func, SYS_PARAM1);
}

static uintptr_t syscall_pci_write_config_32(CPUState* state, PerCPU* cpu, KHandle dev_handle, size_t offset, uint32_t val) {
    Env* env = cpu->current_thread->parent;
    PCI_Device* dev = env_get_handle(env, dev_handle, NULL);
    KCHECK(dev, RESULT_NO_HANDLE);
    KCHECK(dev->super.tag == KOBJECT_DEV_PCI, RESULT_WRONG_HANDLE);

    pci_write_u32(dev->bus, dev->device, dev->func, offset, val);
    return 0;
}

static uintptr_t syscall_mailbox_create(CPUState* state, PerCPU* cpu) {
    Env* env = cpu->current_thread->parent;
    KObject_Mailbox* mailbox = mailbox_create();
    return env_grant_rights(env, 0, &mailbox->super);
}

static uintptr_t syscall_mailbox_ipc(CPUState* state, PerCPU* cpu, KHandle mailbox_handle, KHandle to_handle, MSG_Tag tag, KHandle from_handle) {
    Env* env = cpu->current_thread->parent;

    KObject_Mailbox* mailbox = env_get_handle(env, mailbox_handle, NULL);
    KCHECK(mailbox, RESULT_NO_HANDLE);
    KCHECK(mailbox->super.tag == KOBJECT_THREAD, RESULT_WRONG_HANDLE);

    Thread* curr = cpu->current_thread;
    Thread* next = NULL;
    if (tag.flags & MAILBOX_IPC_SEND) {
        if (to_handle == NULL_HANDLE) {
            // No one around to respond? just wait, we'll retry the
            // syscall when the time comes.
            next = waitqueue_wake(&mailbox->tx_wait, cpu, false);
            if (next == NULL) {
                const uint8_t* code = (const uint8_t*) state->rip;

                #ifdef __x86_64__
                kassert(code[-2] == 0x0F && code[-1] == 0x05, "not a syscall? huh?");
                state->rip -= 2; // 0F 05
                #else
                #error "TODO"
                #endif

                waitqueue_wait(&mailbox->tx_wait, curr);
                yield_thread(state, cpu, curr);
            }
        } else {
            KObject* to = env_get_handle(env, to_handle, NULL);
            KCHECK(to, RESULT_NO_HANDLE);
            KCHECK(to->tag == KOBJECT_THREAD, RESULT_WRONG_HANDLE);

            next = (Thread*) to;
            KCHECK(atomic_ldacq(&next->wait_obj) == mailbox, RESULT_BAD_PERMISSION);
        }
        kassert(next->parent, "mailboxes can't live in kernel-threads");

        // We must be responded to by the same mailbox
        curr->client.is_blocked = true;
        curr->wait_obj = mailbox;

        SYS_PARAM3 = next->super.id;

        int res = mailbox_xfer(state, cpu, curr, next, tag);
        if (res < 0) { return res; }
    }

    if (tag.flags & MAILBOX_IPC_RECV) {
        // Go back to waiting
        curr->client.is_blocked = true;
        curr->wait_obj = mailbox;
        waitqueue_wait(&mailbox->rx_wait, curr);
        // Notify any senders who think there's no one waiting
        waitqueue_wake(&mailbox->tx_wait, cpu, true);
    }

    if (next != NULL) {
        do_context_switch(&next->state, 0, next->utcb_addr);
    } else {
        yield_thread(state, cpu, curr);
    }
}

extern KObject_Mailbox* kernel_root_mailbox;
static uintptr_t syscall_root_mailbox(CPUState* state, PerCPU* cpu) {
    Env* parent = cpu->current_thread->parent;
    return env_grant_rights(parent, KACCESS_WRITE, &kernel_root_mailbox->super);
}

static uintptr_t syscall_tsc_freq(CPUState* state, PerCPU* cpu) {
    return boot_info->tsc_freq;
}

#undef SYS_PARAM0
#undef SYS_PARAM1
#undef SYS_PARAM2
#undef SYS_PARAM3
#undef SYS_PARAM4
#undef SYS_PARAM5
#undef SYS_PARAM6
#undef SYS_PARAM7
