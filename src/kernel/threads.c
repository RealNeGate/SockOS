#include "threads.h"

static Server* get_sched(void) {
    return &cpu_get()->sched;
}

// envs can have address spaces, threads merely inherit theirs.
//
// from here, the kernel may load an app into memory with the user-land loader
// and we have executables.
Env* env_create(void) {
    Env* env = kheap_zalloc(sizeof(Env));
    env->super.tag = KOBJECT_ENV;
    env->addr_space.working_set = nbhm_alloc(100);
    env->access_rights = nbhm_alloc(500);

    #ifdef __x86_64__
    // copy over the kernel's higher half pages bar for bar.
    env->addr_space.hw_tables = kheap_alloc(PAGE_SIZE);
    for (size_t i = 512; (i--) > 256;) {
        u64 src_page = boot_info->kernel_pml4->entries[i];
        if (src_page == 0) { break; }
        env->addr_space.hw_tables->entries[i] = src_page;
    }
    #else
    #error "TODO"
    #endif

    STORE_PUT(env);
    kprintf("[env] %p | %p | HW Tables at %p\n", env, &env->addr_space.hw_tables, env->addr_space.hw_tables);
    return env;
}

void env_kill(Env* env) {
    // kill all it's threads
    spin_lock(&env->lock);
    for (Thread* t = env->first_in_env; t != NULL; t = t->next_in_env) {
        // we don't want to have thread_kill handle removing from the env because we're
        // potentially doing a lot of removals and we can do that with two NULL pointer stores
        // later.
        t->parent = NULL;
        thread_kill(t);
    }

    env->first_in_env = env->last_in_env = NULL;
    spin_unlock(&env->lock);
}

static atomic_int ID_TICKER = 0;
Thread* thread_create(Env* env, ThreadEntryFn* entrypoint, uintptr_t arg, uintptr_t stack, size_t stack_size) {
    bool is_user = env != NULL;

    Thread* new_thread = kheap_alloc(sizeof(Thread));
    *new_thread = (Thread){
        .super = {
            .tag = KOBJECT_THREAD,
        },
        .parent = env,
        // initial cpu state (CPU specific)
        .state = new_thread_state(entrypoint, arg, stack, stack_size, is_user)
    };
    new_thread->client.id     = ++ID_TICKER;
    new_thread->client.weight = 10;
    new_thread->client.slice  = 1000;

    snprintf(new_thread->tag, 32, "Thread-%p", new_thread);
    STORE_PUT(new_thread);

    // attach to env
    if (env != NULL) {
        spin_lock(&env->lock);
        if (env->first_in_env == NULL) {
            env->first_in_env = new_thread;
        }

        // attach thread to end
        Thread* last = env->last_in_env;
        env->last_in_env = new_thread;
        if (last != NULL) {
            new_thread->prev_in_env = last;
            last->next_in_env = new_thread;
        }

        env->last_in_env = new_thread;
        spin_unlock(&env->lock);
    }

    return new_thread;
}

void thread_resume(Thread* thread, PerCPU* cpu) {
    if (cpu == NULL) {
        cpu = &boot_info->cores[0];
    }

    sched_resume_thread(&cpu->sched, &thread->client);
    arch_wake_up(cpu - boot_info->cores);
}

void thread_kill(Thread* thread) {
    kheap_free(thread, sizeof(Thread));

    // TODO(NeGate): remove from schedule
    // ...

    // remove from env
    Env* env = thread->parent;
    if (env != NULL) {
        spin_lock(&env->lock);

        Thread* prev = thread->prev_in_env;
        Thread* next = thread->next_in_env;
        if (prev == NULL) {
            env->first_in_env = next;
        } else {
            prev->next_in_env = next;
        }

        if (next == NULL) {
            env->last_in_env = prev;
        } else {
            next->prev_in_env = prev;
        }

        // unlock env
        spin_unlock(&env->lock);
    }
}
