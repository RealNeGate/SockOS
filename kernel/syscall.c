#include "kernel.h"
#include "pci.h"
#include <beans.h>

////////////////////////////////
// Syscall table
////////////////////////////////
#define SYS_FN(name) static uintptr_t syscall_ ## name(CPUState* state, uintptr_t cr3, PerCPU* cpu)
typedef uintptr_t SyscallFn(CPUState* state, uintptr_t cr3, PerCPU* cpu);

#ifdef __x86_64__
#include "threads.h"

#define GET_RETURN(s) (s)->rax
#define GET_PARAM0(s) (s)->rdi
#define GET_PARAM1(s) (s)->rsi
#define GET_PARAM2(s) (s)->rdx
#define GET_PARAM3(s) (s)->r10
#define GET_PARAM4(s) (s)->r8
#define GET_PARAM5(s) (s)->r9
#else
#error "TODO: Syscall parameters aren't available for this arch"
#endif

#define SYS_PARAM0 GET_PARAM0(state)
#define SYS_PARAM1 GET_PARAM1(state)
#define SYS_PARAM2 GET_PARAM2(state)
#define SYS_PARAM3 GET_PARAM3(state)
#define SYS_PARAM4 GET_PARAM4(state)
#define SYS_PARAM5 GET_PARAM5(state)

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

static _Noreturn void yield_syscall(PerCPU* cpu, uintptr_t cr3, CPUState* state, WaitQueue* wq) {
    const uint8_t* code = (const uint8_t*) state->rip;

    #ifdef __x86_64__
    kassert(code[-2] == 0x0F && code[-1] == 0x05, "not a syscall? huh?");
    state->rip -= 2; // 0F 05
    #else
    #error "TODO"
    #endif

    Thread* old = cpu->current_thread;
    waitqueue_wait(wq, cpu->current_thread);

    state->interrupt_num = 32;
    uintptr_t new_cr3 = x86_irq_int_handler(state, cr3, cpu);
    do_context_switch(state, new_cr3);
}

