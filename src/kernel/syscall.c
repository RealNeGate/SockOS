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
    arch_tlb_shootdown(env);

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
    kprintf("SYS_test(%p)\n", SYS_PARAM0);
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
    if (dev == NULL || dev->super.tag != KOBJECT_DEV_PCI) {
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
    // b.prefetch = (bar.value >> 3) & 0x1;

    kassert(type == 0 || type == 2, "TODO: Unsupported BAR (%d)", type);
    if (type == 2) {
        // 64bit BAR
        addr |= ((uintptr_t) dev->bar[bar_index].value) << 32ull;
    }

    *((size_t*) SYS_PARAM2) = size;

    KObject_VMO* vmo_ptr = vmo_create_physical(addr, size);
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

    kprintf("%ld %ld %ld %ld %p\n", info[0], info[1], info[2], info[3], kaddr2paddr(boot_info->fb.pixels));

    KObject_VMO* vmo_ptr = vmo_create_physical(kaddr2paddr(boot_info->fb.pixels), info[3]);
    return env_open_handle(env, 0, &vmo_ptr->super);
}

SYS_FN(pci_read_config_32) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_pci_read_config_32(pci=%d, offset=%d)\n", SYS_PARAM0, SYS_PARAM1));

    Env* env = cpu->current_thread->parent;
    PCI_Device* dev = env_get_handle(env, SYS_PARAM0, NULL);
    if (dev == NULL || dev->super.tag != KOBJECT_DEV_PCI) {
        return 0;
    }

    return pci_read_u32(dev->bus, dev->device, dev->func, SYS_PARAM1);
}

SYS_FN(pci_write_config_32) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_pci_write_config_32(pci=%d, offset=%d, val=%d)\n", SYS_PARAM0, SYS_PARAM1, SYS_PARAM2));

    Env* env = cpu->current_thread->parent;
    PCI_Device* dev = env_get_handle(env, SYS_PARAM0, NULL);
    if (dev == NULL || dev->super.tag != KOBJECT_DEV_PCI) {
        return 0;
    }

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
    if (mailbox == NULL || mailbox->super.tag != KOBJECT_MAILBOX) {
        return -1;
    }

    if (SYS_PARAM1 > 5) {
        return -1;
    }

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
    if (mailbox == NULL || mailbox->super.tag != KOBJECT_MAILBOX) {
        return -1;
    }

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
    if (mailbox == NULL || mailbox->super.tag != KOBJECT_MAILBOX) {
        return -1;
    }

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

#undef SYS_PARAM0
#undef SYS_PARAM1
#undef SYS_PARAM2
#undef SYS_PARAM3
#undef SYS_PARAM4
#undef SYS_PARAM5
