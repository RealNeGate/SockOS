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

#define SYS_PARAM0 state->rdi
#define SYS_PARAM1 state->rsi
#define SYS_PARAM2 state->rdx
#define SYS_PARAM3 state->r10
#define SYS_PARAM4 state->r8
#define SYS_PARAM5 state->r9
#else
#error "TODO: Syscall parameters aren't available for this arch"
#endif

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

SYS_FN(pipe_create) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_pipe_create(capacity=%d)\n", SYS_PARAM0));
    Env* env = cpu->current_thread->parent;
    KObject_Pipe* pipe = pipe_create(SYS_PARAM0);
    return env_open_handle(env, 0, &pipe->super);
}

SYS_FN(pipe_send) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_pipe_send(pipe=%p, vmo=%p, offset=%p, size=%p)\n", SYS_PARAM0, SYS_PARAM1, SYS_PARAM2));
    Env* env = cpu->current_thread->parent;
    KObject_Pipe* pipe = env_get_handle(env, SYS_PARAM0, NULL);
    KCHECK(pipe, RESULT_NO_HANDLE);
    KCHECK(pipe->super.tag == KOBJECT_PIPE, RESULT_WRONG_HANDLE);

    KObject_VMO* vmo = env_get_handle(env, SYS_PARAM1, NULL);
    KCHECK(vmo, RESULT_NO_HANDLE);
    KCHECK(vmo->super.tag == KOBJECT_VMO, RESULT_WRONG_HANDLE);

    pipe_send(pipe, vmo, SYS_PARAM2, SYS_PARAM3);
    return 0;
}

SYS_FN(pipe_recv) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_pipe_recv(pipe=%p, offset=%p, size=%p)\n", SYS_PARAM0, SYS_PARAM1, SYS_PARAM2));
    Env* env = cpu->current_thread->parent;
    KObject_Pipe* pipe = env_get_handle(env, SYS_PARAM0, NULL);
    KCHECK(pipe, RESULT_NO_HANDLE);
    KCHECK(pipe->super.tag == KOBJECT_PIPE, RESULT_WRONG_HANDLE);

    uint64_t offset, size;
    KObject_VMO* vmo_ptr = pipe_recv(pipe, &offset, &size);

    egest_usermem(SYS_PARAM1, &offset, sizeof(uintptr_t));
    egest_usermem(SYS_PARAM2, &size,   sizeof(uintptr_t));
    return env_open_handle(env, 0, &vmo_ptr->super);
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

    kprintf("MEM DUMP %p\n", env);
    VMem_Cursor cursor = { env->addr_space.root, 0 };
    for (; cursor.node; cursor = vmem_cursor_next(cursor)) {
        VMem_PageDesc* desc = &cursor.node->vals[cursor.index];
        uintptr_t start_addr = cursor.node->keys[cursor.index];
        uintptr_t end_addr   = start_addr + desc->size;

        kprintf("[%p - %p] %d\n", start_addr, end_addr, desc->valid);
    }
    kprintf("\n");
    return 0;
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
    thread_resume(thread);
    return env_open_handle(env, 0, &thread->super);
}

SYS_FN(test) {
    kprintf("SYS_test(%p)\n", SYS_PARAM0);
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

SYS_FN(mailbox_send) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_mailbox_send(mailbox=%d, arg0=%d, arg1=%d, arg2=%d, arg3=%d, arg4=%d)\n", SYS_PARAM0, SYS_PARAM1, SYS_PARAM2, SYS_PARAM3, SYS_PARAM4, SYS_PARAM5));

    Env* env = cpu->current_thread->parent;
    KObject_Mailbox* mailbox = env_get_handle(env, SYS_PARAM0, NULL);
    KCHECK(mailbox, RESULT_NO_HANDLE);
    KCHECK(mailbox->super.tag == KOBJECT_MAILBOX, RESULT_WRONG_HANDLE);

    Thread* curr = cpu->current_thread;
    Thread* next = mailbox_send(mailbox);

    spin_lock(&cpu->sched->lock);
    // if we're switching, save old thread state
    curr->state = *state;

    // transfer our time
    next->start_time = curr->start_time;
    next->exec_time = curr->exec_time;
    next->max_exec_time = curr->max_exec_time;
    next->wait_obj = NULL;
    next->calling_thread = curr;
    // the sender thread is now blocked
    curr->wait_obj = mailbox;

    cpu->current_thread = next;
    cpu->sched->active.data[0] = next;
    spin_unlock(&cpu->sched->lock);

    kassert(next->parent, "mailboxes can't live in kernel-threads");
    arch_set_address_space(next->parent);

    // TODO(NeGate): verify address
    uint64_t* msg = (uint64_t*) next->state.rsi;

    // forward params
    #ifdef __x86_64__
    next->state.rax = SYS_PARAM1;
    msg[0] = SYS_PARAM2;
    msg[1] = SYS_PARAM3;
    msg[2] = SYS_PARAM4;
    msg[3] = SYS_PARAM5;
    #else
    #error "TODO"
    #endif

    do_context_switch(&next->state, 0);
}

SYS_FN(mailbox_wait) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_mailbox_wait(mailbox=%d, data=%p)\n", SYS_PARAM0, SYS_PARAM1));

    Env* env = cpu->current_thread->parent;
    Thread* curr = cpu->current_thread;
    KObject_Mailbox* mailbox = env_get_handle(env, SYS_PARAM0, NULL);
    KCHECK(mailbox, RESULT_NO_HANDLE);
    KCHECK(mailbox->super.tag == KOBJECT_MAILBOX, RESULT_WRONG_HANDLE);

    // Wait on mailbox
    spin_lock(&cpu->sched->lock);
    curr->wait_obj = mailbox;
    mailbox_recv(mailbox, curr);
    spin_unlock(&cpu->sched->lock);

    // Pick a new task
    state->interrupt_num = 32;
    uintptr_t new_cr3 = x86_irq_int_handler(state, cr3, cpu);
    do_context_switch(state, new_cr3);
}

SYS_FN(mailbox_reply) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_mailbox_reply(mailbox=%d, msg=%p, ret0=%d, ret1=%d)\n", SYS_PARAM0, SYS_PARAM1, SYS_PARAM2, SYS_PARAM3));
    Env* env = cpu->current_thread->parent;
    KObject_Mailbox* mailbox = env_get_handle(env, SYS_PARAM0, NULL);
    KCHECK(mailbox, RESULT_NO_HANDLE);
    KCHECK(mailbox->super.tag == KOBJECT_MAILBOX, RESULT_WRONG_HANDLE);

    Thread* curr = cpu->current_thread;
    Thread* next = curr->calling_thread;

    spin_lock(&cpu->sched->lock);
    // save old thread state
    curr->state = *state;
    // transfer our time
    next->start_time = curr->start_time;
    next->exec_time = curr->exec_time;
    next->max_exec_time = curr->max_exec_time;
    next->wait_obj = NULL;
    // the mailbox thread is now blocked
    curr->calling_thread = NULL;
    curr->wait_obj = mailbox;
    mailbox_recv(mailbox, curr);

    cpu->current_thread = next;
    cpu->sched->active.data[0] = next;
    spin_unlock(&cpu->sched->lock);

    // passthru the params
    #ifdef __x86_64__
    next->state.rax = SYS_PARAM2;
    next->state.rdx = SYS_PARAM3;
    #else
    #error "TODO"
    #endif

    kassert(next->parent, "mailboxes can't live in kernel-threads");
    uintptr_t new_cr3 = kaddr2paddr(next->parent->addr_space.hw_tables);
    do_context_switch(&next->state, new_cr3);
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
