// The kernel heap is divided into two parts, one is a core-local queue
// holding segments (2MiB chunks). Then a nested allocator for smaller
// sizes.
//
// Something we need to account for is the fact that a lot of allocations
// are movable given we notify whoever is responsible.
#include <kernel.h>

enum {
    SMALL_SEGMENT_SIZE = 64*1024,
    SEGMENT_SIZE = 2*1024*1024
};

typedef struct {
    _Atomic int64_t bot;
    _Atomic int64_t top;
    _Atomic(void*)* data;
} SegmentPool;

typedef struct HeapBlock HeapBlock;
struct HeapBlock {
    HeapBlock* next;
    size_t size;
    char data[];
};

typedef struct HeapFreeList HeapFreeList;
struct HeapFreeList {
    uint32_t thread_id;
    uint64_t used;

    HeapBlock* free;       // List we allocate from.
    HeapBlock* local_free; // List holding new free allocs.

    // List holding new free allocs from other threads
    _Atomic(HeapBlock*) thread_free;
    atomic_u64 thread_freed;
};

struct Heap {
    // max alloc size of 4096 => 64, 128, 256, 512, 1024, 2048, 4096.
    //   these are sub-allocations from page_64K alloc.
    HeapFreeList page_classes[7];

    // variable size, not really interested in fragmentation
    HeapFreeList var_page;
    // small size pages use this to grab segments
    HeapFreeList page_64K;
    // used by page allocator
    HeapFreeList fixed_page;
};

static int heap_size_class(size_t obj_size) {
    if (obj_size < 64) {
        return 0;
    }
    return 64 - __builtin_clzll((obj_size - 1) / 64);
}

static uint64_t segment_pool_mask;
static SegmentPool segment_pools[MAX_CORES];

static Heap local_heaps[MAX_CORES];

static size_t segment_map_len;
static Heap** segment_map;

void kheap_init(MemMap* mem_map) {
    int core_id = cpu_get_index();
    kassert(core_id == 0, "Just kinda assumed alright!");

    size_t total_chunks = 0;
    uintptr_t highest_addr = 0;
    FOR_N(i, 0, mem_map->nregions) {
        MemRegion* region = &mem_map->regions[i];
        if (region->type != MEM_REGION_USABLE || region->base < 0x100000) {
            continue;
        }

        uintptr_t end  = (region->base + region->pages*PAGE_SIZE) & -SEGMENT_SIZE;
        if (end > highest_addr) {
            highest_addr = end;
        }

        total_chunks += region->pages / (SEGMENT_SIZE / PAGE_SIZE);
    }
    segment_map_len = (highest_addr + SEGMENT_SIZE - 1) / SEGMENT_SIZE;

    int64_t shift     = 64 - __builtin_clzll(total_chunks - 1);
    segment_pool_mask = (1ull << shift) - 1;

    size_t queue_size  = (((1ull << shift)*8) + PAGE_SIZE - 1);
    size_t total_waste = 0;
    ON_DEBUG(KHEAP)(kprintf("[heap] Physical memory (%d chunks, %d KiB / queue)\n", (int)total_chunks, (queue_size + 512) / 1024));

    _Atomic(void*)* queue_arr = NULL;
    size_t queue_cnt = 0;
    size_t segment_map_size = segment_map_len*sizeof(Heap*);
    FOR_N(i, 0, mem_map->nregions) {
        MemRegion* restrict region = &mem_map->regions[i];
        if (region->type != MEM_REGION_USABLE || region->base < 0x100000) {
            continue;
        }

        uintptr_t base = region->base;
        uintptr_t end  = (base + region->pages*PAGE_SIZE) & -SEGMENT_SIZE;

        // allocate enough space for the first pool
        if (queue_arr == NULL && end >= base + queue_size) {
            ON_DEBUG(KHEAP)(kprintf("[heap] Allocated queue-0 @ %p (%#zx bytes)\n", base, queue_size));
            queue_arr = paddr2kaddr(base);

            segment_pools[0].data = queue_arr;
            base += queue_size;
        }

        // allocate segment map
        if (segment_map == NULL && end >= base + segment_map_size) {
            ON_DEBUG(KHEAP)(kprintf("[heap] Allocated segment map @ %p (%#zx bytes)\n", base, segment_map_size));

            segment_map = paddr2kaddr(base);
            base += segment_map_size;
        }

        // give away all remaining aligned chunks to the
        uintptr_t old = base;
        base = (base + SEGMENT_SIZE - 1) & -SEGMENT_SIZE;
        total_waste += base - old;

        if (base < end) {
            ON_DEBUG(KHEAP)(kprintf("Region:  %p - %p (%zu MiB)\n", base, end - 1, ((end - base) + (1u<<19u)) / (1u<<20u)));

            size_t segment_count = (end - base) / SEGMENT_SIZE;
            FOR_REV_N(i, 0, segment_count) {
                uintptr_t ptr = base + i*SEGMENT_SIZE;
                atomic_strlx(&queue_arr[queue_cnt++], paddr2kaddr(ptr));
            }
        }
    }

    ON_DEBUG(KHEAP)(kprintf("Total waste: %zu bytes (%zu KiB)\n", total_waste, (total_waste + 512) / 1024));
    atomic_strlx(&segment_pools[0].bot, queue_cnt);
    atomic_thread_fence(memory_order_release);
}

