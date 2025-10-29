#include <kernel.h>
#include <elf.h>

#ifdef __x86_64__
#include "../arch/x64/x64.h"
#endif

typedef enum ThreadState {
    // just spawned, threads must be told to run before they actually can
    THREAD_STATE_CREATION,

    // we can run, we're in the blocked list rn
    THREAD_STATE_READY,

    // waiting on a timer/mutex
    THREAD_STATE_BLOCKED,

    // we're in the active list... being active
    THREAD_STATE_RUNNING,
} ThreadState;

struct Thread {
    KObject super; // tag = KOBJECT_THREAD

    Env* parent;
    Thread* prev_in_env;
    Thread* next_in_env;

    // part of a core's list of was-just-blocked threads
    Thread* next_in_blocked;

    // Scheduler info
    u64 exec_time;
    u64 max_exec_time;
    u64 start_time;
    u64 wake_time;

    // Last core that this thread ran on
    int core_id;

    ThreadState status;

    // waiting on signalling objects
    _Atomic(void*) wait_obj;

    // Address space optimization, we track the last faulted address.
    // If we keep faulting in an array like
    struct {
        uintptr_t base_addr;
        uintptr_t next_addr;
    } last_touch;

    // Mailbox threads need to notify their calling thread
    Thread* calling_thread;
    uintptr_t saved_sp;

    CPUState state;
};

bool sched_is_blocked(Thread* t);

