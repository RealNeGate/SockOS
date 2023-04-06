#ifndef IMPL
typedef int ThreadEntryFn(void*);

typedef struct Thread Thread;
typedef struct {
    _Atomic int lock;
    PageTable* address_space;

    Thread* first_in_group;
    Thread* last_in_group;
} Threadgroup;

struct Thread {
    Threadgroup* parent;
    Thread* prev_in_group;
    Thread* next_in_group;

    CPUState state;

    // TODO(NeGate): we're missing quite a bit here
    // * priorities
    // * scheduling statuses
};

Thread* threads_current = NULL;
Thread* threads_try_switch(void);

#else

static uint64_t threads_count;
static Thread threads[256];

void threads_init(void) {
}

// groups can have address spaces, threads merely inherit theirs.
//
// from here, the kernel may load an app into memory with the user-land loader
// and we have executables.
Threadgroup* threadgroup_create(void) {
    Threadgroup* group = alloc_physical_page();
    group->address_space = alloc_physical_page();
    return group;
}

void threadgroup_lock(Threadgroup* group) {
    // we shouldn't be spin locking...
    int old = 0;
    while (atomic_compare_exchange_strong(&group->lock, &old, 1)) {}
}

void threadgroup_unlock(Threadgroup* group) {
    atomic_exchange(&group->lock, 0);
}

Thread* thread_create(Threadgroup* group, ThreadEntryFn* entrypoint, uintptr_t stack, size_t stack_size, bool is_user) {
    Thread* new_thread = &threads[threads_count++];
    *new_thread = (Thread){
        .parent = group,

        // initial cpu state (CPU specific)
        .state = new_thread_state(entrypoint, stack, stack_size, is_user)
    };

    // attach to threadgroup
    if (group != NULL) {
        threadgroup_lock(group);
        if (group->first_in_group == NULL) {
            group->first_in_group = new_thread;
        }

        // attach thread to end
        Thread* last = group->last_in_group;
        group->last_in_group = new_thread;
        if (last != NULL) {
            new_thread->prev_in_group = last;
            last->next_in_group = new_thread;
        }

        group->last_in_group = new_thread;
        threadgroup_unlock(group);
    }

    return new_thread;
}

void threads_kill(Thread* thread) {
    Threadgroup* group = thread->parent;
    threadgroup_lock(group);

    // remove
    Thread* prev = thread->prev_in_group;
    Thread* next = thread->next_in_group;
    if (prev == NULL) {
        group->first_in_group = next;
    } else {
        prev->next_in_group = next;
    }

    if (next == NULL) {
        group->last_in_group = prev;
    } else {
        next->prev_in_group = prev;
    }

    // unlock threadgroup
    threadgroup_unlock(group);
}

Thread* threads_try_switch(void) {
    if (threads_current == NULL) {
        return &threads[0];
    }

    int id = ((threads_current - threads) + 1) % threads_count;
    return &threads[id];
}

static void identity_map_kernel_region(PageTable* address_space, void* p, size_t size) {
    uintptr_t x = (((uintptr_t) p) & ~0xFFF);

    // relocate higher half addresses to the ELF in physical memory
    if (x >= 0xFFFFFFFF80000000ull) {
        uintptr_t delta = 0xFFFFFFFF80000000ull - boot_info->elf_physical_ptr;
        x -= delta;
    }

    kprintf("identity map %p => %p (%d)\n", (uintptr_t) p, x, size);
    memmap__view(address_space, x, ((uintptr_t) p) & ~0xFFF, size, PAGE_WRITE);
}

// this is the trusted ELF loader for priveleged programs, normal apps will probably
// be loaded via a shared object.
Threadgroup* threadgroup_spawn(const uint8_t* program, size_t program_size, Thread** root_thread) {
    Threadgroup* group = threadgroup_create();
    kprintf("address=%p\n", group->address_space);

    Elf64_Ehdr* elf_header = (Elf64_Ehdr*) program;
    uintptr_t image_base = 0xC0000000;

    // identity map essential kernel stuff
    //   * IRQ handler
    extern void asm_int_handler(void);
    identity_map_kernel_region(group->address_space, &asm_int_handler, 4096);
    identity_map_kernel_region(group->address_space, (void*) &_idt[0], sizeof(_idt));
    identity_map_kernel_region(group->address_space, boot_info->kernel_stack, KERNEL_STACK_SIZE);
    identity_map_kernel_region(group->address_space, boot_info, sizeof(BootInfo));
    identity_map_kernel_region(group->address_space, &boot_info, sizeof(BootInfo*));

    ////////////////////////////////
    // find program bounds
    ////////////////////////////////
    uintptr_t image_size = 0;

    const uint8_t* segments = program + elf_header->e_phoff;
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
    char* dst = alloc_physical_pages(num_pages);
    if (dst == NULL) {
        // no memory? lmao
        return NULL;
    }

    memmap__view(group->address_space, (uintptr_t) dst, image_base, image_size, PAGE_USER | PAGE_WRITE);

    ////////////////////////////////
    // map segments
    ////////////////////////////////
    size_t segment_header_bounds = elf_header->e_phoff + elf_header->e_phnum*segment_size;
    kassert(segment_header_bounds < program_size, "segments do not fit into file");

    FOREACH_N(i, 0, elf_header->e_phnum) {
        Elf64_Phdr* segment = (Elf64_Phdr*) (segments + i*segment_size);
        if (segment->p_type != PT_LOAD) continue;

        // check segment permissions
        kassert(segment->p_filesz <= segment->p_memsz, "no enough space in memory for file data");
        kassert((segment->p_align & (segment->p_align - 1)) == 0, "alignment is not a power-of-two");

        uintptr_t vaddr = segment->p_vaddr;

        // map file contents
        if (segment->p_filesz) {
            kassert(segment->p_offset + segment->p_filesz < program_size, "segment contents out of bounds");

            const uint8_t* src = program + segment->p_offset;
            memcpy(dst + vaddr, src, segment->p_filesz);
        }

        // map zeroed region at the end
        if (segment->p_memsz > segment->p_filesz) {
            memset(dst + vaddr + segment->p_filesz, 0, segment->p_memsz - segment->p_filesz);
        }
    }

    // tiny i know
    void* physical_stack = alloc_physical_page();
    memmap__view(group->address_space, (uintptr_t) physical_stack, 0xA0000000, 4096, PAGE_USER | PAGE_WRITE);

    *root_thread = thread_create(group, (ThreadEntryFn*) (image_base + elf_header->e_entry), 0xA0000000, 4096, true);
    return group;
}

#endif /* IMPL */
