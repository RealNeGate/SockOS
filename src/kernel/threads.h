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

    Thread* prev_sched;
    Thread* next_sched;

    u64 exec_time;
    u64 max_exec_time;

    // active range
    u64 start_time;

    // sleeping
    u64 wake_time;

    CPUState state;
};
