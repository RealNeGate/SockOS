////////////////////////////////
// Syscall table
////////////////////////////////
#define SYS_FN(name) static PageTable* syscall_ ## name(CPUState* state, PageTable* old_address_space)
typedef PageTable* SyscallFn(CPUState* state, PageTable* old_address_space);

typedef enum {
    #define X(name, ...) SYS_ ## name,
    #include "syscall_table.h"

    SYS_MAX,
} SyscallNum;

// forward decls
#define X(name, ...) static PageTable* syscall_ ## name (CPUState*, PageTable*);
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
    kprintf("SYS_sleep(%p, %d us)\n", threads_current, SYS_PARAM0);

    sched_wait(threads_current, state->rdi);
    thread_yield();

    return old_address_space;
}

SYS_FN(exit_group) {
    Env* group = threads_current->parent;
    kprintf("SYS_exit_group(%p, %d)\n", group, SYS_PARAM0);

    env_kill(group);
    thread_yield();

    return old_address_space;
}

#undef SYS_PARAM0
#undef SYS_PARAM1
#undef SYS_PARAM2
#undef SYS_PARAM3
#undef SYS_PARAM4
#undef SYS_PARAM5
