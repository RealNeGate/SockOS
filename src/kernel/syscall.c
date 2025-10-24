#include <kernel.h>
#include "pci.h"

////////////////////////////////
// Syscall table
////////////////////////////////
#define SYS_FN(name) static uintptr_t syscall_ ## name(CPUState* state, uintptr_t cr3, PerCPU* cpu)
typedef uintptr_t SyscallFn(CPUState* state, uintptr_t cr3, PerCPU* cpu);

typedef enum {
    #define X(name, ...) SYS_ ## name,
    #include "syscall_table.h"

    SYS_MAX,
} SyscallNum;

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

SYS_FN(mmap) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_mmap(vmo=%p, offset=%d, size=%d)\n", SYS_PARAM0, SYS_PARAM1, SYS_PARAM2));

    size_t page_aligned_size = (SYS_PARAM2 + PAGE_SIZE - 1) & -PAGE_SIZE;
    if (page_aligned_size == 0) {
        return 0;
    } else {
        return vmem_map(cpu->current_thread->parent, SYS_PARAM0, SYS_PARAM1, page_aligned_size, VMEM_PAGE_WRITE | VMEM_PAGE_USER);
    }
}

SYS_FN(munmap) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_munmap()\n"));

    Env* env = cpu->current_thread->parent;

    // we can't have multiple writers on the interval tree at once
    // but we don't need a TLB shootdown until page writing.
    rwlock_lock_exclusive(&env->addr_space.lock);

    // shootdown forces all runners to flush, once
    // we do that we can recycle the pages.
    spall_begin_event("shootdown", cpu->core_id);
    arch_tlb_shootdown(env);
    spall_end_event(cpu->core_id);

    rwlock_unlock_exclusive(&env->addr_space.lock);

    return 0;
}

SYS_FN(thread_create) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_thread_create(fn=%d, arg=%p)\n", SYS_PARAM0, SYS_PARAM1));

    Env* env = cpu->current_thread->parent;
    uintptr_t stack_ptr = vmem_map(env, 0, 0, USER_STACK_SIZE, VMEM_PAGE_WRITE | VMEM_PAGE_USER);
    Thread* thread = thread_create(env, (ThreadEntryFn*) SYS_PARAM0, SYS_PARAM1, stack_ptr, USER_STACK_SIZE);

    if (thread == NULL) {
        return 0;
    } else {
        // make an accessible handle for the thread
        thread_resume(thread);
        return env_open_handle(env, 0, &thread->super);
    }
}

SYS_FN(test) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_test(%d)\n", SYS_PARAM0));
    return 0;
}

SYS_FN(pci_claim_device) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_pci_claim_device(count=%d, query=%p)\n", SYS_PARAM0, SYS_PARAM1));

    // TODO(NeGate): validate the array memory
    Env* env = cpu->current_thread->parent;
    uint32_t* arr = (uint32_t*) SYS_PARAM1;

    PCI_Device* found = NULL;
    FOR_N(i, 0, SYS_PARAM0) {
        FOR_N(j, 0, pci_dev_count) {
            PCI_Device* dev = pci_devs[j];
            u32 key = (dev->vendor_id << 16ull) | dev->device_id;
            // is within the filter list
            if (key != arr[i]) { continue; }
            // we can only claim a device if no one else has
            Env* env_null = NULL;
            if (atomic_compare_exchange_strong(&dev->parent, &env_null, env)) {
                found = dev;
                goto done;
            }
        }
    }

    done:
    if (found != NULL) {
        return env_open_handle(env, 0, &found->super);
    } else {
        return 0;
    }
}

SYS_FN(pci_bar_count) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_pci_bar_count(out_mask=%p)\n", SYS_PARAM0));

    Env* env = cpu->current_thread->parent;
    PCI_Device* dev = env_get_handle(env, SYS_PARAM0, NULL);
    if (dev->super.tag != KOBJECT_DEV_PCI) {
        return 0;
    }

    // mask of which BARs are memory
    u32 mask = 0;

    *((u32*) SYS_PARAM0) = mask;
    return dev->bar_count;
}

