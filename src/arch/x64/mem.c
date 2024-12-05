enum {
    CHUNK_SIZE = 2*1024*1024
};

typedef enum Result {
    RESULT_SUCCESS,

    // memory mapping errors
    RESULT_OUT_OF_PAGE_TABLES,
    RESULT_ALLOCATION_UNALIGNED,
} Result;

typedef enum PageFlags {
    PAGE_PRESENT   = 1,
    PAGE_WRITE     = 2,
    PAGE_USER      = 4,
    PAGE_WRITETHRU = 8,
    PAGE_NOCACHE   = 16,
    PAGE_ACCESSED  = 32,
} PageFlags;

typedef struct KernelFreeList KernelFreeList;
struct KernelFreeList {
    // next node is directly after the end size of this one
    uint64_t size     : 62;
    uint64_t is_free  : 1;
    uint64_t has_next : 1;
    // used to track coalescing correctly
    KernelFreeList* prev;
    char data[];
};

static KernelFreeList* kernel_free_list;

static KernelFreeList* kernelfl_next(KernelFreeList* n) {
    return n->has_next ? (KernelFreeList*) &n->data[n->size - sizeof(KernelFreeList)] : NULL;
}

static void* kernelfl_alloc(size_t obj_size) {
    // add free list header and 8b padding
    obj_size = (obj_size + 7) & ~7;
    obj_size += sizeof(KernelFreeList);

    for (KernelFreeList* list = kernel_free_list; list; list = kernelfl_next(list)) {
        if (list->is_free) {
            if (list->size == obj_size) {
                // perfect fit
                list->is_free = false;

                kprintf("[kheap] alloc(%d) = %p\n", obj_size, &list->data[0]);
                return &list->data[0];
            } else if (list->size > obj_size) {
                size_t full_size = list->size;

                // split
                KernelFreeList* split = (KernelFreeList*) &list->data[obj_size - sizeof(KernelFreeList)];
                list->is_free = false;
                list->size = obj_size;

                split->size = full_size - obj_size;
                split->is_free = false;
                split->prev = list;

                kprintf("[kheap] alloc(%d) = %p\n", obj_size, &list->data[0]);
                return &list->data[0];
            }
        }
    }

    kassert(0, "Ran out of kernel memory");
}

static void kernelfl_free(void* obj) {
    KernelFreeList* list = &((KernelFreeList*) obj)[-1];
    kassert(!list->is_free, "not allocated... you can't free it");

    list->is_free = true;

    // if there's free space ahead, merge with it
    KernelFreeList* next = kernelfl_next(list);
    if (next->is_free) {
        list->size += next->size;
    }

    KernelFreeList* prev = list->prev;
    if (list->prev && list->prev->is_free) {
        prev->size += list->size;
        list = prev;
    }

    memset(list->data, 0xCD, list->size - sizeof(KernelFreeList));
}

static void* kernelfl_realloc(void* obj, size_t obj_size) {
    if (obj == NULL) {
        return kernelfl_alloc(obj_size);
    }

    KernelFreeList* list = &((KernelFreeList*) obj)[-1];
    if (obj_size == 0) {
        kernelfl_free(obj);
    }

    // add free list header and 8b padding
    obj_size = (obj_size + 7) & ~7;
    obj_size += sizeof(KernelFreeList);

    // TODO(NeGate): shrink allocation when obj_size is smaller than list->size
    // ...

    // TODO(NeGate): extend current space
    // ...

    // TODO(NeGate): migrate allocation
    // ...
    kassert(0, "TODO");
}

static size_t cpu_alloc_queue_mask;
static void* alloc_physical_chunk(void);

static PerCPU* get_percpu(void) {
    uint32_t* cookie = (uint32_t*) ((uintptr_t) __builtin_frame_address(0) & -KERNEL_STACK_SIZE);
    kassert(cookie[0] == KERNEL_STACK_COOKIE, "bad cookie (%x @ %p)", cookie[0], cookie);
    return &boot_info->cores[cookie[1]];
}

