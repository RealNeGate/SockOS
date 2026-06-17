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

static int distance_to_core(int a, int b) {
    if (a == b) {
        return 0;
    }
    return 32 - __builtin_clz(a ^ b);
}

static int* sched_weight_matrix;
void sched_init(void) {
    PerCPU* cpu = cpu_get();
    cpu->sched = kheap_zalloc(sizeof(PerCPU_Scheduler));

    if (0 && cpu == &boot_info->cores[0]) {
        kprintf("Init weight matrix!!!\n");
        FOR_N(j, 0, boot_info->core_count) {
            printf("[");
            FOR_N(i, 0, boot_info->core_count) {
                int dist = distance_to_core(i, j);
                printf(" %d", dist);
            }
            printf(" ]\n");
        }
    }
}

u64 sched_total_exec_time(PerCPU* cpu, u64 now_time) {
    u64 exec_time = atomic_load_explicit(&cpu->sched->total_exec_time, memory_order_relaxed);
    /* Thread* curr = cpu->current_thread;
    if (curr != NULL) {
        exec_time += now_time - curr->start_time;
    } */
    return exec_time;
}

int sched_load_balancer(void* arg) {
    uint64_t last_exec[256];
    uint64_t total_exec[256];
    uint64_t usage[256];
    uint64_t last_time = 0;
    for (;;) {
        sleep(1000000);

        uint64_t now_time = __rdtsc() / boot_info->tsc_freq;
        FOR_N(i, 0, boot_info->core_count) {
            PerCPU* some_cpu = &boot_info->cores[i];
            total_exec[i] = sched_total_exec_time(some_cpu, now_time);
        }

        uint64_t delta = now_time - last_time;
        if (last_time == 0) {
            FOR_N(i, 0, 4) {
                last_exec[i] = total_exec[i];
            }
        } else {
            kprintf("[");

            FOR_N(i, 0, boot_info->core_count) {
                uint64_t active = last_time ? total_exec[i] - last_exec[i] : 0;
                last_exec[i] = total_exec[i];

                uint64_t percent = (active * 1000) / delta;
                usage[i] = percent;

                kprintf(" %3lu.%1lu%%", percent / 10, percent % 10);
            }

            int lat = delta - 1000000;
            int percent = (lat * 1000) / 1000000;

            kprintf("] latency = %10luus (%3d.%.2d%%)\n", lat, percent / 10, percent % 10);

            // pick highest task, move to lowest usage core
            /*int lowest = 0, highest = 0;
            FOR_N(i, 1, boot_info->core_count) {
                if (usage[lowest]  > usage[i]) { lowest = i;  }
                if (usage[highest] < usage[i]) { highest = i; }
            }

            int gap = usage[highest] - usage[lowest];
            if (highest != lowest && gap > 10) {
                kprintf("SHOULD MIGRATE FROM %d -> %d (gap %d)\n", highest, lowest, gap);

                PerCPU* high_cpu  = &boot_info->cores[highest];
                Thread* high_task = high_cpu->current_thread;

                int exp = -1;
                if (atomic_compare_exchange_strong(&high_task->to_be_migrated, &exp, lowest)) {
                    kprintf("Migrating Thread-%p!!!\n", high_task);
                } else {
                    kprintf("Can't migrate Thread-%p!!! %d\n", high_task, exp);
                }
            }*/
        }
        last_time = now_time;
    }
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

enum {
    SCHED_MIN_QUANTA =  1000,
    SCHED_LATENCY    = 16666, // 60Hz
};

bool sched_is_blocked(Thread* t) {
    return t->wake_time != 0 || atomic_load_explicit(&t->wait_obj, memory_order_relaxed);
}

Thread* sched_pick_next(PerCPU* cpu, u64 now_time, u64* restrict out_wake_us) {
    int core_id = cpu - boot_info->cores;
    PerCPU_Scheduler* sched = cpu->sched;

    // kprintf("sched_pick_next(now=%d, %d)\n", now_time, sched->active.count);

    // We run this function when pre-emptions happen so we should check up on the last
    // running task.
    Thread* curr = cpu->current_thread;
    if (curr != NULL) {
        u64 delta = now_time - curr->start_time;

        // update total_exec_time
        atomic_fetch_add_explicit(&sched->total_exec_time, delta, memory_order_relaxed);

        // update exec_time
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

            // if the thread has completed it's time slice, we need to re-insert
            // into the queue with the new exec_time.
            ON_DEBUG(SCHED)(kprintf("[sched] CPU-%d: Thread-%p has stopped (max %lu us)\n", core_id, curr, curr->max_exec_time));
            curr->status = THREAD_STATE_READY;
            runqueue_insert(&sched->active, curr);
        } else {
            curr->status = THREAD_STATE_BLOCKED;
            curr->exec_time -= sched->min_exec_time;
            ON_DEBUG(SCHED)(kprintf("[sched] CPU-%d: Thread-%p is blocked after %lu us\n", core_id, curr, curr->exec_time));

            if (curr->wake_time != 0) {
                wakequeue_insert(&cpu->sched->waiters, curr);
            }
        }
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
    curr = runqueue_pop(&sched->active);
    curr->core_id = core_id;
    curr->status = THREAD_STATE_RUNNING;

    u64 exec_time = curr->exec_time - sched->min_exec_time;
    if (sched->min_exec_time < curr->exec_time) {
        sched->min_exec_time = curr->exec_time;
    }

    // allocate time slice
    curr->start_time = now_time;
    if (sched->ideal_exec_time > exec_time) {
        curr->max_exec_time = sched->ideal_exec_time - exec_time;
    } else {
        curr->max_exec_time = 1;
    }

    ON_DEBUG(SCHED)(kprintf("[sched] CPU-%d: Thread-%p was alloted %lu us (exec time = %lu)\n", core_id, curr, curr->max_exec_time, exec_time));
    *out_wake_us = now_time + curr->max_exec_time;
    return curr;
}

