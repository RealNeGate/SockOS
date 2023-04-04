#ifndef IMPL
typedef int ThreadEntryFn(void*);

typedef struct {
    CPUState state;

    // TODO(NeGate): we're missing quite a bit here
    // * priorities
    // * scheduling statuses
    // * an address space
} Thread;

Thread* threads_current = NULL;
Thread* threads_try_switch(void);

#else

static uint64_t threads_count;
static Thread threads[256];

void threads_init(void) {
}

Thread* threads_spawn(ThreadEntryFn* entrypoint, size_t stack_size, bool is_user) {
    // it's tiny i know...
    void* stack = alloc_physical_page();
    Thread* new_thread = &threads[threads_count++];

    // initial cpu state (CPU specific)
    new_thread->state = new_thread_state(entrypoint, stack, PAGE_SIZE, is_user);
    return new_thread;
}

Thread* threads_try_switch(void) {
    if (threads_current == NULL) {
        return &threads[0];
    }

    int id = ((threads_current - threads) + 1) % threads_count;
    return &threads[id];
}
#endif /* IMPL */