static _Noreturn void yield_thread(CPUState* state, uintptr_t cr3, PerCPU* cpu) {
    state->interrupt_num = 32;
    uintptr_t new_cr3 = x86_irq_int_handler(state, cr3, cpu);
    do_context_switch(state, new_cr3);
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

#include "syscall_wrappers.h"

////////////////////////////////
// Syscall impls
////////////////////////////////
static uintptr_t syscall_test(CPUState* state, uintptr_t cr3, PerCPU* cpu, uint64_t a0) {
    printf("SYS_test(%p)\n", a0);
    return 0;
}

static uintptr_t syscall_debug_log(CPUState* state, uintptr_t cr3, PerCPU* cpu, KHandle vmo_handle, size_t len) {
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

static uintptr_t syscall_thread_create(CPUState* state, uintptr_t cr3, PerCPU* cpu, KHandle env_handle, uintptr_t ip, uintptr_t sp, uintptr_t arg, uintptr_t utcb, int flags) {
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

    // make an accessible handle for the thread
    thread_resume(thread, NULL);
    return env_grant_rights(env, KACCESS_WRITE, &thread->super);
}

static uintptr_t syscall_thread_control(CPUState* state, uintptr_t cr3, PerCPU* cpu, KHandle thread_handle, int request, uint64_t arg, Rawptr ptr) {
    Env* env = cpu->current_thread->parent;
    Thread* thread = cpu->current_thread;

    if (thread_handle && thread_handle != thread->super.id) {
        thread = env_get_handle(env, thread_handle, NULL);
        KCHECK(thread, RESULT_NO_HANDLE);
        KCHECK(thread->super.tag == KOBJECT_THREAD, RESULT_WRONG_HANDLE);

        // thread_control on another thread
        return RESULT_BAD_PERMISSION;
    } else {
        // thread_control on self
        switch (request) {
            case THREAD_CTRL_EXIT: {
                thread->client.is_dead = true;
                yield_thread(state, cr3, cpu);
            }

            case THREAD_CTRL_SLEEP: {
                sched_wait(arg);
                yield_thread(state, cr3, cpu);
            }

            default: return RESULT_BAD_PERMISSION;
        }
    }
    return 0;
}

static uintptr_t syscall_env_create(CPUState* state, uintptr_t cr3, PerCPU* cpu) {
    Env* parent = cpu->current_thread->parent;
    Env* env    = env_create();
    return env_grant_rights(parent, KACCESS_WRITE, &env->super);
}

static uintptr_t syscall_mem_map(CPUState* state, uintptr_t cr3, PerCPU* cpu, KHandle env_handle, MapControl ctrl, KHandle vmo_handle, uintptr_t offset, size_t size, int flags) {
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

static uintptr_t syscall_mem_unmap(CPUState* state, uintptr_t cr3, PerCPU* cpu, KHandle env_handle, uintptr_t vaddr, size_t size) {
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

static uintptr_t syscall_mem_translate(CPUState* state, uintptr_t cr3, PerCPU* cpu, KHandle env_handle, Rawptr addr) {
    int res;
    Env* env = cpu->current_thread->parent;
    if (env_handle) {
        // TODO(NeGate): viewing physical addresses shouldn't be unprivileged
        KVALIDATE(GET_OBJ_WITH_RIGHTS(env, env_handle, KOBJECT_ENV, 0, &env));
    }
    return translate_vaddr(env, (uintptr_t) addr);
}

static uintptr_t syscall_vmo_create(CPUState* state, uintptr_t cr3, PerCPU* cpu, size_t size) {
    Env* env = cpu->current_thread->parent;
    KObject_VMO* vmo_ptr = vmo_create_physical(0, size, VMEM_PAGE_WRITE);
    return env_grant_rights(env, KACCESS_WRITE, &vmo_ptr->super);
}

static uintptr_t syscall_vmo_get_size(CPUState* state, uintptr_t cr3, PerCPU* cpu, KHandle vmo_handle) {
    Env* env = cpu->current_thread->parent;
    KObject_VMO* vmo = env_get_handle(env, SYS_PARAM0, NULL);
    KCHECK(vmo, RESULT_NO_HANDLE);
    KCHECK(vmo->super.tag == KOBJECT_VMO, RESULT_WRONG_HANDLE);
    return vmo->size;
}

static uintptr_t syscall_event_create(CPUState* state, uintptr_t cr3, PerCPU* cpu) {
    Env* parent = cpu->current_thread->parent;
    KObject_Event* event = event_create();
    return env_grant_rights(parent, 0, &event->super);
}

static uintptr_t syscall_event_wait(CPUState* state, uintptr_t cr3, PerCPU* cpu, KHandle event_handle) {
    Env* env = cpu->current_thread->parent;
    KObject_Event* event = env_get_handle(env, event_handle, NULL);
    KCHECK(event, RESULT_NO_HANDLE);
    KCHECK(event->super.tag == KOBJECT_EVENT, RESULT_WRONG_HANDLE);

    bool res = event_wait(event, cpu->current_thread);
    if (!res) {
        // Continue running, we signalled already
        return 0;
    }
    yield_thread(state, cr3, cpu);
}

static uintptr_t syscall_event_signal(CPUState* state, uintptr_t cr3, PerCPU* cpu, KHandle event_handle) {
    Env* env = cpu->current_thread->parent;
    KObject_Event* event = env_get_handle(env, event_handle, NULL);
    KCHECK(event, RESULT_NO_HANDLE);
    KCHECK(event->super.tag == KOBJECT_EVENT, RESULT_WRONG_HANDLE);

    Thread* next = event_signal(event);
    if (next != NULL) {
        next->client.is_blocked = false;
        next->wait_obj = NULL;
        thread_resume(next, cpu);
    }
}

extern KObject_Mailbox* kernel_root_mailbox;
static uintptr_t syscall_root_mailbox(CPUState* state, uintptr_t cr3, PerCPU* cpu) {
    Env* parent = cpu->current_thread->parent;
    return env_grant_rights(parent, KACCESS_WRITE, &kernel_root_mailbox->super);
}

#if 0
////////////////////////////////
// Syscall table
////////////////////////////////
SYS_FN(sleep) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_sleep(t=%d us)\n", SYS_PARAM0));
    sched_wait(SYS_PARAM0);

    state->interrupt_num = 32;
    uintptr_t new_cr3 = x86_irq_int_handler(state, cr3, cpu);
    do_context_switch(state, new_cr3);
}

SYS_FN(env_create) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_env_create()\n"));
    Env* parent = cpu->current_thread->parent;
    Env* env    = env_create();
    return env_grant_rights(parent, KACCESS_WRITE, &env->super);
}

SYS_FN(root_mailbox) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_root_mailbox()\n"));
    Env* parent = cpu->current_thread->parent;
    return env_grant_rights(parent, KACCESS_WRITE, &kernel_root_mailbox->super);
}

SYS_FN(vmo_create) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_vmo_create(paddr=%p, size=%d)\n", SYS_PARAM0, SYS_PARAM1));
    Env* env = cpu->current_thread->parent;
    KObject_VMO* vmo_ptr = vmo_create_physical(SYS_PARAM0, SYS_PARAM1, VMEM_PAGE_WRITE);
    return env_grant_rights(env, KACCESS_WRITE, &vmo_ptr->super);
}