void kheap_multicore(size_t num_cores) {
    int core_id = cpu_get_index();
    kassert(core_id == 0, "Just kinda assumed alright!");

    int64_t b = atomic_ldacq(&segment_pools[0].bot);
    int64_t t = atomic_ldacq(&segment_pools[0].top);

    int per_core = (b - t) / num_cores;
    size_t queue_size = (segment_pool_mask + 1) * sizeof(void*);

    uint64_t mask = segment_pool_mask;
    FOR_N(i, 1, num_cores) {
        _Atomic(void*)* queue_arr = kheap_alloc(queue_size);
        FOR_N(j, 0, per_core) {
            void* ptr = atomic_ldrlx(&segment_pools[0].data[t & mask]);
            atomic_strlx(&queue_arr[j], ptr);
            t = (t + 1) & mask;
        }
        segment_pools[i].data = queue_arr;
        atomic_strlx(&segment_pools[i].bot, per_core);
    }
    ON_DEBUG(KHEAP)(kprintf("[heap] Core0 remaining %zu, Per-Core %zu\n", b - t, per_core));

    atomic_strlx(&segment_pools[0].top, t);
    atomic_thread_fence(memory_order_seq_cst);

    // local heaps need to know where they belong, used for cross-core freeing
    FOR_N(i, 0, num_cores) {
        FOR_N(j, 0, ELEM_COUNT(local_heaps[i].page_classes)) {
            local_heaps[i].page_classes[j].thread_id = i;
        }

        local_heaps[i].var_page.thread_id = i;
        local_heaps[i].page_64K.thread_id = i;
    }
}

// TODO(NeGate): when this function fails, it should look at its
// neighbors (in an ordering favorable to the cache topology) and
// try to steal memory.
static void* alloc_segment(void) {
    int core_id = cpu_get_index();
    SegmentPool* pool = &segment_pools[core_id];

    // pop from local queue
    int64_t bot = atomic_ldacq(&pool->bot) - 1;
    atomic_strlx(&pool->bot, bot);

    int64_t top = atomic_ldrlx(&pool->top);
    if (top <= bot) {
        // queue isn't empty
        void* ptr = atomic_ldacq(&pool->data[bot & segment_pool_mask]);
        if (top == bot) {
            // race other cores for final entry in the queue
            bool succ = atomic_cas_acq_rel(&pool->top, &top, top + 1);
            atomic_strel(&pool->bot, bot + 1);
            return succ ? ptr : NULL;
        }
        return ptr;
    } else {
        atomic_strel(&pool->bot, bot + 1);
        return NULL;
    }
}

