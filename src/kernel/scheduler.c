#include "threads.h"

void tq_append(ThreadQueue* tq, Thread* t) {
    tq->data[tq->tail] = t;
    tq->tail = (tq->tail + 1) % 256;
}

void tq_insert(ThreadQueue* tq, Thread* t, bool is_waiter) {
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

    /* kprintf("%s: [ %d %d : ", is_waiter ? "Wait" : "Act", tq->head, tq->tail);
    for (int i = tq->head; i != tq->tail; i = (i + 1) % 256) {
        Thread* t = tq->data[i];
        kprintf("Thread-%p ", t);
    }
    kprintf("]\n"); */
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

    // TODO(NeGate): there's potential logic & overflow bugs if now_time exceeds end_time
    // kprintf("sched_wait(now=%d, timeout=%d): %p\n", now_time, timeout, t);
    if (timeout == 0) {
        // give up rest of time slice
        t->wake_time = 1;
    } else {
        uint64_t wake_time = now_time + timeout;
        t->wake_time = wake_time;
    }

    tq_insert(&cpu->sched->waiters, t, true);
}

// We need this function to behave in a relatively fast and bounded fashion, it's generally called
// in a context switch interrupt after all.
Thread* sched_try_switch(u64 now_time, u64* restrict out_wake_us) {
    PerCPU_Scheduler* sched = cpu_get()->sched;
    kprintf("sched_try_switch(now=%d)\n", now_time);

    // wake up sleepy guys
    while (sched->waiters.head != sched->waiters.tail) {
        Thread* waiter = sched->waiters.data[sched->waiters.head];
        if (now_time < waiter->wake_time) {
            break;
        }

        // kprintf("  Thread-%p is awake now\n", waiter);
        sched->waiters.head = (sched->waiters.head + 1) % 256;

        waiter->wake_time = 0;
        sched->total_exec += waiter->exec_time;
        tq_insert(&sched->active, waiter, false);
    }

    // recompute scheduling period
    if (1) {
        int active_count = sched->active.tail;
        if (active_count < sched->active.head) {
            active_count += 256;
        }
        active_count -= sched->active.head;

        int N = (SCHED_QUANTA / 1000);
        if (active_count > N) {
            sched->scheduling_period = 1000 * active_count;
        } else {
            sched->scheduling_period = SCHED_QUANTA;
        }

        sched->ideal_exec_time = active_count > 0 ? sched->scheduling_period / active_count : 0;
        // kprintf("SCHEDULING PERIOD OF %d us\n", sched->scheduling_period);
    }

    // if there's no active tasks, we just wait on the next sleeper
    if (sched->active.head == sched->active.tail) {
        if (sched->waiters.head != sched->waiters.tail) {
            Thread* waiter = sched->waiters.data[sched->waiters.head];

            // round to minimum quanta
            u64 wake_time = ((waiter->wake_time + 999) / 1000) * 1000;

            // kprintf("  sleep until %d\n", wake_time);
            *out_wake_us = wake_time;
            return NULL;
        } else {
            // kprintf("  sleep basically forever\n");
            *out_wake_us = now_time + 1000000;
            return NULL;
        }
    }

    // pop leftmost thread, reinsert with new weight
    Thread* active = sched->active.data[sched->active.head];
    sched->active.head = (sched->active.head + 1) % 256;
    sched->total_exec -= active->exec_time;

    // kprintf("  Thread-%p with lowest exec time (%d us)\n", active, active->exec_time);

    // allocate time slice
    active->start_time = now_time;
    if (sched->ideal_exec_time > active->exec_time) {
        active->max_exec_time = sched->ideal_exec_time - active->exec_time;
    } else {
        active->max_exec_time = 1;
    }

    // round to minimum quanta
    active->max_exec_time = ((active->max_exec_time + 999) / 1000) * 1000;

    kprintf("  alloting %d micros to Thread-%p\n", active->max_exec_time, active);
    *out_wake_us = now_time + active->max_exec_time;
    return active;
}