SYS_FN(pci_get_bar) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_pci_get_bar(pci=%d, bar=%d, out_size=%p)\n", SYS_PARAM0, SYS_PARAM1, SYS_PARAM2));

    Env* env = cpu->current_thread->parent;
    PCI_Device* dev = env_get_handle(env, SYS_PARAM0, NULL);
    if (dev->super.tag != KOBJECT_DEV_PCI) {
        return 0;
    }

    int bar_index = SYS_PARAM1;
    if (bar_index >= dev->bar_count) {
        return 0;
    }

    Raw_BAR* bar = &dev->bar[bar_index];

    // I/O ports can't be mapped in
    if (bar->value & 0x1) {
        return 0; // (bar.value >> 2) << 2;
    }

    size_t size = ~bar->size + 1;
    uintptr_t addr = (bar->value >> 4) << 4;

    u8 type = (bar->value >> 1) & 0x3;
    kassert(type == 0, "TODO: Only supports BAR 32-bit");
    // b.prefetch = (bar.value >> 3) & 0x1;

    *((size_t*) SYS_PARAM2) = size;

    KObject_VMO* vmo_ptr = vmo_create_physical(addr, size);
    return env_open_handle(env, 0, &vmo_ptr->super);
}

SYS_FN(mailbox_create) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_mailbox_create(stack_size=%d, max_rqs=%d, handler=%p)\n", SYS_PARAM0, SYS_PARAM1, SYS_PARAM2));

    Env* env = cpu->current_thread->parent;

    size_t stack_stride = (SYS_PARAM0 + PAGE_SIZE);
    size_t stack_pool_size = stack_stride*SYS_PARAM1;
    uintptr_t stack_ptr = vmem_map(env, 0, 0, stack_pool_size, VMEM_PAGE_WRITE | VMEM_PAGE_USER);
    if (stack_ptr == 0) {
        return 0;
    }

    KObject_Mailbox* mailbox = mailbox_create(SYS_PARAM1);
    if (mailbox == NULL) {
        return 0;
    }

    mailbox->handler_pc = (void*) SYS_PARAM2;
    FOR_N(i, 0, SYS_PARAM1) {
        uintptr_t sp = stack_ptr + stack_stride*i;
        Thread* thread = thread_create(env, (ThreadEntryFn*) SYS_PARAM0, SYS_PARAM1, sp, stack_stride);
        kassert(thread, "BAD BAD TODO");

        thread->saved_sp = thread->state.rsp;
        thread->wait_obj = mailbox;
        mailbox_recv(mailbox, thread);
    }

    return env_open_handle(env, 0, &mailbox->super);
}

SYS_FN(mailbox_send) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_mailbox_send(mailbox=%d, %d)\n", SYS_PARAM0, SYS_PARAM1));

    Env* env = cpu->current_thread->parent;
    Thread* curr = cpu->current_thread;

    KObject_Mailbox* mailbox = env_get_handle(env, SYS_PARAM0, NULL);
    Thread* next = mailbox_send(mailbox);

    u64 now_time = __rdtsc() / boot_info->tsc_freq;
    u64 delta = now_time - curr->start_time;
    u64 stolen_time = curr->max_exec_time - delta;
    if (delta >= curr->max_exec_time) {
        stolen_time = 1;
    }

    #ifdef __x86_64__
    next->state.rsp = next->saved_sp;
    next->state.rip = (u64) mailbox->handler_pc;
    next->state.rdi = SYS_PARAM1;
    next->state.rsi = SYS_PARAM2;
    next->state.rdx = SYS_PARAM3;
    next->state.r10 = SYS_PARAM4;
    next->state.r8  = SYS_PARAM5;
    #else
    #error "TODO"
    #endif

    next->max_exec_time += stolen_time;
    next->wait_obj = NULL;
    next->calling_thread = curr;

    // TODO(NeGate): force the mailbox thread to run next... it really should be an "always" thing.
    curr->wait_obj = mailbox;
    thread_resume(next);

    // Pick a new task
    state->interrupt_num = 32;
    uintptr_t new_cr3 = x86_irq_int_handler(state, cr3, cpu);
    do_context_switch(state, new_cr3);
    return 0;
}

SYS_FN(mailbox_yield) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_mailbox_yield(handle=%d, ret=%d)\n", SYS_PARAM0, SYS_PARAM1));

    Env* env = cpu->current_thread->parent;
    Thread* curr = cpu->current_thread;
    Thread* next = curr->calling_thread;
    curr->calling_thread = NULL;

    #ifdef __x86_64__
    next->state.rax = SYS_PARAM1;
    #else
    #error "TODO"
    #endif

    // Wait on mailbox now
    KObject_Mailbox* mailbox = env_get_handle(env, SYS_PARAM0, NULL);
    next->wait_obj = NULL;
    curr->wait_obj = mailbox;
    mailbox_recv(mailbox, curr);
    thread_resume(next);

    // Pick a new task
    state->interrupt_num = 32;
    uintptr_t new_cr3 = x86_irq_int_handler(state, cr3, cpu);
    do_context_switch(state, new_cr3);
    return 0;
}

#undef SYS_PARAM0
#undef SYS_PARAM1
#undef SYS_PARAM2
#undef SYS_PARAM3
#undef SYS_PARAM4
#undef SYS_PARAM5
