#include "threads.h"

static int runqueue_cmp(Thread* a, Thread* b) {
    return a->exec_time - b->exec_time;
}

static int wakequeue_cmp(Thread* a, Thread* b) {
    return a->wake_time - b->wake_time;
}

#define PRIO_ELEM     Thread*
#define PRIO_TYPE     ThreadQueue
#define PRIO_FN(name) runqueue_ ## name
#include "prio_queue.h"

#define PRIO_ELEM     Thread*
#define PRIO_TYPE     ThreadQueue
#define PRIO_FN(name) wakequeue_ ## name
#include "prio_queue.h"

void tq_dump(ThreadQueue* tq, bool is_waiter, u64 min_time) {
    kprintf("  %s: [ %d : ", is_waiter ? "Wait" : "Act", tq->count);
    for (int i = 0; i < tq->count; i++) {
        Thread* t = tq->data[i];
        kprintf("Thread-%p (%d) ", t, t->exec_time - min_time);
    }
    kprintf("]\n");
}

void sleep(u64 timeout) {
    sched_wait(timeout);
    sched_yield();
}

void sched_init(void) {
    PerCPU* cpu = cpu_get();
    cpu->sched = kheap_zalloc(sizeof(PerCPU_Scheduler));
}

void sched_wait(u64 timeout) {
    PerCPU* cpu = cpu_get();
    Thread* t = cpu->current_thread;

    // TODO(NeGate): there's potential logic & overflow bugs if now_time exceeds end_time
    uint64_t now_time = __rdtsc() / boot_info->tsc_freq;
    if (timeout == 0) {
        // give up rest of time slice
        t->wake_time = 1;
    } else {
        uint64_t wake_time = now_time + timeout;
        t->wake_time = wake_time;
    }
}

// keeps moving tasks to try to keep the exec times near each other
int sched_load_balancer(void*) {
    static i64 total_exec[256];
    for (;;) {
        // find average
        /*u64 avg = 0;
        FOR_N(i, 0, boot_info->core_count) {
            total_exec[i] = boot_info->cores[i].sched->total_exec;
        }
        FOR_N(i, 0, boot_info->core_count) {
            avg += total_exec[i];
        }
        boot_info->average_exec_time = avg / boot_info->core_count;
        ON_DEBUG(SCHED)(kprintf("[sched] Average CPU exec time: %d us\n", avg));

        if (avg > 1000) {
            FOR_N(i, 0, boot_info->core_count) {
                // if we're above average in exec times, we'll throw tasks into the
                // lowest exec core.
                if (total_exec[i] <= boot_info->average_exec_time) {
                    continue;
                }

                int overhead = boot_info->average_exec_time - total_exec[i];

                // round to min quanta
                overhead = (overhead + 999) / 1000 * 1000;

                ON_DEBUG(SCHED)(kprintf("[sched] CPU-%d: APLAPLPA %d\n", i, overhead));

                int lowest = -1;
                FOR_N(j, 0, boot_info->core_count) if (i != j) {
                    if (lowest < 0 || total_exec[j] < total_exec[lowest]) {
                        lowest = j;
                    }
                }

                // pluck highest task
                PerCPU_Scheduler* sched = boot_info->cores[i].sched;

                PerCPU_Scheduler* sched2 = boot_info->cores[i].sched;
                spin_lock(&sched->lock);

                spin_lock(&sched->lock);
            }
        }*/

        kprintf("POLL!\n");
        sleep(1000000);
    }
}

enum {
    SCHED_MIN_QUANTA =  1000,
    SCHED_LATENCY    = 16666, // 60Hz
};

bool sched_is_blocked(Thread* t) {
    return t->wake_time != 0 || atomic_load_explicit(&t->wait_obj, memory_order_relaxed);
}

u64 sched_total_exec_time(PerCPU* cpu, u64 now_time) {
    u64 exec_time = atomic_load_explicit(&cpu->sched->total_exec_time, memory_order_relaxed);
    Thread* curr = cpu->current_thread;
    if (curr != NULL) {
        exec_time += now_time - curr->start_time;
    }
    return exec_time;
}