SYS_FN(vmo_get_size) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_vmo_get_size(vmo=%p)\n", SYS_PARAM0));

    Env* env = cpu->current_thread->parent;
    KObject_VMO* vmo = env_get_handle(env, SYS_PARAM0, NULL);
    KCHECK(vmo, RESULT_NO_HANDLE);
    KCHECK(vmo->super.tag == KOBJECT_VMO, RESULT_WRONG_HANDLE);
    return vmo->size;
}

SYS_FN(mmap) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_mmap(env=%p, vmo=%p, addr=%p, size=%d, prot=%x, offset=%d)\n", SYS_PARAM0, SYS_PARAM1, SYS_PARAM2, SYS_PARAM3, SYS_PARAM4, SYS_PARAM5));

    size_t   size = SYS_PARAM3;
    uint32_t prot = SYS_PARAM4;
    size_t offset = SYS_PARAM5;

    uint32_t flags = 0;
    if (prot & PROT_WRITE) { flags |= VMEM_PAGE_WRITE; }

    size_t page_aligned_size = (size + PAGE_SIZE - 1) & -PAGE_SIZE;
    KCHECK(page_aligned_size, 0);

    Env* env = cpu->current_thread->parent;

    int res;
    Env* map_env = env;
    if (SYS_PARAM0) {
        KVALIDATE(GET_OBJ_WITH_RIGHTS(env, SYS_PARAM0, KOBJECT_ENV, KACCESS_WRITE, &map_env));
    }

    KObject_VMO* vmo = NULL;
    if (SYS_PARAM1) {
        // TODO(NeGate): validate permissions correctly when mapping VMOs
        KVALIDATE(GET_OBJ_WITH_RIGHTS(env, SYS_PARAM1, KOBJECT_VMO, 0, &vmo));
        flags |= vmo->flags;

        if (map_env != env) {
            env_grant_rights(map_env, 0, &vmo->super);
        }
    }

    return vmem_map(map_env, vmo, SYS_PARAM2, offset, page_aligned_size, flags, NULL);
}

SYS_FN(mdump) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_mdump(env=%p)\n", SYS_PARAM0));

    Env* env = cpu->current_thread->parent;
    if (SYS_PARAM0) {
        env = env_get_handle(env, SYS_PARAM0, NULL);
        KCHECK(env, RESULT_NO_HANDLE);
        KCHECK(env->super.tag == KOBJECT_ENV, RESULT_WRONG_HANDLE);
    }

    vmem_dump(env);
    return 0;
}

