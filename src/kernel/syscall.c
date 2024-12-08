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

    sched_wait(cpu->current_thread, SYS_PARAM0);
    state->interrupt_num = 32;
    uintptr_t new_cr3 = irq_int_handler(state, cr3, cpu);

    do_context_switch(state, new_cr3);
}

SYS_FN(mmap) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_mmap(paddr=%p, size=%d)\n", SYS_PARAM0, SYS_PARAM1));

    size_t page_aligned_size = (SYS_PARAM1 + PAGE_SIZE - 1) & -PAGE_SIZE;
    state->rax = vmem_alloc(&cpu->current_thread->parent->addr_space, page_aligned_size, SYS_PARAM0, VMEM_PAGE_READ | VMEM_PAGE_WRITE | VMEM_PAGE_USER);
    return cr3;
}

#undef SYS_PARAM0
#undef SYS_PARAM1
#undef SYS_PARAM2
#undef SYS_PARAM3
#undef SYS_PARAM4
#undef SYS_PARAM5
