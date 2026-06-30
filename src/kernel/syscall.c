#include <kernel.h>
#include "pci.h"
#include <beans.h>

////////////////////////////////
// Syscall table
////////////////////////////////
#define SYS_FN(name) static uintptr_t syscall_ ## name(CPUState* state, uintptr_t cr3, PerCPU* cpu)
typedef uintptr_t SyscallFn(CPUState* state, uintptr_t cr3, PerCPU* cpu);

// forward decls
#define X(name, ...) static uintptr_t syscall_ ## name (CPUState*, uintptr_t, PerCPU*);
#include "syscall_table.h"

SyscallFn* syscall_table[] = {
    #define X(name, ...) [SYS_ ## name] = syscall_ ## name,
    #include "syscall_table.h"
};
size_t syscall_table_count = SYS_MAX;

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
    if (desc->vmo_handle != 0) {
        // Translate address into VMO space
        size_t offset = page_aligned - start_addr;
        in_space_addr = desc->offset + offset;

        KObject_VMO* vmo_ptr = env_get_handle(env, desc->vmo_handle, NULL);
        if (vmo_ptr == NULL) {
            return 0;
        }

        if (vmo_ptr->paddr) {
            // physical addresses don't get cached in the working set, we're
            // better off just not putting entries into a hash map.
            return vmo_ptr->paddr + in_space_addr + page_offset;
        }

        // TODO(NeGate): implement pager behavior
        ws = &vmo_ptr->pages;
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
    printf("YIELD SYSCALL!!!\n");

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

SYS_FN(sched_time) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_sched_time(arr=%p)\n", SYS_PARAM0));

    u64 now_time = __rdtsc() / boot_info->tsc_freq;
    u64* arr = (u64*) SYS_PARAM0;
    FOR_N(i, 0, boot_info->core_count) {
        PerCPU* some_cpu = &boot_info->cores[i];
        arr[i] = sched_total_exec_time(some_cpu, now_time);
    }
    return now_time;
}

SYS_FN(debug_log) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_debug_log(vmo=%p, length=%d)\n", SYS_PARAM0, SYS_PARAM1));

    Env* env = cpu->current_thread->parent;
    KObject_VMO* vmo = env_get_handle(env, SYS_PARAM0, NULL);
    KCHECK(vmo, RESULT_NO_HANDLE);
    KCHECK(vmo->super.tag == KOBJECT_VMO, RESULT_WRONG_HANDLE);

    size_t len = SYS_PARAM1;
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

SYS_FN(env_create) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_env_create()\n"));
    Env* parent = cpu->current_thread->parent;
    Env* env    = env_create();
    return env_open_handle(parent, 0, &env->super);
}

extern KObject_Mailbox* kernel_root_mailbox;
SYS_FN(get_root_mailbox) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_get_root_mailbox()\n"));
    Env* parent = cpu->current_thread->parent;
    return env_open_handle(parent, 0, &kernel_root_mailbox->super);
}

SYS_FN(vmo_create) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_vmo_create(paddr=%p, size=%d)\n", SYS_PARAM0, SYS_PARAM1));
    Env* env = cpu->current_thread->parent;
    KObject_VMO* vmo_ptr = vmo_create_physical(SYS_PARAM0, SYS_PARAM1, VMEM_PAGE_WRITE);
    return env_open_handle(env, 0, &vmo_ptr->super);
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
    if (SYS_PARAM0) {
        env = env_get_handle(env, SYS_PARAM0, NULL);
        KCHECK(env, RESULT_NO_HANDLE);
        KCHECK(env->super.tag == KOBJECT_ENV, RESULT_WRONG_HANDLE);
    }

    KHandle vmo_handle = SYS_PARAM1;
    if (SYS_PARAM1) {
        KObject_VMO* vmo = env_get_handle(cpu->current_thread->parent, SYS_PARAM1, NULL);
        KCHECK(vmo, RESULT_NO_HANDLE);
        KCHECK(vmo->super.tag == KOBJECT_VMO, RESULT_WRONG_HANDLE);

        flags |= vmo->flags;

        if (env != cpu->current_thread->parent) {
            vmo_handle = env_open_handle(env, 0, &vmo->super);
        }
    }

    return vmem_map(env, vmo_handle, SYS_PARAM2, offset, page_aligned_size, flags, NULL);
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
    if (SYS_PARAM0) {
        KObject_VMO* vmo = env_get_handle(env, SYS_PARAM0, NULL);
        KCHECK(vmo, RESULT_NO_HANDLE);
        KCHECK(vmo->super.tag == KOBJECT_VMO, RESULT_WRONG_HANDLE);
    }

    uintptr_t paddr;
    uintptr_t mapped = vmem_map(env, SYS_PARAM0, 0, SYS_PARAM1, page_aligned_size, VMEM_PAGE_WRITE | VMEM_PAGE_PINNED, &paddr);

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