SYS_FN(get_paddr) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_get_paddr(vaddr=%p)\n", SYS_PARAM0));

    Env* env = cpu->current_thread->parent;
    uintptr_t paddr = translate_vaddr(env, SYS_PARAM0);
    return paddr;
}

SYS_FN(mpin) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_mpin(vmo=%p, offset=%d, size=%d, out_paddr=%p)\n", SYS_PARAM0, SYS_PARAM1, SYS_PARAM2, SYS_PARAM3));

    size_t page_aligned_size = (SYS_PARAM2 + PAGE_SIZE - 1) & -PAGE_SIZE;
    KCHECK(page_aligned_size, 0);

    Env* env = cpu->current_thread->parent;
    KObject_VMO* vmo = NULL;
    if (SYS_PARAM0) {
        vmo = env_get_handle(env, SYS_PARAM0, NULL);
        KCHECK(vmo, RESULT_NO_HANDLE);
        KCHECK(vmo->super.tag == KOBJECT_VMO, RESULT_WRONG_HANDLE);
    }

    uintptr_t paddr;
    uintptr_t mapped = vmem_map(env, vmo, 0, SYS_PARAM1, page_aligned_size, VMEM_PAGE_WRITE | VMEM_PAGE_PINNED, &paddr);

    egest_usermem(SYS_PARAM3, &paddr, sizeof(uintptr_t));
    return mapped;
}

SYS_FN(munmap) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_munmap()\n"));
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

SYS_FN(pci_device_count) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_pci_device_count()\n"));
    return pci_dev_count;
}

SYS_FN(pci_claim_device) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_pci_claim_device(index=%d, out_key=%p)\n", SYS_PARAM0, SYS_PARAM1));
    KCHECK(SYS_PARAM0 < pci_dev_count, 0);

    Env* env = cpu->current_thread->parent;

    Env* env_null = NULL;
    PCI_Device* dev = pci_devs[SYS_PARAM0];
    if (atomic_compare_exchange_strong(&dev->parent, &env_null, env)) {
        u32 key = (dev->vendor_id << 16ull) | dev->device_id;

        egest_usermem(SYS_PARAM1, &key, sizeof(u32));
        return env_grant_rights(env, KACCESS_WRITE, &dev->super);
    }

    return 0;
}

SYS_FN(pci_bar_count) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_pci_bar_count(out_mask=%p)\n", SYS_PARAM0));

    Env* env = cpu->current_thread->parent;
    PCI_Device* dev = env_get_handle(env, SYS_PARAM0, NULL);
    KCHECK(dev, RESULT_NO_HANDLE);
    KCHECK(dev->super.tag == KOBJECT_DEV_PCI, RESULT_WRONG_HANDLE);

    // mask of which BARs are memory
    u32 mask = 0;
    egest_usermem(SYS_PARAM0, &mask, sizeof(u32));
    return dev->bar_count;
}

SYS_FN(pci_get_bar) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_pci_get_bar(pci=%d, bar=%d, out_size=%p)\n", SYS_PARAM0, SYS_PARAM1, SYS_PARAM2));

    Env* env = cpu->current_thread->parent;
    PCI_Device* dev = env_get_handle(env, SYS_PARAM0, NULL);
    KCHECK(dev, RESULT_NO_HANDLE);
    KCHECK(dev->super.tag == KOBJECT_DEV_PCI, RESULT_WRONG_HANDLE);

    int bar_index = SYS_PARAM1;
    KCHECK(bar_index < dev->bar_count, RESULT_NO_BAR);
    KCHECK((dev->bar[bar_index].value & 0x1) == 0, RESULT_IO_BAR);

    Raw_BAR* bar = &dev->bar[bar_index];
    size_t size = ~bar->size + 1;
    uintptr_t addr = (bar->value >> 4) << 4;
    u8 type = (bar->value >> 1) & 0x3;

    kassert(type == 0 || type == 2, "TODO: Unsupported BAR (%d)", type);
    if (type == 2) { // 64bit BAR
        KCHECK(bar_index+1 < dev->bar_count, RESULT_NO_BAR);
        addr |= ((uintptr_t) dev->bar[bar_index + 1].value) << 32ull;
    }

    egest_usermem(SYS_PARAM2, &size, sizeof(size));
    bool prefetch = (bar->value >> 3) & 1;

    KObject_VMO* vmo_ptr = vmo_create_physical(addr, size, prefetch ? VMEM_PAGE_WRITETHRU : VMEM_PAGE_UNCACHED);
    return env_grant_rights(env, 0, &vmo_ptr->super);
}

