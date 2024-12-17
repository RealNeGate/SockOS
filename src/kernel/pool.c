#include <kernel.h>

static size_t cpu_alloc_queue_mask;

void kpool_init(MemMap* restrict mem_map) {
    size_t total_chunks = 0;
    int biggest = -1;
    FOR_N(i, 0, mem_map->nregions) {
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
    FOR_N(i, 0, mem_map->nregions) {
        MemRegion* restrict region = &mem_map->regions[i];
        if (region->type != MEM_REGION_USABLE || region->base < 0x100000) {
            continue;
        }

        int aa = (region->pages*PAGE_SIZE) / (1024*1024);

        uintptr_t base = region->base;
        uintptr_t end  = (base + region->pages*PAGE_SIZE) & -CHUNK_SIZE;

        // chop off 10% of the biggest memory region for the kernel heap
        if (biggest == i) {
            size_t kernel_heap_size = ((region->pages + 9) / 10) * PAGE_SIZE;
            kprintf("Kernel heap: %p - %p (%d MiB)\n", base, base + kernel_heap_size - 1, kernel_heap_size / (1024*1024));

            kernel_free_list = paddr2kaddr(base);
            kernel_free_list->cookie = 0xABCDABCD;
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

            boot_info->cores[0].alloc.data = paddr2kaddr(base);
            base += pages_per_queue*PAGE_SIZE;
        }

        uintptr_t i = (base + CHUNK_SIZE - 1) & -CHUNK_SIZE;
        if (i < end) {
            kprintf("Region: %p - %p (%d MiB)\n", i, end - 1, aa);

            while (i < end) {
                atomic_store_explicit(&boot_info->cores[0].alloc.data[c++], paddr2kaddr(i), memory_order_relaxed);
                i += CHUNK_SIZE;
            }
        }
    }

    atomic_store_explicit(&boot_info->cores[0].alloc.bot, c, memory_order_relaxed);
}

// hand the other threads portions of the memory
// so one queue is clogged up and the others are starved.
void kpool_subdivide(int num_cores) {
    i64 b = atomic_load_explicit(&boot_info->cores[0].alloc.bot, memory_order_acquire);
    i64 t = atomic_load_explicit(&boot_info->cores[0].alloc.top, memory_order_acquire);

    int per_core = (b - t) / num_cores;
    size_t queue_size = (cpu_alloc_queue_mask + 1) * sizeof(uintptr_t);

    i64 k = t + per_core;
    FOR_N(i, 1, num_cores) {
        boot_info->cores[i].alloc.data = kheap_alloc(queue_size);

        FOR_N(j, 0, per_core) {
            void* ptr = atomic_load_explicit(&boot_info->cores[0].alloc.data[k & cpu_alloc_queue_mask], memory_order_relaxed);

            atomic_store_explicit(&boot_info->cores[i].alloc.data[j], ptr, memory_order_relaxed);
            k = (k + 1) & cpu_alloc_queue_mask;
        }
        atomic_store_explicit(&boot_info->cores[i].alloc.bot, per_core, memory_order_relaxed);
    }

    atomic_store_explicit(&boot_info->cores[0].alloc.bot, per_core, memory_order_relaxed);
}

void* kpool_alloc_chunk(void) {
    PerCPU* cpu = cpu_get();

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

void kpool_free_chunk(void* ptr) {
    PerCPU* cpu = cpu_get();

    // push to local queue
    int64_t b = atomic_load_explicit(&cpu->alloc.bot, memory_order_acquire);
    atomic_store_explicit(&cpu->alloc.data[b & cpu_alloc_queue_mask], ptr, memory_order_release);
    atomic_store_explicit(&cpu->alloc.bot, b + 1, memory_order_release);
}

void* kpool_alloc_page(void) {
    PerCPU* cpu = cpu_get();
    if (cpu->heap == NULL) {
        // allocate chunk, subdivide into 4KiB
        char* dst = kpool_alloc_chunk();
        for (size_t i = CHUNK_SIZE / PAGE_SIZE; i--;) {
            kpool_free_page(&dst[i * PAGE_SIZE]);
        }
    }

    // pop freelist
    PageFreeList* fl = cpu->heap;
    cpu->heap = fl->next;

    ON_DEBUG(KPOOL)(kprintf("[kpool] alloc(%d) = %p\n", PAGE_SIZE, fl));
    memset(fl, 0, PAGE_SIZE);
    return fl;
}

void kpool_free_page(void* ptr) {
    PerCPU* cpu = cpu_get();

    // push to freelist
    PageFreeList* fl = (PageFreeList*) ptr;
    fl->next = cpu->heap;
    cpu->heap = fl;
}

