#include "threads.h"

// wait queue

void tq_append(ThreadQueue* tq, Thread* t) {
    tq->data[tq->tail] = t;
    tq->tail = (tq->tail + 1) % 256;
}

void tq_dump(ThreadQueue* tq, bool is_waiter, u64 min_time) {
    kprintf("  %s: [ %d %d : ", is_waiter ? "Wait" : "Act", tq->head, tq->tail);
    for (int i = tq->head; i != tq->tail; i = (i + 1) % 256) {
        Thread* t = tq->data[i];
        kprintf("Thread-%p (%d) ", t, t->exec_time - min_time);
    }
    kprintf("]\n");
}

void tq_insert(ThreadQueue* tq, Thread* t, bool is_waiter) {
    /*if (!is_waiter) {
        kprintf("  BEFORE(%p):\n", t); tq_dump(tq, is_waiter);
    }*/

    // shift up
    int j = tq->tail;
    if (j < tq->head) { j += 256; }

    if (is_waiter) { // sort by wake time
        while (j-- > tq->head && tq->data[j % 256]->wake_time > t->wake_time) {
            tq->data[(j + 1) % 256] = tq->data[j % 256];
        }
    } else {
        while (j-- > tq->head && tq->data[j % 256]->exec_time > t->exec_time) {
            tq->data[(j + 1) % 256] = tq->data[j % 256];
        }
    }

    tq->tail = (tq->tail + 1) % 256;
    tq->data[(j + 1) % 256] = t;

    /*if (!is_waiter) {
        kprintf("  AFTER:\n"); tq_dump(tq, is_waiter);
    }*/
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

    uint64_t now_time = __rdtsc() / boot_info->tsc_freq;
    // kprintf("sched_wait(now=%d, timeout=%d): %p\n", now_time, timeout, t);

    // TODO(NeGate): there's potential logic & overflow bugs if now_time exceeds end_time
    if (timeout == 0) {
        // give up rest of time slice
        t->wake_time = 1;
    } else {
        uint64_t wake_time = now_time + timeout;
        t->wake_time = wake_time;
    }

    tq_insert(&cpu->sched->waiters, t, true);
}

// keeps moving tasks to try to keep the exec times near each other
#if 0
int sched_load_balancer(void*) {
    static i64 total_exec[256];
    for (;;) {
        // find average
        u64 avg = 0;
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
        }

        sleep(100000);
    }
}
#endif

enum {
    SCHED_MIN_QUANTA =  1000,
    SCHED_LATENCY    = 16666, // 60Hz
};

bool sched_is_blocked(Thread* t) {
    // We're asleep
    if (t->wake_time != 0) {
        return true;
    }

    // Our address space has been locked
    if (t->parent) {
        Thread* tlb_lock = atomic_load_explicit(&t->parent->addr_space.tlb_lock, memory_order_acquire);
        if (tlb_lock && t != tlb_lock) {
            ON_DEBUG(SCHED)(kprintf("[sched] shot down! Thread-%p\n", t));
            return true;
        }
    }

    return false;
}

