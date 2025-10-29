#include <kernel.h>
#include "threads.h"

WaitQueue* waitqueue_alloc(void) {
    return NULL;
}

void waitqueue_free(WaitQueue* wq) {
}

void waitqueue_wait(WaitQueue* wq, Thread* t) {
}

Thread* waitqueue_wake(WaitQueue* wq) {
    return NULL;
}

void waitqueue_broadcast(WaitQueue* wq) {
}
