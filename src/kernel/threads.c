#include "threads.h"

static PerCPU_Scheduler* get_sched(void) {
    PerCPU* cpu = cpu_get();
    return cpu->sched;
}

// envs can have address spaces, threads merely inherit theirs.
//
// from here, the kernel may load an app into memory with the user-land loader
// and we have executables.
Env* env_create(void) {
    Env* env = kheap_zalloc(sizeof(Env));
    env->super.tag = KOBJECT_ENV;
    env->addr_space.working_set = nbhm_alloc(100);

    env->handles = kheap_zalloc(sizeof(KHandleTable) + sizeof(KHandleEntry));
    env->handles->capacity = 1;
    env->handles->entries[0].open = 1;

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

static int round_robin_load_balance;
Thread* thread_create(Env* env, ThreadEntryFn* entrypoint, uintptr_t arg, uintptr_t stack, size_t stack_size) {
    bool is_user = env != NULL;

    Thread* new_thread = kheap_alloc(sizeof(Thread));
    *new_thread = (Thread){
        .super = {
            .tag = KOBJECT_THREAD,
        },
        .parent = env,
        .wake_time = 0,
        .exec_time = 0,

        // initial cpu state (CPU specific)
        .state = new_thread_state(entrypoint, arg, stack, stack_size, is_user)
    };

    // userland programs need an extra stack for syscall handling
    if (0 && is_user) {
        // map all kernel stacks
        FOR_N(i, 0, boot_info->core_count) {
            char* page = ((char*) boot_info->cores[i].kernel_stack_top) - KERNEL_STACK_SIZE;
            memmap_view(boot_info->kernel_pml4, kaddr2paddr(page), (uintptr_t) page, KERNEL_STACK_SIZE, VMEM_PAGE_WRITE);
        }
    }

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

void thread_resume(Thread* thread) {
    /*int i = round_robin_load_balance;
    if (++round_robin_load_balance == boot_info->core_count) {
        round_robin_load_balance = 0;
    }*/
    // ON_DEBUG(SCHED)(kprintf("[sched] created Thread-%p, placed onto CPU-%d (%s)\n", thread, i, thread->parent ? "USER" : "KERNEL"));

    int i = 0;

    // Put to sleep on a core
    PerCPU_Scheduler* sched = boot_info->cores[i].sched;
    spin_lock(&sched->lock);
    kassert(!sched_is_blocked(thread), "we shouldn't be blocked atm... why are you resuming us");
    Thread* latest = atomic_load_explicit(&boot_info->cores[i].blocked_threads, memory_order_relaxed);
    do {
        thread->next_in_blocked = latest;
    } while (!atomic_compare_exchange_strong_explicit(&boot_info->cores[i].blocked_threads, &latest, thread, memory_order_acq_rel, memory_order_acquire));
    spin_unlock(&sched->lock);

    arch_wake_up(i);
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
