#include <kernel.h>
#include <elf.h>

#ifdef __x86_64__
#include "../arch/x64/x64.h"
#endif

struct Thread {
    Env* parent;
    Thread* prev_in_env;
    Thread* next_in_env;

    Thread* prev_sched;
    Thread* next_sched;

    // active range
    u64 start_time, end_time;

    // how much of the quanta did we give up to waiting
    u64 wait_time;

    // sleeping
    u64 wake_time;

    CPUState state;
};
