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

SYS_FN(thread_create) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_thread_create(fn=%d, arg=%p)\n", SYS_PARAM0, SYS_PARAM1));

    Env* env = cpu->current_thread->parent;
    uintptr_t stack_ptr = vmem_map(env, 0, 0, USER_STACK_SIZE, VMEM_PAGE_WRITE | VMEM_PAGE_USER);
    Thread* thread = thread_create(env, (ThreadEntryFn*) SYS_PARAM0, SYS_PARAM1, stack_ptr, 16384);

    if (thread == NULL) {
        return 0;
    } else {
        // make an accessible handle for the thread
        return env_open_handle(env, 0, &thread->super);
    }
}

SYS_FN(test) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_test()\n"));
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
        kprintf("CLAIMED!!\n");
        pci_print_device(found);

        return env_open_handle(env, 0, &found->super);
    } else {
        return 0;
    }
}

#undef SYS_PARAM0
#undef SYS_PARAM1
#undef SYS_PARAM2
#undef SYS_PARAM3
#undef SYS_PARAM4
#undef SYS_PARAM5
