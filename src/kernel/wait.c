#include <kernel.h>
#include "threads.h"

void waitqueue_wait(WaitQueue* wq, Thread* t) {
    spin_lock(&wq->lock);
    kprintf("%p: %p MUST WAIT!!!\n", wq, t);

    kassert(t->wait_obj == NULL, "huh? %p", t->wait_obj);
    t->wait_obj = wq;
    t->next_in_wait = wq->thread;
    wq->thread = t;

    spin_unlock(&wq->lock);
}

Thread* waitqueue_wake(WaitQueue* wq, PerCPU* cpu) {
    spin_lock(&wq->lock);

    Thread* t = wq->thread;
    if (t == NULL) {
        spin_unlock(&wq->lock);
        return NULL;
    }

    kassert(t->wait_obj == wq, "huh?");
    t->wait_obj = NULL;

    // Advance
    wq->thread = t->next_in_wait;
    t->next_in_wait = NULL;
    kprintf("%p: %p MUST WAKE!!!\n", wq, t);
    spin_unlock(&wq->lock);

    // Add to active list
    thread_resume(t, cpu);
    return t;
}

void waitqueue_broadcast(WaitQueue* wq) {
}