Thread* sched_pick_next(PerCPU* cpu, u64 now_time, u64* restrict out_wake_us) {
    int core_id = cpu->core_id;
    PerCPU_Scheduler* sched = cpu->sched;

    // kprintf("sched_pick_next(now=%d)\n", now_time);

    // We run this function when pre-emptions happen so we should check up on the last
    // running task.
    Thread* curr = cpu->current_thread;
    if (curr != NULL) {
        kassert(curr == sched->active.data[sched->active.head], "bad %p", curr);
        u64 delta = now_time - curr->start_time;

        // update exec_time, higher priorities
        curr->exec_time += delta;
        curr->start_time = now_time;

        // ON_DEBUG(SCHED)(kprintf("[sched] CPU-%d: Thread-%p got %f ms (exec_time = %f ms)\n", cpu->core_id, curr, delta / 1000.0f, curr->exec_time / 1000.0f));

        u64 exec_time = curr->exec_time - sched->min_exec_time;
        if (!sched_is_blocked(curr)) {
            // if the task hasn't finished it's time slice, we just keep running it
            if (exec_time < curr->max_exec_time) {
                // ON_DEBUG(SCHED)(kprintf("[sched] CPU-%d: Keep running Thread-%p for another %f ms\n", cpu->core_id, curr, (curr->max_exec_time - exec_time) / 1000.0f));
                *out_wake_us = now_time + (curr->max_exec_time - exec_time);
                return curr;
            }

            // remove curr
            sched->active.head = (sched->active.head + 1) % 256;

            // if the thread has completed it's time slice, we need to re-insert
            // into the queue with the new exec_time.
            ON_DEBUG(SCHED)(kprintf("[sched] CPU-%d: Thread-%p has stopped (max %f ms)\n", cpu->core_id, curr, curr->max_exec_time / 1000.0f));
            tq_insert(&sched->active, curr, false);
        } else {
            // remove curr
            curr->active = false;
            curr->exec_time -= sched->min_exec_time;
            sched->active.head = (sched->active.head + 1) % 256;

            ON_DEBUG(SCHED)(kprintf("[sched] CPU-%d: Thread-%p is blocked after %f ms\n", core_id, curr, curr->exec_time / 1000.0f));
        }
    }

    // wake up sleepy guys
    while (sched->waiters.head != sched->waiters.tail) {
        Thread* waiter = sched->waiters.data[sched->waiters.head];
        if (now_time < waiter->wake_time) {
            break;
        }

        ON_DEBUG(SCHED)(kprintf("[sched] CPU-%d: Thread-%p is awake now (exec time was %d us)\n", core_id, waiter, waiter->exec_time));
        sched->waiters.head = (sched->waiters.head + 1) % 256;

        waiter->exec_time += sched->min_exec_time;
        waiter->wake_time = 0;
        waiter->active = false;
        tq_insert(&sched->active, waiter, false);
    }

    // recompute scheduling period
    if (1) {
        int active_count = sched->active.tail;
        if (active_count < sched->active.head) {
            active_count += 256;
        }
        active_count -= sched->active.head;

        if (active_count == 0) {
            sched->ideal_exec_time = 0;
            ON_DEBUG(SCHED)(kprintf("[sched] CPU-%d: no tasks, no slice\n", core_id));
        } else {
            // N is the number of active threads we can have before
            // resorting to period stretching.
            int N = SCHED_LATENCY / SCHED_MIN_QUANTA;
            u64 ideal = SCHED_LATENCY / active_count;

            sched->ideal_exec_time = ideal < SCHED_MIN_QUANTA ? SCHED_MIN_QUANTA : ideal;
            ON_DEBUG(SCHED)(kprintf("[sched] CPU-%d: ideal slice of %f ms (%d active)\n", core_id, sched->ideal_exec_time / 1000.0f, active_count));
        }
    }

    // if there's no active tasks, we just wait on the next sleeper
    if (sched->active.head == sched->active.tail) {
        if (sched->waiters.head != sched->waiters.tail) {
            Thread* waiter = sched->waiters.data[sched->waiters.head];

            ON_DEBUG(SCHED)(kprintf("[sched] CPU-%d: sleep for %f ms\n", core_id, (waiter->wake_time - now_time) / 1000.0f));
            *out_wake_us = waiter->wake_time;
            return NULL;
        } else {
            *out_wake_us = UINT64_MAX;
            return NULL;
        }
    }

    // tq_dump(&sched->active, false, sched->min_exec_time);

    // use leftmost thread
    curr = sched->active.data[sched->active.head];
    curr->core_id = core_id;

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
    if (sched->waiters.head != sched->waiters.tail) {
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

    ON_DEBUG(SCHED)(kprintf("[sched] CPU-%d: Thread-%p was alloted %f ms (exec time = %f)\n", core_id, curr, curr->max_exec_time / 1000.0f, exec_time / 1000.0f));
    *out_wake_us = now_time + curr->max_exec_time;
    return curr;
}