static void init_physical_page_alloc(MemMap* restrict mem_map) {
    kprintf("MemMap: %p\n", mem_map);
    kprintf("* %p\n", mem_map->regions[0].base);

    size_t total_chunks = 0;
    int biggest = -1;
    FOREACH_N(i, 0, mem_map->nregions) {
        MemRegion* region = &mem_map->regions[i];
        if (region->type != MEM_REGION_USABLE || region->base < 0x100000) {
            continue;
        }

        if (region->pages > mem_map->regions[biggest].pages) {
            biggest = i;
        }

        total_chunks += region->pages / (CHUNK_SIZE / PAGE_SIZE);
    }

    int64_t shift = 64 - __builtin_clzll(total_chunks - 1);
    cpu_alloc_queue_mask   = (1ull << shift) - 1;
    size_t pages_per_queue = (((1ull << shift)*8) + PAGE_SIZE - 1) / PAGE_SIZE;

    kprintf("Physical memory (%d chunks, %d KiB / queue)\n", (int)total_chunks, (pages_per_queue * PAGE_SIZE) / 1024);

    // c is count of chunks handed to a core's queue,
    size_t c = 0;
    FOREACH_N(i, 0, mem_map->nregions) {
        MemRegion* restrict region = &mem_map->regions[i];
        if (region->type != MEM_REGION_USABLE || region->base < 0x100000) {
            continue;
        }

        int aa = (region->pages*PAGE_SIZE) / (1024*1024);

        uintptr_t base = boot_info->identity_map_ptr + region->base;
        uintptr_t end  = (base + region->pages*PAGE_SIZE) & -CHUNK_SIZE;

        // chop off 10% of the biggest memory region for the kernel heap
        if (biggest == i) {
            size_t kernel_heap_size = ((region->pages + 9) / 10) * PAGE_SIZE;
            kprintf("Kernel heap: %p - %p (%d MiB)\n", base, base + kernel_heap_size - 1, kernel_heap_size / (1024*1024));

            kernel_free_list = (KernelFreeList*) base;
            kernel_free_list->size = kernel_heap_size;
            kernel_free_list->is_free = 1;
            kernel_free_list->has_next = false;
            kernel_free_list->prev = NULL;

            base += kernel_heap_size;
        }

        if (boot_info->cores[0].alloc.data == NULL) {
            if ((end - base) / PAGE_SIZE < pages_per_queue) {
                continue;
            }

            boot_info->cores[0].alloc.data = (void*) base;
            base += pages_per_queue*PAGE_SIZE;
        }

        uintptr_t i = (base + CHUNK_SIZE - 1) & -CHUNK_SIZE;
        if (i < end) {
            kprintf("Region: %p - %p (%d MiB)\n", i, end - 1, aa);

            while (i < end) {
                atomic_store_explicit(&boot_info->cores[0].alloc.data[c++], (void*) i, memory_order_relaxed);
                i += CHUNK_SIZE;
            }
        }
    }

    atomic_store_explicit(&boot_info->cores[0].alloc.bot, c, memory_order_relaxed);
}

// hand the other threads portions of the memory
// so one queue is clogged up and the others are starved.
static void subdivide_memory(MemMap* restrict mem_map, int num_cores) {
    int64_t b = atomic_load_explicit(&boot_info->cores[0].alloc.bot, memory_order_acquire);
    int64_t t = atomic_load_explicit(&boot_info->cores[0].alloc.top, memory_order_acquire);

    int per_core = (b - t) / num_cores;
    size_t pages_per_queue = (((cpu_alloc_queue_mask+1)*8) + PAGE_SIZE - 1) / PAGE_SIZE;

    // kprintf("Core[0] queue has %d entries (split into %d)\n", boot_info->cores[0].alloc.bot, per_core);

    int64_t k = t + per_core;
    FOREACH_N(i, 1, num_cores) {
        kassert(pages_per_queue == 1, "TODO: queue too big (we're dumb)");
        boot_info->cores[i].alloc.data = alloc_physical_page();

        FOREACH_N(j, 0, per_core) {
            void* ptr = atomic_load_explicit(&boot_info->cores[0].alloc.data[k & cpu_alloc_queue_mask], memory_order_relaxed);

            // kprintf("Page[%d]: %p\n", k & cpu_alloc_queue_mask, ptr);

            atomic_store_explicit(&boot_info->cores[i].alloc.data[j], ptr, memory_order_relaxed);
            k = (k + 1) & cpu_alloc_queue_mask;
        }
        atomic_store_explicit(&boot_info->cores[i].alloc.bot, per_core, memory_order_relaxed);
    }

    atomic_store_explicit(&boot_info->cores[0].alloc.bot, per_core, memory_order_relaxed);
}

