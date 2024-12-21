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
    env->addr_space.commit_table = nbhm_alloc(60);

    env->handles = kheap_zalloc(sizeof(KHandleTable) + sizeof(KHandleEntry));
    env->handles->capacity = 1;
    env->handles->entries[0].open = 1;

    #ifdef __x86_64__
    // copy over the kernel's higher half pages bar for bar.
    env->addr_space.hw_tables = kpool_alloc_page();
    for (size_t i = 512; (i--) > 256;) {
        u64 src_page = boot_info->kernel_pml4->entries[i];
        if (src_page == 0) { break; }
        env->addr_space.hw_tables->entries[i] = src_page;
    }
    #else
    #error "TODO"
    #endif

    kprintf("[env] HW Tables at %p\n", env->addr_space.hw_tables);
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

    Thread* new_thread = kpool_alloc_page();
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
    if (is_user) {
        void* page = kpool_alloc_page();

        new_thread->kstack_addr = (uintptr_t) page + PAGE_SIZE;
        memmap_view(boot_info->kernel_pml4, kaddr2paddr(page), (uintptr_t) page, PAGE_SIZE, VMEM_PAGE_WRITE);

        kprintf("[env] Thread kernel stack: %p - %p\n", new_thread->kstack_addr, new_thread->kstack_addr + PAGE_SIZE - 1);
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

    // put into schedule
    {
        int i = round_robin_load_balance;
        if (++round_robin_load_balance == boot_info->core_count) {
            round_robin_load_balance = 0;
        }

        ON_DEBUG(SCHED)(kprintf("[sched] created Thread-%p, placed onto CPU-%d (%s)\n", new_thread, i, new_thread->parent ? "USER" : "KERNEL"));

        PerCPU_Scheduler* sched = boot_info->cores[i].sched;
        spin_lock(&sched->lock);
        new_thread->active = true;
        tq_append(&sched->active, new_thread);
        spin_unlock(&sched->lock);
        arch_wake_up(i);
    }

    return new_thread;
}

void thread_kill(Thread* thread) {
    kheap_free(thread);

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
    kprintf("Loading a program! %p\n", program);
    Elf64_Ehdr* elf_header = (Elf64_Ehdr*) program;

    KObject_VMO* vmo_ptr = vmo_create_physical(kaddr2paddr((void*) program), program_size);
    KHandle elf_vmo = env_open_handle(env, 0, &vmo_ptr->super);

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
        uintptr_t vaddr = segment->p_vaddr & -PAGE_SIZE;
        size_t offset   = segment->p_offset & -PAGE_SIZE;

        // kprintf("[elf] segment: %p (%d) => %p (%d)\n", vaddr, segment->p_memsz, paddr, segment->p_filesz);

        size_t file_size = (segment->p_filesz + PAGE_SIZE - 1) & -PAGE_SIZE;
        size_t mem_size  = (segment->p_memsz  + PAGE_SIZE - 1) & -PAGE_SIZE;

        if (file_size > 0) {
            vmem_add_range(env, elf_vmo, vaddr, offset, file_size, VMEM_PAGE_WRITE | VMEM_PAGE_USER);
        }

        if (mem_size > file_size) {
            // zero pages
            vmem_add_range(env, 0, vaddr+mem_size, file_size - mem_size, 0, VMEM_PAGE_WRITE | VMEM_PAGE_USER);
        }
    }

    // tiny i know
    uintptr_t stack_ptr = vmem_map(env, 0, 0, USER_STACK_SIZE, VMEM_PAGE_WRITE | VMEM_PAGE_USER);

    kprintf("[elf] entry=%p\n", elf_header->e_entry);
    kprintf("[elf] stack=%p\n", stack_ptr);

    return thread_create(env, (ThreadEntryFn*) elf_header->e_entry, boot_info->tsc_freq, stack_ptr, USER_STACK_SIZE);
}
