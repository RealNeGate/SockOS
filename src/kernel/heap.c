#include <kernel.h>

enum {
    SMALL_SEGMENT_SIZE = 64*1024,
    SEGMENT_SIZE = 4*1024*1024
};

static Lock heap_lock;

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

typedef struct HeapSegment HeapSegment;
struct HeapSegment {
    HeapSegment* next;
    HeapBlock* free;

    bool is_small;
    HeapFreeList* owner;
};

struct Heap {
    // max alloc size of 4096 => 16, 32, 64, 128, 256, 512, 1024, 2048, 4096
    HeapFreeList page_classes[9];
    // variable size, not really interested in fragmentation
    HeapFreeList var_page;
    // small size pages use this to grab segments
    HeapFreeList page_64K;
};

static int heap_size_class(size_t obj_size) {
    return 64 - __builtin_clzll((obj_size - 1) / 16);
}

// check if there's any block in here which can hold the alloc
static void* fl_alloc_fit(HeapFreeList* list, size_t size, size_t align) {
    for (HeapBlock *block = list->free, *prev = NULL; block; prev = block, block = block->next) {
        if (size <= block->size) {
            // Replace free-list node due to imperfect split
            if (size != block->size) {
                kassert(block->size - size >= sizeof(HeapBlock), "fragmentation smaller than HeapBlock");
                HeapBlock* next = (HeapBlock*) ((char*) block + size);
                next->next = block->next;
                next->size = block->size - size;

                // add to segment freelist
                if (prev) {
                    prev->next = next;
                } else {
                    list->free = next;
                }
            }
            list->used += 1;
            return block;
        }
    }
    return NULL;
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

static bool fl_free(HeapFreeList* list, void* obj, size_t size) {
    HeapBlock* block = (HeapBlock*) obj;
    block->size = size;

    bool discard = false;
    if (list->thread_id == cpu_get()->core_id) {
        ON_DEBUG(KHEAP)(kprintf("[kheap] free(%p, %zu)\n", obj, size));

        // Local free
        block->next = list->local_free;
        list->local_free = block;
        list->used -= 1;

        uint64_t thread_freed = atomic_load_explicit(&list->thread_freed, memory_order_relaxed);
        if (list->used == thread_freed) {
            discard = true;
        }
    } else {
        ON_DEBUG(KHEAP)(kprintf("[kheap] deferred_free(%p, %zu)\n", obj, size));

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

static size_t heap_segment_count;
static HeapSegment* heap_segments; // [addr / SEGMENT_SIZE]

static Lock heap_lock;
static HeapSegment* heap_segment_4M;

static Heap local_heaps[MAX_CORES];

void kheap_init(MemMap* restrict mem_map) {
    uintptr_t highest = 0;
    FOR_N(i, 0, mem_map->nregions) {
        MemRegion* restrict region = &mem_map->regions[i];
        if (region->type == MEM_REGION_USABLE && region->base >= 0x100000) {
            uintptr_t high = region->base + region->pages*PAGE_SIZE;
            if (high > highest) { highest = high; }
        }
    }

    heap_segment_count = (highest + SEGMENT_SIZE - 1) / SEGMENT_SIZE;
    size_t requested_pages = ((heap_segment_count * sizeof(HeapSegment)) + PAGE_SIZE - 1) / PAGE_SIZE;

    HeapSegment* tail = NULL;
    ON_DEBUG(KHEAP)(kprintf("[kheap] We have %f MiB of physically mapped space, we need %d KiB for segment arrays\n", highest / (1024.0f * 1024.0f), requested_pages * (PAGE_SIZE / 1024)));

    size_t allocated = 0, available = 0;
    FOR_N(i, 0, mem_map->nregions) {
        MemRegion* restrict region = &mem_map->regions[i];
        size_t pages   = region->pages;
        uintptr_t base = region->base;

        if (region->type == MEM_REGION_USABLE && region->base >= 0x100000) {
            // Chop off some space for the heap page array
            if (heap_segments == NULL && pages >= requested_pages) {
                heap_segments = paddr2kaddr(base);
                memset(heap_segments, 0, requested_pages*PAGE_SIZE);

                base += requested_pages*PAGE_SIZE;
                pages -= requested_pages;
            }

            float mib = (region->pages*PAGE_SIZE) / (1024.0f*1024.0f);
            uintptr_t end = base + pages*PAGE_SIZE;
            ON_DEBUG(KHEAP)(kprintf("[kheap] Region [%p, %p) (%.2f MiB)\n", base, end - 1, mib));

            uintptr_t curr = base & -SEGMENT_SIZE;
            while (curr < end) {
                uintptr_t lo = curr;
                if (lo < base) { lo = base; }

                uintptr_t hi = curr + SEGMENT_SIZE;
                if (hi > end) { hi = end; }

                if (hi - lo == SEGMENT_SIZE) {
                    // ON_DEBUG(KHEAP)(kprintf("         [%zu]: %p - %p\n", lo / SEGMENT_SIZE, lo, hi - 1));

                    HeapSegment* s = &heap_segments[lo / SEGMENT_SIZE];
                    HeapBlock* block = (HeapBlock*) paddr2kaddr(lo);
                    block->next = s->free;
                    block->size = hi - lo;
                    s->free = block;

                    if (tail) {
                        tail->next = s;
                        tail = s;
                    } else {
                        heap_segment_4M = tail = s;
                    }
                    s->owner = NULL;
                    allocated += SEGMENT_SIZE;
                }
                curr = hi;
            }
            available += pages*PAGE_SIZE;
        }
    }
    kassert(heap_segments, "Not enough contiguous space for the heap segment array...");
    kprintf("[kheap] using %.3f MiB out of %.3f MiB (%.3f%%)\n", allocated / (1024.0f * 1024.0f), available / (1024.0f * 1024.0f), ((double) allocated / (double) available) * 100.0f);
}

void kheap_multicore(size_t num_cores) {
    FOR_N(i, 0, num_cores) {
        FOR_N(j, 0, ELEM_COUNT(local_heaps[i].page_classes)) {
            local_heaps[i].page_classes[j].thread_id = i;
        }

        local_heaps[i].var_page.thread_id = i;
        local_heaps[i].page_64K.thread_id = i;
    }
}

void kheap_dump(void) {
    kprintf("[kheap] dump\n");
    /* for (HeapBlock* list = kernel_free_list; list; list = list->next) {
        kprintf("        [%p, %p) size=%d (%s, %s)\n", list, (char*) list + list->size, list->size, list->is_free ? "free" : "used", list->has_next ? "has next" : "end");
    } */
}

void* kheap_zalloc(size_t obj_size) {
    void* dst = kheap_alloc(obj_size);
    memset(dst, 0x0, obj_size);
    return dst;
}

static HeapSegment* gimme_segment(HeapFreeList* list, size_t size, bool is_small, void** out_obj) {
    // take a 4M segment
    spin_lock(&heap_lock);
    HeapSegment* segment = heap_segment_4M;
    kassert(segment, "OOM");
    heap_segment_4M = segment->next;
    spin_unlock(&heap_lock);

    ON_DEBUG(KHEAP)(kprintf("[kheap] Allocating 4M segment: %p\n", segment->free));

    segment->is_small = is_small;
    segment->owner = list;

    char* base = (char*) segment->free;
    if (is_small) {
        // give away the remaining ones to the thread for other allocs
        FOR_REV_N(i, 1, SEGMENT_SIZE / SMALL_SEGMENT_SIZE) {
            HeapBlock* block = (HeapBlock*) (base + (i*SMALL_SEGMENT_SIZE));
            block->next = list->local_free;
            block->size = SMALL_SEGMENT_SIZE;
            list->local_free = block;
        }
    } else {
        HeapBlock* block = (HeapBlock*) (base + size);
        block->next = list->local_free;
        block->size = size;
        list->local_free = block;
    }

    *out_obj = base;
    return segment;
}

void* kheap_alloc(size_t obj_size) {
    int core_id = cpu_get()->core_id;
    Heap* heap = &local_heaps[core_id];

    if (obj_size <= 4096) {
        int size_class = heap_size_class(obj_size);
        obj_size = 16ull << size_class;

        HeapFreeList* list = &heap->page_classes[size_class];
        void* obj = fl_alloc_exact(list);
        if (obj == NULL) {
            HeapSegment* segment;
            obj = fl_alloc_exact(&heap->page_64K);
            if (obj == NULL) {
                segment = gimme_segment(&heap->page_64K, SMALL_SEGMENT_SIZE, true, &obj);
            } else {
                size_t index = kaddr2paddr(obj) / SEGMENT_SIZE;
                kassert(index < heap_segment_count, "we're trying to free an invalid object, %p (index=%d, limit=%d)", obj, index, heap_segment_count);
                segment = &heap_segments[index];
            }

            ON_DEBUG(KHEAP)(kprintf("[kheap] Allocating 64K segment: %p\n", obj));

            // add new entries to the free list
            FOR_REV_N(i, 1, SMALL_SEGMENT_SIZE / obj_size) {
                HeapBlock* block = (HeapBlock*) (((char*) obj) + (i*obj_size));
                block->next = list->local_free;
                block->size = obj_size;
                list->local_free = block;
            }
        }

        ON_DEBUG(KHEAP)(kprintf("[kheap] alloc(%zu) %p\n", obj_size, obj));
        return obj;
    } else {
        obj_size  = (obj_size + 15) & ~15ull;
        void* obj = fl_alloc_fit(&heap->var_page, obj_size, 16);
        if (obj == NULL) {
            gimme_segment(&heap->var_page, obj_size, false, &obj);
            kassert(obj, "OOM");
        }

        ON_DEBUG(KHEAP)(kprintf("[kheap] alloc(%zu) %p\n", obj_size, obj));
        return obj;
    }
}

void kheap_free(void* obj, size_t obj_size) {
    uintptr_t index = kaddr2paddr(obj) / SEGMENT_SIZE;
    kassert(index < heap_segment_count, "we're trying to free an invalid object, %p (index=%d, limit=%d)", obj, index, heap_segment_count);

    int size_class = heap_size_class(obj_size);
    obj_size = 16ull << size_class;

    HeapSegment* segment = &heap_segments[index];
    HeapFreeList* list = segment->owner;
    if (fl_free(list, obj, obj_size)) {
    }
}
