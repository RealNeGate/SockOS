#include <kernel.h>
#include <beans.h>

#ifdef __x86_64__
#include "../arch/x64/x64.h"
#endif

#include "scheduler.h"

struct Thread {
    KObject super; // tag = KOBJECT_THREAD

    Env* parent;
    Thread* prev_in_env;
    Thread* next_in_env;

    // Scheduler info
    Client client;

    // Last core that this thread ran on
    int core_id;

    // Wait-list info
    Thread* next_in_wait;
    // waiting on signalling objects
    _Atomic(void*) wait_obj;

    // In the environment's address space
    uintptr_t utcb_addr;

    // Address space optimization, we track the last faulted address.
    struct {
        uintptr_t base_addr;
        uintptr_t next_addr;
    } last_touch;

    char tag[32];

    // segment regs
    #ifdef __x86_64__
    uintptr_t fs_base;
    uintptr_t gs_base;
    #endif

    _Alignas(16) CPUState state;
};

bool sched_is_blocked(Thread* t);