Thread* sched_pick_next(PerCPU* cpu, u64 now_time, u64* restrict out_wake_us) {
    int core_id = cpu - boot_info->cores;
    PerCPU_Scheduler* sched = cpu->sched;

    // kprintf("sched_pick_next(now=%d)\n", now_time);
    // We run this function when pre-emptions happen so we should check up on the last
    // running task.
    Thread* curr = cpu->current_thread;
    if (curr != NULL) {
        kassert(curr == runqueue_peek(&sched->active), "bad %p %p", curr, runqueue_peek(&sched->active));
        u64 delta = now_time - curr->start_time;

        // update total_exec_time
        atomic_fetch_add_explicit(&cpu->sched->total_exec_time, delta, memory_order_relaxed);

        // update exec_time, higher priorities
        curr->exec_time += delta;
        curr->start_time = now_time;

        ON_DEBUG(SCHED)(kprintf("[sched] CPU-%d: Thread-%p got %lu us (exec_time = %lu us)\n", core_id, curr, delta, curr->exec_time));

        u64 exec_time = curr->exec_time - sched->min_exec_time;
        if (!sched_is_blocked(curr)) {
            // if the task hasn't finished it's time slice, we just keep running it
            if (exec_time < curr->max_exec_time) {
                // ON_DEBUG(SCHED)(kprintf("[sched] CPU-%d: Keep running Thread-%p for another %f ms\n", core_id, curr, (curr->max_exec_time - exec_time) / 1000.0f));
                *out_wake_us = now_time + (curr->max_exec_time - exec_time);
                return curr;
            }

            // remove curr
            runqueue_pop(&sched->active);

            // if the thread has completed it's time slice, we need to re-insert
            // into the queue with the new exec_time.
            ON_DEBUG(SCHED)(kprintf("[sched] CPU-%d: Thread-%p has stopped (max %lu us)\n", core_id, curr, curr->max_exec_time));
            runqueue_insert(&sched->active, curr);
        } else {
            // remove curr
            runqueue_pop(&sched->active);
            curr->exec_time -= sched->min_exec_time;
            ON_DEBUG(SCHED)(kprintf("[sched] CPU-%d: Thread-%p is blocked after %lu us\n", core_id, curr, curr->exec_time));

            if (curr->wake_time != 0) {
                wakequeue_insert(&cpu->sched->waiters, curr);
            }
        }
        curr->status = THREAD_STATE_READY;
    }

    // wake up sleepy guys
    while (sched->waiters.count > 0) {
        Thread* waiter = wakequeue_peek(&sched->waiters);
        if (now_time < waiter->wake_time) {
            break;
        }

        ON_DEBUG(SCHED)(kprintf("[sched] CPU-%d: Thread-%p is awake now (exec time was %d us)\n", core_id, waiter, waiter->exec_time));
        wakequeue_pop(&sched->waiters);
        waiter->wake_time = 0;
        waiter->exec_time += sched->min_exec_time;
        waiter->status = THREAD_STATE_READY;
        runqueue_insert(&sched->active, waiter);
    }

    // everything in this list should be ready to run, if not then we still remove it because that means it's
    // someone else's job to wake it up.
    Thread* blocked = atomic_exchange_explicit(&cpu->blocked_threads, NULL, memory_order_acq_rel);
    while (blocked) {
        ON_DEBUG(SCHED)(kprintf("[sched] CPU-%d: Thread-%p is unblocked now (exec time was %d us)\n", core_id, blocked, blocked->exec_time));
        blocked->exec_time += sched->min_exec_time;
        blocked->status = THREAD_STATE_READY;
        runqueue_insert(&sched->active, blocked);

        blocked = blocked->next_in_blocked;
    }

    // recompute scheduling period
    if (sched->active.count == 0) {
        sched->ideal_exec_time = 0;
        ON_DEBUG(SCHED)(kprintf("[sched] CPU-%d: no tasks, no slice\n", core_id));

        // if there's no active tasks, we just wait on the next sleeper
        if (sched->waiters.count > 0) {
            Thread* waiter = wakequeue_peek(&sched->waiters);

            ON_DEBUG(SCHED)(kprintf("[sched] CPU-%d: sleep for %lu us\n", core_id, (waiter->wake_time - now_time)));
            *out_wake_us = waiter->wake_time;
            return NULL;
        } else {
            *out_wake_us = UINT64_MAX;
            return NULL;
        }
    } else {
        // N is the number of active threads we can have before
        // resorting to period stretching.
        int N = SCHED_LATENCY / SCHED_MIN_QUANTA;
        u64 ideal = SCHED_LATENCY / sched->active.count;

        sched->ideal_exec_time = ideal < SCHED_MIN_QUANTA ? SCHED_MIN_QUANTA : ideal;
        ON_DEBUG(SCHED)(kprintf("[sched] CPU-%d: ideal slice of %lu us (%d active)\n", core_id, sched->ideal_exec_time, sched->active.count));
    }

    // use lowest exec time thread
    curr = runqueue_peek(&sched->active);
    curr->core_id = core_id;
    curr->status = THREAD_STATE_RUNNING;

    u64 exec_time = curr->exec_time - sched->min_exec_time;
    if (sched->min_exec_time < curr->exec_time) {
        sched->min_exec_time = curr->exec_time;
    }

    // ON_DEBUG(SCHED)(kprintf("[sched] CPU-%d: Thread-%p with lowest exec time (%f ms)\n", core_id, curr, exec_time / 1000.0f));

    // allocate time slice
    curr->start_time = now_time;
    if (sched->ideal_exec_time > exec_time) {
        curr->max_exec_time = sched->ideal_exec_time - exec_time;
    } else {
        curr->max_exec_time = 1;
    }

    // check the next timed wait, if there's something coming up which "deserves" the time more, we'll
    // truncate our slice and resolve them as quickly as possible.
    #if 0
    if (sched->waiters.count > 0) {
        Thread* waiter = sched->waiters.data[sched->waiters.head];
        kassert(now_time < waiter->wake_time, "how is it still in the wait queue? %d %d", now_time, waiter->wake_time);

        uint64_t time_until_wake = waiter->wake_time - now_time;

        // exec time if it ran until the pre-empt point
        uint64_t next_active  = (curr->exec_time + time_until_wake) - sched->min_exec_time;

        // exec time if it ran normally
        uint64_t next_active2 = (curr->exec_time + curr->max_exec_time) - sched->min_exec_time;

        ON_DEBUG(SCHED)(kprintf("[sched] CPU-%d: Thread-%p gonna wake in %f ms (waiter: %f ms vs active: %f ms vs active2: %f ms)\n", core_id, waiter, (waiter->wake_time - now_time) / 1000.0f, waiter->exec_time / 1000.0f, next_active / 1000.0f, next_active2 / 1000.0f));
    }
    #endif

    ON_DEBUG(SCHED)(kprintf("[sched] CPU-%d: Thread-%p was alloted %lu us (exec time = %lu)\n", core_id, curr, curr->max_exec_time, exec_time));
    *out_wake_us = now_time + curr->max_exec_time;
    return curr;
}

