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

    // Mailbox threads need to notify their calling thread
    Thread* calling_thread;
    uintptr_t saved_sp;

    // In the environment's address space
    uintptr_t utcb_addr;

    // Address space optimization, we track the last faulted address.
    // If we keep faulting in an array like
    struct {
        uintptr_t base_addr;
        uintptr_t next_addr;
    } last_touch;

    char tag[32];

    _Alignas(16) CPUState state;
};

bool sched_is_blocked(Thread* t);


