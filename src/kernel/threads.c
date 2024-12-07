#ifndef IMPL
typedef int ThreadEntryFn(void*);

typedef struct Thread Thread;
typedef struct Wait Wait;
typedef struct Env Env;

#define SCHED_QUANTA 15625 // 64Hz

struct Env {
    _Atomic int lock;
    VMem_AddrSpace addr_space;

    Thread* first_in_env;
    Thread* last_in_env;

    // Handle table
};

struct Thread {
    Env* parent;
    Thread* prev_in_env;
    Thread* next_in_env;

    Thread* prev_sched;
    Thread* next_sched;

    // active range
    u64 start_time, end_time;

    // how much of the quanta did we give up to waiting
    u64 wait_time;

    // sleeping
    u64 wake_time;

    CPUState state;
};

#define IS_AWAKE(t) ((t).wake_time == 0)

_Atomic int threads_lock;

void spin_lock(_Atomic(int)* lock);
void spin_unlock(_Atomic(int)* lock);

Env* env_create(void);
void env_kill(Env* env);

Thread* thread_create(Env* env, ThreadEntryFn* entrypoint, uintptr_t stack, size_t stack_size, bool is_user);
void thread_kill(Thread* thread);

// this doesn't stall, just schedules a wait
void sched_wait(Thread* t, u64 timeout);
Thread* sched_try_switch(u64 now_time, u64* restrict next_wake);

#else

static PerCPU_Scheduler* get_sched(void) {
    // initialize scheduler
    PerCPU* cpu = get_percpu();
    if (cpu->sched == NULL) {
        cpu->sched = alloc_physical_page();
        *cpu->sched = (PerCPU_Scheduler){ 0 };
    }

    return cpu->sched;
}

void spin_lock(_Atomic(int)* lock) {
    // we shouldn't be spin locking...
    while (!atomic_compare_exchange_strong(lock, &(int){ 0 }, 1)) {
        asm volatile ("pause");
    }
}

void spin_unlock(_Atomic(int)* lock) {
    atomic_exchange(lock, 0);
}

static void identity_map_kernel_region(VMem_AddrSpace* addr_space, void* p, size_t size) {
    uintptr_t x = (((uintptr_t) p) & ~0xFFF);

    // relocate higher half addresses to the ELF in physical memory
    if (x >= 0xFFFFFFFF80000000ull) {
        uintptr_t delta = 0xFFFFFFFF80000000ull - boot_info->elf_physical_ptr;
        x -= delta;
    }

    uintptr_t base = ((uintptr_t) p) & ~0xFFF;
    memmap__view(addr_space->hw_tables, x, base, size + 4096, PAGE_WRITE);
}

// envs can have address spaces, threads merely inherit theirs.
//
// from here, the kernel may load an app into memory with the user-land loader
// and we have executables.
Env* env_create(void) {
    Env* env = alloc_physical_page();
    env->addr_space.hw_tables = alloc_physical_page();
    env->addr_space.commit_table = nbhm_alloc(500);

    // copy over the kernel's higher half pages bar for bar.
    for (size_t i = 512; (i--) > 256;) {
        u64 src_page = boot_info->kernel_pml4->entries[i];
        if (src_page == 0) { break; }
        env->addr_space.hw_tables->entries[i] = src_page;
    }

    // kprintf("AA\n");
    // dump_pages(boot_info->kernel_pml4, 0, 0);
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

Thread* thread_create(Env* env, ThreadEntryFn* entrypoint, uintptr_t stack, size_t stack_size, bool is_user) {
    Thread* new_thread = alloc_physical_page();
    *new_thread = (Thread){
        .parent = env,
        .wake_time = 0,

        // initial cpu state (CPU specific)
        .state = new_thread_state(entrypoint, stack, stack_size, is_user)
    };

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

    // put into schedule
    PerCPU_Scheduler* sched = get_sched();
    spin_lock(&threads_lock);
    tq_append(&sched->active, new_thread);
    if (sched->active.curr == NULL) {
        sched->active.curr = new_thread;
    }
    spin_unlock(&threads_lock);

    kprintf("created thread: %p\n", new_thread);
    return new_thread;
}

void thread_kill(Thread* thread) {
    free_physical_page(thread);

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

////////////////////////////////
// Mini-ELF loader
////////////////////////////////
// this is the trusted ELF loader for priveleged programs, normal apps will probably
// be loaded via a shared object.
Thread* env_load_elf(Env* env, const u8* program, size_t program_size) {
    kprintf("Loading a program! %p\n", env->addr_space.hw_tables);
    Elf64_Ehdr* elf_header = (Elf64_Ehdr*) program;

    ////////////////////////////////
    // map segments
    ////////////////////////////////
    size_t segment_size = elf_header->e_phentsize;
    size_t segment_header_bounds = elf_header->e_phoff + elf_header->e_phnum*segment_size;
    kassert(segment_header_bounds < program_size, "segments do not fit into file");

    const u8* segments = program + elf_header->e_phoff;
    FOR_N(i, 0, elf_header->e_phnum) {
        Elf64_Phdr* segment = (Elf64_Phdr*) (segments + i*segment_size);
        if (segment->p_type != PT_LOAD) continue;

        // check segment permissions
        kassert(segment->p_filesz <= segment->p_memsz, "no enough space in memory for file data");
        kassert((segment->p_align & (segment->p_align - 1)) == 0, "alignment is not a power-of-two");

        // file offset % page_size == virtual addr % page_size, it allows us to file map
        // awkward offsets because the virtual address is just as awkward :p
        uintptr_t vaddr = (segment->p_vaddr & -PAGE_SIZE);
        uintptr_t paddr = kaddr2paddr((u8*) program + (segment->p_offset & -PAGE_SIZE));

        // kprintf("[elf] segment: %p (%d) => %p (%d)\n", vaddr, segment->p_memsz, paddr, segment->p_filesz);

        size_t file_size = (segment->p_filesz + PAGE_SIZE - 1) & -PAGE_SIZE;
        size_t mem_size  = (segment->p_memsz  + PAGE_SIZE - 1) & -PAGE_SIZE;

        if (file_size > 0) {
            vmem_add_range(&env->addr_space, vaddr, file_size, paddr, VMEM_PAGE_READ | VMEM_PAGE_WRITE | VMEM_PAGE_USER);
        }

        if (mem_size > file_size) {
            // zero pages
            vmem_add_range(&env->addr_space, vaddr+mem_size, file_size - mem_size, 0, VMEM_PAGE_READ | VMEM_PAGE_WRITE | VMEM_PAGE_USER);
        }
    }

    // tiny i know
    uintptr_t stack_ptr = vmem_alloc(&env->addr_space, 8192, 0, VMEM_PAGE_READ | VMEM_PAGE_WRITE | VMEM_PAGE_USER);

    kprintf("[elf] entry=%p\n", elf_header->e_entry);
    kprintf("[elf] stack=%p\n", stack_ptr);

    return thread_create(env, (ThreadEntryFn*) elf_header->e_entry, stack_ptr, 8192, true);
}

#endif /* IMPL */