static void free_segment(void* ptr) {
    int core_id = cpu_get_index();
    SegmentPool* pool = &segment_pools[core_id];

    // push to local queue
    int64_t b = atomic_ldacq(&pool->bot);
    atomic_strlx(&pool->data[b & segment_pool_mask], ptr);
    atomic_thread_fence(memory_order_release);
    atomic_strlx(&pool->bot, b + 1);
}

// every block in this page is capable of holding the alloc
static void* fl_alloc_exact(HeapFreeList* list) {
    HeapBlock* block = list->free;
    if (block == NULL) {
        // no blocks? "compact" the lists together
        list->free = list->local_free;
        list->local_free = NULL;

        // append the stuff that other threads freed
        HeapBlock* other = atomic_exchange_explicit(&list->thread_free, NULL, memory_order_acq_rel);
        if (other != NULL) {
            other->next = list->free;
            list->free = other;
        }

        block = list->free;
        if (block == NULL) {
            return NULL;
        }
    }
    list->free = block->next;
    list->used += 1;
    return block;
}

// check if there's any block in here which can hold the alloc
static void* fl_alloc_fit(HeapFreeList* list, size_t size, size_t align) {
    // we shouldn't really call this everytime
    if (1) {
        // no blocks? "compact" the lists together
        list->free = list->local_free;
        list->local_free = NULL;

        // append the stuff that other threads freed
        HeapBlock* other = atomic_exchange_explicit(&list->thread_free, NULL, memory_order_acq_rel);
        if (other != NULL) {
            other->next = list->free;
            list->free = other;
        }
    }

    for (HeapBlock *block = list->free, *prev = NULL; block; prev = block, block = block->next) {
        uintptr_t start = (uintptr_t) block;
        uintptr_t next_used = start + block->size;

        // round up
        start = (start + align - 1) & -align;

        uintptr_t end = start + size;
        if (end <= next_used) {
            // Replace free-list node due to imperfect split
            if (size != block->size) {
                kassert(next_used - end >= sizeof(HeapBlock), "fragmentation smaller than HeapBlock");
                HeapBlock* next = (HeapBlock*) end;
                next->next = list->local_free;
                next->size = next_used - end;
                list->local_free = next;

                if (prev) {
                    prev->next = block->next;
                } else {
                    list->free = block->next;
                }
            }
            list->used += 1;
            return block;
        }
    }
    return NULL;
}

static void* gimme_segment(Heap* heap, HeapFreeList* list, size_t size, bool is_small) {
    char* segment = alloc_segment();
    if (segment == NULL) {
        return NULL;
    }

    ON_DEBUG(KHEAP)(kprintf("[heap] Allocating 2M segment, split into %zuK: %p\n", kaddr2paddr(segment), size / 1024));
    if (is_small) {
        // give away the remaining ones to the thread for other allocs
        FOR_REV_N(i, 1, SEGMENT_SIZE / size) {
            HeapBlock* block = (HeapBlock*) &segment[i*size];
            block->next = list->local_free;
            block->size = size;
            list->local_free = block;
        }
    } else {
        HeapBlock* block = (HeapBlock*) (segment + size);
        block->next = list->local_free;
        block->size = SEGMENT_SIZE - size;
        list->local_free = block;
    }

    uintptr_t i = kaddr2paddr(segment) / SEGMENT_SIZE;
    kassert(i < segment_map_len, "OOB!");
    segment_map[i] = heap;
    return segment;
}

void* kheap_alloc_page(void) {
    int core_id = cpu_get_index();
    Heap* heap  = &local_heaps[core_id];
    void* page  = fl_alloc_exact(&heap->fixed_page);
    if (page == NULL) {
        page = gimme_segment(heap, &heap->fixed_page, PAGE_SIZE, true);
    }

    memset(page, 0, PAGE_SIZE);
    return page;
}

void kheap_free_page(void* ptr) {

}