static void* alloc_physical_chunk(void) {
    PerCPU* cpu = get_percpu();

    // pop from local queue
    int64_t b = atomic_load_explicit(&cpu->alloc.bot, memory_order_acquire);
    int64_t t = atomic_load_explicit(&cpu->alloc.top, memory_order_acquire);

    b -= 1;
    atomic_store_explicit(&cpu->alloc.bot, b, memory_order_release);

    int64_t size = b - t;
    if (size < 0) {
        atomic_store_explicit(&cpu->alloc.bot, b, memory_order_release);

        // local queue is empty, go try to steal from a friend
        kassert(0, "unreachable");
        return NULL;
    }

    void* ptr = atomic_load_explicit(&cpu->alloc.data[b & cpu_alloc_queue_mask], memory_order_acquire);
    if (size > 0) {
        return ptr;
    }

    if (!atomic_compare_exchange_strong(&cpu->alloc.top, &t, t + 1)) {
        kassert(0, "unreachable");
        return NULL;
    }

    atomic_store_explicit(&cpu->alloc.bot, t + 1, memory_order_release);
    return ptr;
}

static void free_physical_chunk(void* ptr) {
    PerCPU* cpu = get_percpu();

    // push to local queue
    int64_t b = atomic_load_explicit(&cpu->alloc.bot, memory_order_acquire);
    atomic_store_explicit(&cpu->alloc.data[b & cpu_alloc_queue_mask], ptr, memory_order_release);
    atomic_store_explicit(&cpu->alloc.bot, b + 1, memory_order_release);
}

static void free_physical_page(void* ptr) {
    PerCPU* cpu = get_percpu();

    // push to freelist
    PageFreeList* fl = (PageFreeList*) ptr;
    fl->next = cpu->heap;
    cpu->heap = fl;
}

static void* alloc_physical_page(void) {
    PerCPU* cpu = get_percpu();
    if (cpu->heap == NULL) {
        // allocate chunk, subdivide into 4KiB
        char* dst = alloc_physical_chunk();
        for (size_t i = CHUNK_SIZE / PAGE_SIZE; i--;) {
            free_physical_page(&dst[i * PAGE_SIZE]);
        }
    }

    // pop freelist
    PageFreeList* fl = cpu->heap;
    cpu->heap = fl->next;

    kprintf("[kpool] alloc(%d) = %p\n", PAGE_SIZE, fl);
    memset(fl, 0, PAGE_SIZE);
    return fl;
}

static u64 canonical_addr(u64 ptr) {
    return (ptr >> 48) != 0 ? ptr | (0xFFFull << 48) : ptr;
}

static PageTable* get_pt(PageTable* parent, size_t index) {
    if (parent->entries[index] & PAGE_PRESENT) {
        return paddr2kaddr(canonical_addr(parent->entries[index] & 0xFFFFFFFFF000ull));
    } else {
        return NULL;
    }
}

static PageTable* get_or_alloc_pt(PageTable* parent, size_t index, int depth, PageFlags flags) {
    if (parent->entries[index] & PAGE_PRESENT) {
        if (flags) {
            parent->entries[index] |= flags;
        }

        return paddr2kaddr(canonical_addr(parent->entries[index] & 0xFFFFFFFFF000ull));
    }

    PageTable* new_pt = alloc_physical_page();
    kassert(((uintptr_t) new_pt & 0xFFF) == 0, "page tables must be 4KiB aligned");
    parent->entries[index] = kaddr2paddr(new_pt) | flags | PAGE_PRESENT;
    return new_pt;
}

// Identity map
static Result memmap__view(PageTable* address_space, uintptr_t phys_addr, uintptr_t virt_addr, size_t size, PageFlags flags) {
    size_t page_count = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    kassert((phys_addr & 0xFFFull) == 0, "physical address unaligned (%p)", phys_addr);
    kassert((virt_addr & 0xFFFull) == 0, "virtual address unaligned (%p)", virt_addr);
    kassert(flags & 0xFFF, "invalid flags (%x)", flags);

    // Generate the page table mapping
    bool is_current = kaddr2paddr(address_space) == x86_get_cr3();
    for (size_t i = 0; i < page_count; i++) {
        PageTable* table_l3 = get_or_alloc_pt(address_space, (virt_addr >> 39) & 0x1FF, 0, flags); // 512GiB
        PageTable* table_l2 = get_or_alloc_pt(table_l3,      (virt_addr >> 30) & 0x1FF, 1, flags); // 1GiB
        PageTable* table_l1 = get_or_alloc_pt(table_l2,      (virt_addr >> 21) & 0x1FF, 2, flags); // 2MiB
        size_t pte_index = (virt_addr >> 12) & 0x1FF; // 4KiB

        table_l1->entries[pte_index] = (phys_addr & 0xFFFFFFFFF000) | flags | PAGE_PRESENT;
        if (is_current) {
            x86_invalidate_page(virt_addr);
        }

        virt_addr += PAGE_SIZE, phys_addr += PAGE_SIZE;
    }

    return RESULT_SUCCESS;
}