SYS_FN(pci_read_config_32) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_pci_read_config_32(pci=%d, offset=%d)\n", SYS_PARAM0, SYS_PARAM1));

    Env* env = cpu->current_thread->parent;
    PCI_Device* dev = env_get_handle(env, SYS_PARAM0, NULL);
    KCHECK(dev, RESULT_NO_HANDLE);
    KCHECK(dev->super.tag == KOBJECT_DEV_PCI, RESULT_WRONG_HANDLE);

    return pci_read_u32(dev->bus, dev->device, dev->func, SYS_PARAM1);
}

SYS_FN(pci_write_config_32) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_pci_write_config_32(pci=%d, offset=%d, val=%d)\n", SYS_PARAM0, SYS_PARAM1, SYS_PARAM2));

    Env* env = cpu->current_thread->parent;
    PCI_Device* dev = env_get_handle(env, SYS_PARAM0, NULL);
    KCHECK(dev, RESULT_NO_HANDLE);
    KCHECK(dev->super.tag == KOBJECT_DEV_PCI, RESULT_WRONG_HANDLE);

    pci_write_u32(dev->bus, dev->device, dev->func, SYS_PARAM1, SYS_PARAM2);
    return 0;
}

SYS_FN(mailbox_create) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_mailbox_create(max_rqs=%d)\n", SYS_PARAM0));

    Env* env = cpu->current_thread->parent;
    KObject_Mailbox* mailbox = mailbox_create(SYS_PARAM0);
    if (mailbox == NULL) {
        return 0;
    }
    return env_grant_rights(env, 0, &mailbox->super);
}

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
    next->calling_thread = curr;

    // replace curr thread in scheduler state
    cpu->current_thread = next;
    // kprintf("XFER C%d -> C%d\n", curr->client.id, next->client.id);
}

static int mailbox_xfer(CPUState* state, PerCPU* cpu, Thread* curr, Thread* next, KObject_Mailbox* mailbox) {
    // the sender thread is now blocked
    transfer_time(cpu, curr, next, state);

    uint64_t  send_info   = SYS_PARAM1;
    uint64_t  send_msg    = SYS_PARAM4;
    KObjectID send_handle = SYS_PARAM5;

    uint64_t recv_info  = GET_PARAM1(&next->state);
    uintptr_t recv_msg  = GET_PARAM4(&next->state);

    // msg length should be the min of the sender and receiver
    size_t msg_len = send_info >> 32ull;
    if (msg_len > (recv_info >> 32ull)) {
        msg_len = (recv_info >> 32ull);
    }
    KCHECK(msg_len <= 64, RESULT_PACKET_TOO_BIG);
    GET_RETURN(&next->state) = (msg_len << 32ull) | (send_info & 0xFFFFFFFF);

    // transfer inline args
    GET_PARAM2(&next->state) = SYS_PARAM2;
    GET_PARAM3(&next->state) = SYS_PARAM3;
    GET_PARAM5(&next->state) = 0;

    // translate handle
    if (send_handle) {
        KObject* obj = env_get_handle(curr->parent, send_handle, NULL);
        KCHECK(obj, RESULT_NO_HANDLE);
        env_grant_rights(next->parent, 0, obj);
    }

    // copy message across address space
    if (msg_len > 0) {
        char* tmp = cpu->message_buffer;
        ingest_usermem(tmp, send_msg, msg_len);
        arch_set_address_space(next->parent);
        egest_usermem(recv_msg, tmp, msg_len);
    } else {
        arch_set_address_space(next->parent);
    }

    if (mailbox != NULL) {
        // Put the mailbox thread back on the wait list.
        mailbox_recv(mailbox, curr);
        // Notify any senders who think there's no one waiting
        waitqueue_wake(&mailbox->tx_wait, cpu);
    }

    // actually transition now
    do_context_switch(&next->state, 0);
}