void* kheap_alloc(size_t obj_size) {
    int core_id = cpu_get_index();
    Heap* heap  = &local_heaps[core_id];

    if (obj_size < 4096) {
        int size_class  = heap_size_class(obj_size);
        size_t old_size = obj_size;
        obj_size = 64ull << size_class;
        kassert(obj_size >= old_size, "woah");

        HeapFreeList* list = &heap->page_classes[size_class];
        void* obj = fl_alloc_exact(list);
        if (obj == NULL) {
            char* small_segment = fl_alloc_exact(&heap->page_64K);
            if (small_segment == NULL) {
                small_segment = gimme_segment(heap, &heap->page_64K, SMALL_SEGMENT_SIZE, true);
                kassert(small_segment, "OOM");
            }

            // add new entries to the free list
            FOR_REV_N(i, 1, SMALL_SEGMENT_SIZE / obj_size) {
                HeapBlock* block = (HeapBlock*) &small_segment[i*obj_size];
                block->next = list->local_free;
                block->size = obj_size;
                list->local_free = block;
            }
            obj = small_segment;
        }

        ON_DEBUG(KHEAP)(kprintf("[heap] alloc(%zu => %zu) %p\n", old_size, obj_size, obj));
        // kprintf("=== ALLOC %p %zu (%p) ===\n", obj, obj_size, list);
        return obj;
    } else {
        obj_size  = (obj_size + 4095) & ~4095ull;

        void* obj = fl_alloc_fit(&heap->var_page, obj_size, 4096);
        if (obj == NULL) {
            kassert(obj_size <= SEGMENT_SIZE, "Too much to alloc together!!! %zu", obj_size);
            obj = gimme_segment(heap, &heap->var_page, obj_size, false);
            kassert(obj, "OOM");
        }

        ON_DEBUG(KHEAP)(kprintf("[heap] alloc(%zu) %p\n", obj_size, obj));
        // kprintf("=== ALLOC %p %zu (%p) ===\n", obj, obj_size, &heap->var_page);
        return obj;
    }
}

static bool fl_free(HeapFreeList* list, void* obj, size_t size) {
    HeapBlock* block = (HeapBlock*) obj;
    block->size = size;

    bool discard = false;
    if (list->thread_id == cpu_get_index()) {
        ON_DEBUG(KHEAP)(kprintf("[heap] free(%p, %zu)\n", obj, size));

        // Local free
        block->next = list->local_free;
        list->local_free = block;
        list->used -= 1;

        uint64_t thread_freed = atomic_load_explicit(&list->thread_freed, memory_order_relaxed);
        if (list->used == thread_freed) {
            discard = true;
        }
    } else {
        ON_DEBUG(KHEAP)(kprintf("[heap] deferred_free(%p, %zu)\n", obj, size));

        // Non-local free, queue up for the owner thread to handle it
        do {
            block->next = list->thread_free;
        } while (!atomic_cas_acq_rel(&list->thread_free, &(HeapBlock*){ block }, block->next));

        atomic_fetch_add_explicit(&list->thread_freed, 1, memory_order_acq_rel);
    }

    #ifndef NDEBUG
    memset(block->data, 0xCC, size - sizeof(HeapBlock));
    #endif

    return discard;
}

void kheap_free(void* obj, size_t obj_size) {
    uintptr_t index = kaddr2paddr(obj) / SEGMENT_SIZE;
    kassert(index < segment_map_len, "we're trying to free an invalid object, %p (index=%d, limit=%d)", obj, index, segment_map_len);

    Heap* heap = segment_map[index];
    kassert(heap, "Not a segment associated with a heap");

    // The top-level free list of the segment can be either page_64K if obj_size
    // is less than 4K, var_page if not.
    HeapFreeList* list = NULL;
    if (obj_size < 4096) {
        int size_class = heap_size_class(obj_size);
        obj_size = 64ull << size_class;
        list     = &heap->page_classes[size_class];
    } else {
        obj_size = (obj_size + 4095) & ~4095ull;
        list     = &heap->var_page;
    }

    // kprintf("=== FREE %p %zu (%p) ===\n", obj, obj_size, list);
    fl_free(list, obj, obj_size);
}

void* kheap_zalloc(size_t obj_size) {
    void* dst = kheap_alloc(obj_size);
    memset(dst, 0x0, obj_size);
    return dst;
}
