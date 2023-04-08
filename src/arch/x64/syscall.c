
static PageTable* SYS_exit_group(CPUState* state, PageTable* old_address_space) {
    Environment* group = threads_current->parent;
    kprintf("SYS_exit_group(%p, %d)\n", group, state->rdi);

    env_kill(group);
    asm volatile("int $32"); // context switch interrupt

    // Thread* next = threads_try_switch();
    // kassert(next, "Halt i guess?");

    return old_address_space;
}

////////////////////////////////
// Syscall table
////////////////////////////////
typedef PageTable* SyscallFn(CPUState* state, PageTable* old_address_space);
SyscallFn* syscall_table[] = {
    [231] = SYS_exit_group,
};
size_t syscall_table_count = sizeof(syscall_table) / sizeof(syscall_table[0]);
