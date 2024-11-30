#ifndef IMPL
typedef int ThreadEntryFn(void*);

typedef struct Thread Thread;
typedef struct Wait Wait;
typedef struct Env Env;

#define SCHED_QUANTA 15625 // 64Hz

struct Env {
    _Atomic int lock;

    VMem_AddrSpace* address_space;

    Thread* first_in_env;
    Thread* last_in_env;
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
    env->address_space = kernelfl_alloc(sizeof(VMem_AddrSpace));
    env->address_space->hw_tables = alloc_physical_page();

    // identity map essential kernel stuff
    //   * IRQ handler
    extern void asm_int_handler(void);
    extern void syscall_handler(void);
    identity_map_kernel_region(env->address_space, &asm_int_handler, 4096);
    identity_map_kernel_region(env->address_space, &syscall_handler, 4096);
    identity_map_kernel_region(env->address_space, (void*) &_idt[0], sizeof(_idt));
    identity_map_kernel_region(env->address_space, boot_info, sizeof(BootInfo));
    identity_map_kernel_region(env->address_space, &boot_info, sizeof(BootInfo*));
    identity_map_kernel_region(env->address_space, &syscall_table[0], sizeof(syscall_table));

    // TODO(NeGate): uhh... i think we wanna map all their kernel stacks?
    identity_map_kernel_region(env->address_space, boot_info->cores[0].kernel_stack, KERNEL_STACK_SIZE);

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
    kprintf("Loading a program!\n");
    Elf64_Ehdr* elf_header = (Elf64_Ehdr*) program;

    ////////////////////////////////
    // find program bounds
    ////////////////////////////////
    uintptr_t image_base = 0xC0000000;
    uintptr_t image_size = 0;

    const u8* segments = program + elf_header->e_phoff;
    size_t segment_size = elf_header->e_phentsize;
    FOREACH_N(i, 0, elf_header->e_phnum) {
        Elf64_Phdr* segment = (Elf64_Phdr*) (segments + i*segment_size);
        if (segment->p_type != PT_LOAD) continue;

        uintptr_t segment_top = segment->p_vaddr + segment->p_memsz;
        if (segment_top > image_size) {
            image_size = segment_top;
        }
    }

    ////////////////////////////////
    // allocate virtual pages
    ////////////////////////////////
    size_t num_pages = (image_size + 0xFFF) / 4096;

    ////////////////////////////////
    // map segments
    ////////////////////////////////
    size_t segment_header_bounds = elf_header->e_phoff + elf_header->e_phnum*segment_size;
    kassert(segment_header_bounds < program_size, "segments do not fit into file");

    /* FOREACH_N(i, 0, elf_header->e_phnum) {
        Elf64_Phdr* segment = (Elf64_Phdr*) (segments + i*segment_size);
        if (segment->p_type != PT_LOAD) continue;

        // check segment permissions
        kassert(segment->p_filesz <= segment->p_memsz, "no enough space in memory for file data");
        kassert((segment->p_align & (segment->p_align - 1)) == 0, "alignment is not a power-of-two");

        // file offset % page_size == virtual addr % page_size, it allows us to file map
        // awkward offsets because the virtual address is just as awkward :p
        uintptr_t vaddr = image_base + (segment->p_vaddr & -0x1000);

        // allocate individual chunks
        char* dst = alloc_physical_chunk();
        kassert(dst, "OOM!");
        kassert(segment->p_memsz < CHUNK_SIZE, "segment too big");

        kprintf("  Segment: %p => %p\n", dst, vaddr);

        // map file contents
        if (segment->p_filesz) {
            kassert(segment->p_offset + segment->p_filesz <= program_size, "segment contents out of bounds (%x + %x < %x)", segment->p_offset, segment->p_filesz, program_size);

            const u8* src = program + segment->p_offset;
            memcpy(dst, src, segment->p_filesz);
        }

        // map zeroed region at the end
        if (segment->p_memsz > segment->p_filesz) {
            memset(dst + segment->p_filesz, 0, segment->p_memsz - segment->p_filesz);
        }

        memmap__view(env->address_space, (uintptr_t) dst, vaddr, segment->p_memsz, PAGE_USER | PAGE_WRITE);
    }

    // tiny i know
    void* physical_stack = alloc_physical_page();
    memmap__view(env->address_space, (uintptr_t) physical_stack, 0xA0000000, 4096, PAGE_USER | PAGE_WRITE); */

    return thread_create(env, (ThreadEntryFn*) (image_base + elf_header->e_entry), 0xA0000000, 4096, true);
}

#endif /* IMPL */