SYS_FN(mailbox_send) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_mailbox_send(mailbox=%d, info=%p, arg0=%p, arg1=%p, body=%p, handle=%p)\n", SYS_PARAM0, SYS_PARAM1, SYS_PARAM2, SYS_PARAM3, SYS_PARAM4, SYS_PARAM5));

    Env* env = cpu->current_thread->parent;
    KObject_Mailbox* mailbox = env_get_handle(env, SYS_PARAM0, NULL);
    KCHECK(mailbox, RESULT_NO_HANDLE);
    KCHECK(mailbox->super.tag == KOBJECT_MAILBOX, RESULT_WRONG_HANDLE);

    Thread* curr = cpu->current_thread;
    Thread* next = mailbox_send(mailbox);
    if (next == NULL) {
        // TODO(NeGate): mailbox has no one waiting to answer responses,
        // we should setup a smarter scheduling at some point to signal
        // when it's ready but for now we'll just yield.
        yield_syscall(cpu, cr3, state, &mailbox->tx_wait);
    }
    kassert(next->parent, "mailboxes can't live in kernel-threads");

    // Put to wait on mailbox
    curr->client.is_blocked = true;
    curr->wait_obj = mailbox;
    return mailbox_xfer(state, cpu, curr, next, NULL);
}

SYS_FN(mailbox_reply) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_mailbox_reply(mailbox=%d, info=%p, data=%p, handle=%p)\n", SYS_PARAM0, SYS_PARAM1, SYS_PARAM4, SYS_PARAM5));

    Env* env = cpu->current_thread->parent;
    KObject_Mailbox* mailbox = env_get_handle(env, SYS_PARAM0, NULL);
    KCHECK(mailbox, RESULT_NO_HANDLE);
    KCHECK(mailbox->super.tag == KOBJECT_MAILBOX, RESULT_WRONG_HANDLE);

    Thread* curr = cpu->current_thread;
    Thread* next = curr->calling_thread;

    curr->client.is_blocked = true;
    curr->wait_obj = mailbox;
    curr->calling_thread = NULL;
    return mailbox_xfer(state, cpu, curr, next, mailbox);
}

SYS_FN(mailbox_wait) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_mailbox_wait(mailbox=%d, info=%p, data=%p, handle=%p)\n", SYS_PARAM0, SYS_PARAM1, SYS_PARAM4, SYS_PARAM5));

    Env* env = cpu->current_thread->parent;
    Thread* curr = cpu->current_thread;
    KObject_Mailbox* mailbox = env_get_handle(env, SYS_PARAM0, NULL);
    KCHECK(mailbox, RESULT_NO_HANDLE);
    KCHECK(mailbox->super.tag == KOBJECT_MAILBOX, RESULT_WRONG_HANDLE);

    // Wait on mailbox
    curr->client.is_blocked = true;
    curr->wait_obj = mailbox;
    mailbox_recv(mailbox, curr);

    // Notify any senders who think there's no one waiting
    waitqueue_wake(&mailbox->tx_wait, cpu);

    // Pick a new task
    state->interrupt_num = 32;
    uintptr_t new_cr3 = x86_irq_int_handler(state, cr3, cpu);
    do_context_switch(state, new_cr3);
}

// Replace this with a routine that stays in userland
SYS_FN(tsc_freq) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_tsc_freq()\n"));
    return boot_info->tsc_freq;
}
#endif

#undef SYS_PARAM0
#undef SYS_PARAM1
#undef SYS_PARAM2
#undef SYS_PARAM3
#undef SYS_PARAM4
#undef SYS_PARAM5