static void memmap__unview(PageTable* address_space, uintptr_t virt_addr, size_t size) {
    size_t page_count = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    kassert((virt_addr & 0xFFFull) == 0, "virtual address unaligned (%p)", virt_addr);

    // Generate the page table mapping
    bool is_current = kaddr2paddr(address_space) == x86_get_cr3();
    for (size_t i = 0; i < page_count; i++, virt_addr += PAGE_SIZE) {
        PageTable* table_l3 = get_pt(address_space, (virt_addr >> 39) & 0x1FF); // 512GiB
        if (table_l3 == NULL) { continue; }
        PageTable* table_l2 = get_pt(table_l3,      (virt_addr >> 30) & 0x1FF); // 1GiB
        if (table_l2 == NULL) { continue; }
        PageTable* table_l1 = get_pt(table_l2,      (virt_addr >> 21) & 0x1FF); // 2MiB
        if (table_l1 == NULL) { continue; }

        size_t pte_index = (virt_addr >> 12) & 0x1FF; // 4KiB
        table_l1->entries[pte_index] = 0;
        if (is_current) {
            x86_invalidate_page(virt_addr);
        }
    }
}

static void dump_pages(PageTable* pt, int depth, uintptr_t base) {
    static const uint64_t shifts[4] = { 39, 30, 21, 12 };
    for (int i = 0; i < 512; i++) {
        if (pt->entries[i] & 1) {
            for (int i = 0; i < depth; i++) { kprintf("  "); }

            uintptr_t vaddr = base + (i << shifts[depth]);
            kprintf("[%d] %p (%p)\n", i, pt->entries[i], vaddr);

            if (depth < 3) {
                dump_pages((PageTable*) (pt->entries[i] & -PAGE_SIZE), depth + 1, vaddr);
            }
        }
    }
}

static void memdump(u64 *buffer, size_t size) {
    int scale = 16;
    int max_pixel = boot_info->fb.width * boot_info->fb.height;
    int width = boot_info->fb.width;

    for (int i = 0; i < size; i++) {
        u32 color = 0xFF000000 | ((buffer[i] > 0) ? buffer[i] : 0xFF050505);

        for (int y = 0; y < scale; y++) {
            for (int x = 0; x < scale; x++) {

                int sx = ((i * scale) % width) + x;
                int sy = (((i * scale) / width) * scale) + y;
                int idx = (sy * width) + sx;
                if (idx >= max_pixel) return;

                boot_info->fb.pixels[idx] = color;
            }
        }
    }
}

static u64 memmap__probe(PageTable* address_space, uintptr_t virt) {
    size_t l[4] = {
        (virt >> 39) & 0x1FF,
        (virt >> 30) & 0x1FF,
        (virt >> 21) & 0x1FF,
        (virt >> 12) & 0x1FF,
    };

    PageTable* curr = address_space;
    for (size_t i = 0; i < 3; i++) {
        // kprintf("Travel %x %x %x\n", (int)(uintptr_t) curr, (int) i, curr->entries[l[i]]);
        if (curr->entries[l[i]] == 0) {
            // kprintf("Didn't find page!!!\n");
            return 0;
        }

        curr = (PageTable*) canonical_addr(curr->entries[l[i]] & 0xFFFFFFFFF000ull);
    }

    return curr->entries[l[3]];
}

static bool memmap__translate(PageTable* address_space, uintptr_t virt, u64* out) {
    u64 r = memmap__probe(address_space, virt);
    if ((r & PAGE_PRESENT) == 0) {
        return false;
    }

    *out = (r & ~0xFFFull) | (virt & 0xFFF);
    return true;
}