SYS_FN(thread_create) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_thread_create(env=%p, fn=%p, arg=%p, stack_size=%d, flags=%d)\n", SYS_PARAM0, SYS_PARAM1, SYS_PARAM2, SYS_PARAM3, SYS_PARAM4));

    Env* env = cpu->current_thread->parent;
    if (SYS_PARAM0) {
        env = env_get_handle(env, SYS_PARAM0, NULL);
        KCHECK(env, RESULT_NO_HANDLE);
        KCHECK(env->super.tag == KOBJECT_ENV, RESULT_WRONG_HANDLE);
    }

    ThreadEntryFn* fn = (ThreadEntryFn*) SYS_PARAM1;
    uintptr_t arg = SYS_PARAM2;
    size_t stack_size = SYS_PARAM3;

    if (SYS_PARAM4 & 1) {
        KObject* obj = env_get_handle(cpu->current_thread->parent, arg, NULL);
        KCHECK(obj, RESULT_NO_HANDLE);

        // import argument as handle
        arg = env_open_handle(env, 0, obj);
    }

    uintptr_t stack_ptr = vmem_map(env, 0, 0, 0, stack_size, VMEM_PAGE_WRITE, NULL);
    KCHECK(stack_ptr, RESULT_NO_MEM);

    Thread* thread = thread_create(env, fn, arg, stack_ptr, stack_size);
    KCHECK(thread, RESULT_NO_MEM);

    // make an accessible handle for the thread
    thread_resume(thread, NULL);
    return env_open_handle(cpu->current_thread->parent, 0, &thread->super);
}

SYS_FN(thread_setattr) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_thread_setattr(%p, %d, %d)\n", SYS_PARAM0, SYS_PARAM1, SYS_PARAM2));
    Env* env = cpu->current_thread->parent;

    Thread* thread = env_get_handle(env, SYS_PARAM0, NULL);
    KCHECK(thread, RESULT_NO_HANDLE);
    KCHECK(thread->super.tag == KOBJECT_THREAD, RESULT_WRONG_HANDLE);

    if (SYS_PARAM1 == 0) {
        // thread->client.weight = 20;
    } else if (SYS_PARAM1 == 1) {
        // TODO(NeGate): make this safe
        int i = 0;
        const char* str = (const char*) SYS_PARAM2;
        for (; i < 31 && str[i]; i++) {
            thread->tag[i] = str[i];
        }
        thread->tag[i] = 0;
        // kprintf("SET NAME '%s'\n", thread->tag);
    }

    return 0;
}

SYS_FN(event_create) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_event_create()\n"));
    Env* parent = cpu->current_thread->parent;
    KObject_Event* event = event_create();
    return env_open_handle(parent, 0, &event->super);
}

SYS_FN(event_wait) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_event_wait(%p)\n", SYS_PARAM0));

    Env* env = cpu->current_thread->parent;
    KObject_Event* event = env_get_handle(env, SYS_PARAM0, NULL);
    KCHECK(event, RESULT_NO_HANDLE);
    KCHECK(event->super.tag == KOBJECT_EVENT, RESULT_WRONG_HANDLE);

    bool res = event_wait(event, cpu->current_thread);
    if (!res) {
        // Continue running, we signalled already
        return 0;
    }

    // Pick a new task
    state->interrupt_num = 32;
    uintptr_t new_cr3 = x86_irq_int_handler(state, cr3, cpu);
    do_context_switch(state, new_cr3);
}

SYS_FN(event_signal) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_event_signal(%p)\n", SYS_PARAM0));

    Env* env = cpu->current_thread->parent;
    KObject_Event* event = env_get_handle(env, SYS_PARAM0, NULL);
    KCHECK(event, RESULT_NO_HANDLE);
    KCHECK(event->super.tag == KOBJECT_EVENT, RESULT_WRONG_HANDLE);

    Thread* next = event_signal(event);
    if (next != NULL) {
        next->client.is_blocked = false;
        next->wait_obj = NULL;
        thread_resume(next, cpu);
    }
    return 0;
}

SYS_FN(test) {
    kprintf("SYS_test(%p)\n", SYS_PARAM0);

    for (size_t j = 0; j < 50; j++) {
        for (size_t i = 0; i < 50; i++) {
            boot_info->fb.pixels[i + (j * boot_info->fb.stride)] = SYS_PARAM0;
        }
    }

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
        return env_open_handle(env, 0, &dev->super);
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
    return env_open_handle(env, 0, &vmo_ptr->super);
}

SYS_FN(fb_grab) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_fb_grab(%p)\n", SYS_PARAM0));

    Env* env = cpu->current_thread->parent;
    uint64_t* info = (uint64_t*) SYS_PARAM0;
    info[0] = boot_info->fb.width;
    info[1] = boot_info->fb.height;
    info[2] = boot_info->fb.stride;
    info[3] = boot_info->fb.stride * 4 * boot_info->fb.height;

    KObject_VMO* vmo_ptr = vmo_create_physical(kaddr2paddr(boot_info->fb.pixels), info[3], VMEM_PAGE_WRITETHRU);
    return env_open_handle(env, 0, &vmo_ptr->super);
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
    return env_open_handle(env, 0, &mailbox->super);
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
    kprintf("XFER C%d -> C%d\n", curr->client.id, next->client.id);
}

static int mailbox_xfer(CPUState* state, PerCPU* cpu, Thread* curr, Thread* next, KObject_Mailbox* mailbox) {
    // the sender thread is now blocked
    transfer_time(cpu, curr, next, state);

    uint64_t send_info  = SYS_PARAM1;
    uint64_t send_msg   = SYS_PARAM4;
    KHandle send_handle = SYS_PARAM5;

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

        KHandle recv_handle = env_open_handle(next->parent, 0, obj);
        GET_PARAM5(&next->state) = recv_handle;
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

#undef SYS_PARAM0
#undef SYS_PARAM1
#undef SYS_PARAM2
#undef SYS_PARAM3
#undef SYS_PARAM4
#undef SYS_PARAM5
