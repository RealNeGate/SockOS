#include "threads.h"

#define SCHED_IMPL
#define SCHED_ASSERT(x) kassert(x, "BAD SCHED!")
#include "scheduler.h"

enum {
    SCHED_MIN_QUANTA =  1000, // 1ms
    SCHED_LATENCY    = 10000, // 100Hz
};

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
    /* if (0 && cpu == &boot_info->cores[0]) {
        kprintf("Init weight matrix!!!\n");
        FOR_N(j, 0, boot_info->core_count) {
            printf("[");
            FOR_N(i, 0, boot_info->core_count) {
                int dist = distance_to_core(i, j);
                printf(" %d", dist);
            }
            printf(" ]\n");
        }
    } */
}

uint64_t sched_total_exec_time(PerCPU* cpu, uint64_t now_time) {
    return 0;
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

            kprintf("] latency = %10luus, %lu (%3d.%.1d%%)\n", lat, delta, percent / 10, percent % 10);

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

    uint64_t now_time = arch_get_micros();
    if (timeout == 0) {
        // give up rest of time slice
        t->client.wake_time = 1;
    } else {
        t->client.wake_time = now_time + timeout;
    }
}

Thread* sched_pick_next(PerCPU* cpu, uint64_t now_time, uint64_t* restrict out_wake_us) {
    Thread* curr = cpu->current_thread;
    cpu->sched.curr = curr ? &curr->client : NULL;

    Client* c = sched_pick_client(&cpu->sched, now_time, out_wake_us);
    if (c == NULL) {
        // kprintf("A ___ %ld\n", *out_wake_us - now_time);
        return NULL;
    }

    // convert client to thread
    ptrdiff_t dist_to_base = offsetof(Thread, client);
    Thread* t = (Thread*) (((char*) c) - dist_to_base);
    // kprintf("A %32s C%d %ld\n", t->tag, t->client.id, *out_wake_us - now_time);
    return t;
}

