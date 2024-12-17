#include <kernel.h>
#include <elf.h>

#ifdef __x86_64__
#include "../arch/x64/x64.h"
#endif

struct Thread {
    KObject super; // tag = KOBJECT_THREAD

    Env* parent;
    Thread* prev_in_env;
    Thread* next_in_env;

    // Scheduler info
    u64 exec_time;
    u64 max_exec_time;
    u64 start_time;
    u64 wake_time;

    CPUState state;

    // Used for syscalls:
    //   on x64 it's hooked to kernel GS base
    //   when a userland app is running.
    uintptr_t kstack_addr;
};

