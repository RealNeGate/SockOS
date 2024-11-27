#ifndef IMPL
typedef struct {
    Thread *curr, *first, *last;
} ThreadQueue;

struct PerCPU_Scheduler {
    ThreadQueue active;
    ThreadQueue waiters;
};

static void tq_append(ThreadQueue* tq, Thread* t);
#else
static Thread* tq_peek(ThreadQueue* tq) {
    return tq->curr;
}

static void tq_remove(ThreadQueue* tq, Thread* t) {
    Thread* next = t->next_sched;
    if (tq->first == t) { tq->first = next; }
    if (tq->last == t) { tq->last = t->prev_sched; }
    t->prev_sched->next_sched = next;
    if (next != NULL) {
        next->prev_sched = t->prev_sched;
    }
}

static void tq_insert_after(ThreadQueue* tq, Thread* at, Thread* t) {
    t->prev_sched = at;
    t->next_sched = at->next_sched;
    at->next_sched->prev_sched = t;
    at->next_sched = t;
}

static void tq_append(ThreadQueue* tq, Thread* t) {
    t->next_sched = NULL;

    if (tq->first == NULL) {
        t->prev_sched = NULL;
        tq->first = tq->last = t;
    } else {
        t->prev_sched = tq->last;
        tq->last->next_sched = t;
        tq->last = t;
    }
}

static Thread* tq_advance(ThreadQueue* tq) {
    Thread* next = tq->curr->next_sched;
    if (next == NULL) {
        // wrap around
        next = tq->first;
    }
    return (tq->curr = next);
}

#define kprintf
void sched_wait(Thread* t, u64 timeout) {
    // PerCPU_Scheduler* sched = get_sched();
    PerCPU_Scheduler* sched = boot_info->cores[0].sched;
    kassert(sched->active.curr == t, "wtf?");

    uint64_t now_time = __rdtsc() / boot_info->tsc_freq;

    // TODO(NeGate): there's potential logic & overflow bugs if now_time exceeds end_time
    kprintf("sched_wait(now=%d, timeout=%d):\n", now_time, timeout);
    if (timeout == 0) {
        // give up rest of time slice
        t->wake_time = t->end_time;
        t->wait_time = t->end_time - now_time;
    } else {
        uint64_t wake_time = now_time + timeout;
        t->wake_time = wake_time;

        // how much time of our slice was just consumed by waiting, the more time
        // given up the higher priority we are for waking up.
        uint64_t end_of_slice_sleep = (wake_time > t->end_time ? t->end_time : wake_time);
        if (end_of_slice_sleep <= now_time) {
            t->wait_time = 0;
        } else {
            t->wait_time = end_of_slice_sleep - now_time;
        }
        kprintf("  Thread-%p gave up %d us\n", t, t->wait_time);
    }

    t->start_time = t->end_time = 0;
    tq_advance(&sched->active);

    // remove ourselves from the active list
    tq_remove(&sched->active, t);
    tq_append(&sched->waiters, t);

    // migrate to wait list (highest wait_time first)
    /* if (sched->waiters == NULL || sched->waiters->wait_time < t->wait_time) {
        t->next_sched  = sched->waiters;
        sched->waiters = t;
    } else {
        Thread* other = sched->waiters;
        while (other != NULL) {
            Thread* next = other->next_sched;
            if (next == NULL || t->wait_time > next->wait_time) {
                other->next_sched = t;
                t->next_sched = next;
                break;
            }
            other = next;
        }
    } */
}

// We need this function to behave in a relatively fast and bounded fashion, it's generally called
// in a context switch interrupt after all.
Thread* sched_try_switch(u64 now_time, u64* restrict out_wake_us) {
    PerCPU_Scheduler* sched = get_sched();
    kprintf("sched_try_switch(now=%d):\n", now_time);

    Thread* active = tq_peek(&sched->active);
    kprintf("  active = Thread-%p {", active);
    for (Thread* t = sched->active.first; t; t = t->next_sched) {
        kprintf("  Thread-%p", t);
    }
    kprintf("  }\n");

    if (active != NULL) {
        kprintf("  end_time = %d\n", active->end_time);

        // allocate same quanta for each active item
        if (active->start_time == 0) {
            kprintf("  allot %d micros to Thread-%p\n", SCHED_QUANTA, active);

            active->start_time = now_time;
            active->end_time = now_time + SCHED_QUANTA;
        }

        // move along to the next task
        if (now_time > active->end_time) {
            // if there's completed waiters, we should try to context switch to
            // them before the rest of the tasks. this is because waiting would
            // have required them to have given up their time slice earlier on.
            Thread* next_waiter = sched->waiters.first;
            if (next_waiter && (next_waiter->wake_time == 0 || next_waiter->wake_time > now_time)) {
                kprintf("  Thread-%p is awake now\n", next_waiter, next_waiter->wait_time);

                tq_remove(&sched->waiters, next_waiter);
                tq_append(&sched->active,  next_waiter);

                // reset wait/wake info
                next_waiter->wake_time = 0;
                next_waiter->wait_time = 0;
            }

            // reset start & end
            active->start_time = active->end_time = 0;

            active = tq_advance(&sched->active);
            kprintf("  advance to Thread-%p\n", active);

            // schedule next task
            active->start_time = now_time;
            active->end_time = now_time + SCHED_QUANTA;
        }

        *out_wake_us = active->end_time;
        return active;
    } else {
        // idle until the next sleeper to wake up (10 seconds is the max sleep we'll do)
        uint64_t next_wake = UINT64_MAX;
        for (Thread* t = sched->waiters.first; t; t = t->next_sched) {
            if (next_wake > t->wake_time) {
                next_wake = t->wake_time;
            }
        }

        kprintf("  sleep until t=%d\n", next_wake);
        *out_wake_us = next_wake;
        return NULL;
    }
}
#undef kprintf
#endif

