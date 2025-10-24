#include <kernel.h>
#include "threads.h"

WaitQueue* waitqueue_alloc(void) {
    WaitQueue* wq = kheap_zalloc(sizeof(WaitQueue));
    wq->tail = &wq->dummy;
    return wq;
}

void waitqueue_free(WaitQueue* wq) {
    kheap_free(wq);
}

void waitqueue_wait(WaitQueue* wq, Thread* t) {
    WaitNode* new_node = kheap_alloc(sizeof(WaitNode));
    new_node->next = NULL;
    new_node->thread = t;

    spin_lock(&wq->lock);
    wq->tail->next = new_node;
    wq->tail = new_node;
    spin_unlock(&wq->lock);
}

Thread* waitqueue_wake(WaitQueue* wq) {
    spin_lock(&wq->lock);
    // advance head, if we're the one to do it then
    // we wake up the thread.
    WaitNode* head = wq->dummy.next;
    if (head == NULL) {
        spin_unlock(&wq->lock);
        return NULL;
    }
    wq->dummy.next = head->next;

    Thread* t = head->thread;
    kheap_free(head);
    spin_unlock(&wq->lock);

    thread_resume(t);
    return t;
}

void waitqueue_broadcast(WaitQueue* wq) {
    while (waitqueue_wake(wq)) {}
}
