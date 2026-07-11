#include <kernel.h>
#include "threads.h"

#if 0
void waitqueue_wait(WaitQueue* wq, Thread* t) {
    // Find top node, append to it
    Thread* top = atomic_ldacq(&wq->thread);
    Thread* next = NULL;
    while (top != NULL && !atomic_cas_acq_rel(&top->next, &next, t)) {
        top = next;
    }

    kassert(t->wait_obj == NULL && !t->client.is_blocked, "huh? %p", t->wait_obj);
    t->client.is_blocked = true;
    t->wait_obj = wq;
    t->next_in_wait = wq->thread;
}
#endif

void waitqueue_wait(WaitQueue* wq, Thread* t, void* wait_obj) {
    spin_lock(&wq->lock);
    // kprintf("%p: %s MUST WAIT!!! C%d\n", wq, t->tag, t->client.id);

    kassert(t->wait_obj == NULL && !t->client.is_blocked, "huh? %p", t->wait_obj);
    t->client.is_blocked = true;
    t->wait_obj = wait_obj;
    t->next_in_wait = wq->thread;
    wq->thread = t;

    spin_unlock(&wq->lock);
}

Thread* waitqueue_wake(WaitQueue* wq, PerCPU* cpu, void* wait_obj, bool resume) {
    spin_lock(&wq->lock);

    Thread* t = wq->thread;
    if (t == NULL) {
        spin_unlock(&wq->lock);
        return NULL;
    }

    kassert(t->wait_obj == wait_obj, "huh?");
    t->wait_obj = NULL;
    t->client.is_blocked = false;

    // Advance
    wq->thread = t->next_in_wait;
    t->next_in_wait = NULL;
    spin_unlock(&wq->lock);

    if (resume) {
        // Add to active list
        thread_resume(t, cpu);
    }
    return t;
}

void waitqueue_broadcast(WaitQueue* wq) {
}
