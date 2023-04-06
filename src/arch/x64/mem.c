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

// this is what we use to allocate physical memory pages, it's a page big
typedef struct BitmapAllocPage BitmapAllocPage;
typedef struct BitmapAllocPageHeader {
    BitmapAllocPage* next;
    // we just track the popcount
    uint32_t popcount, cap;
} BitmapAllocPageHeader;

enum {
    BITMAP_ALLOC_WORD_CAP = (PAGE_SIZE - sizeof(BitmapAllocPageHeader)) / sizeof(uint64_t),
    BITMAP_ALLOC_PAGES_COVERED = BITMAP_ALLOC_WORD_CAP * 64,
};

struct BitmapAllocPage {
    BitmapAllocPageHeader header;
    uint64_t used[BITMAP_ALLOC_WORD_CAP];
};
_Static_assert(sizeof(BitmapAllocPage) == 4096, "BitmapAllocPage must be a page big");

static BitmapAllocPage *alloc_head, *alloc_tail;

static BitmapAllocPage* new_bitmap_alloc_page(uintptr_t p) {
    BitmapAllocPage* page = (BitmapAllocPage*) p;
    memset(page, 0, sizeof(BitmapAllocPage));
    return page;
}

static BitmapAllocPage* append_bitmap_alloc_page(BitmapAllocPage* page) {
    if (alloc_head == NULL) {
        alloc_head = alloc_tail = page;
    } else {
        alloc_tail->header.next = page;
        alloc_tail = page;
    }
    return page;
}

static void mark_bitmap_alloc_page(BitmapAllocPage* page, size_t i) {
    page->used[i / 64] |= (1ull << (i % 64));
    page->header.popcount += 1;
}

static void init_physical_page_alloc(MemMap* restrict mem_map) {
    FOREACH_N(i, 0, mem_map->nregions) {
        MemRegion* restrict region = &mem_map->regions[i];
        if (region->type != MEM_REGION_USABLE || region->base < 0x100000) {
            continue;
        }

        size_t total_pages = region->pages;
        kprintf("Region start: %p (%d pages, %d pages max in freelist)\n", region->base, region->pages, BITMAP_ALLOC_PAGES_COVERED);

        BitmapAllocPage* page = new_bitmap_alloc_page(region->base);
        append_bitmap_alloc_page(page);

        size_t used = 1;
        while (used + BITMAP_ALLOC_PAGES_COVERED + 1 < total_pages) {
            page->header.cap = BITMAP_ALLOC_PAGES_COVERED - 1;
            kprintf("1: cap = %p\n", page->header.cap);

            // highest page is being used to store the next freelist start
            mark_bitmap_alloc_page(page, BITMAP_ALLOC_WORD_CAP - 1);
            used += BITMAP_ALLOC_PAGES_COVERED - 1;

            // move ahead to this new bitmap
            page = append_bitmap_alloc_page(new_bitmap_alloc_page(region->base + used*PAGE_SIZE));
        }

        // trim last page
        page->header.cap = total_pages - used;
        kprintf("2: cap = %p\n", page->header.cap);
    }
}

static BitmapAllocPage* find_physical_alloc(const void* ptr_) {
    const char* ptr = ptr_;

    BitmapAllocPage* p = alloc_head;
    for (; p != NULL; p = p->header.next) {
        // range check
        char* start = (char*) p;
        char* end   = start + p->header.cap*PAGE_SIZE;

        if (ptr >= start && ptr < end) {
            return p;
        }
    }

    return NULL;
}

static void* alloc_physical_page(void) {
    // find page with some empty page first
    BitmapAllocPage* p = alloc_head;
    while (p != NULL && p->header.popcount >= p->header.cap) {
        p = p->header.next;
    }

    // we've run out of physical pages
    if (p == NULL) {
        return NULL;
    }

    // find free bit
    FOREACH_N(i, 0, BITMAP_ALLOC_WORD_CAP) if (p->used[i] != UINT64_MAX) {
        int bit = p->used[i] ? __builtin_ffsll(~p->used[i]) - 1 : 0;
        size_t index = i*64 + bit;

        mark_bitmap_alloc_page(p, index);

        // zero pages here to avoid problems everywhere else
        char* result = ((char*) p) + ((index+1) * PAGE_SIZE);
        memset(result, 0, PAGE_SIZE);
        return result;
    }

    kassert(0, "unreachable");
}

static void* alloc_physical_pages(size_t num_pages) {
    BitmapAllocPage* p = alloc_head;

    for (;;) {
        // find page with some empty page first
        while (p != NULL && p->header.popcount + num_pages > p->header.cap) {
            p = p->header.next;
        }

        // we've run out of physical pages
        if (p == NULL) {
            return NULL;
        }

        // find free bit
        FOREACH_N(i, 0, BITMAP_ALLOC_WORD_CAP) if (p->used[i] != UINT64_MAX) {
            int bit = p->used[i] ? __builtin_ffsll(~p->used[i]) - 1 : 0;
            size_t index = i*64 + bit;

            // check for sequential pages
            FOREACH_N(j, 1, num_pages) {
                size_t index2 = index + j;
                uint64_t mask = (1u << (index2 % 64));

                if (p->used[index2 / 64] & mask) goto bad_region;
            }

            mark_bitmap_alloc_page(p, index);

            // zero pages here to avoid problems everywhere else
            char* result = ((char*) p) + ((index+1) * PAGE_SIZE);
            memset(result, 0, num_pages * PAGE_SIZE);
            return result;

            // couldn't find enough sequential pages
            bad_region:
            break;
        }

        p = p->header.next;
    }
}

static void free_physical_page(const void* ptr) {
    BitmapAllocPage* p = find_physical_alloc(ptr);
    kassert(p != NULL, "attempt to free non-allocated page");

    size_t page_index = (((char*) ptr - (char*) p) / PAGE_SIZE) - 1;
    uint64_t mask = 1ull << (page_index % 64);

    kassert(p->used[page_index / 64] & mask, "double free?");
    p->used[page_index / 64] &= ~mask;
}

static uint64_t canonical_addr(uint64_t ptr) {
    return (ptr >> 48) != 0 ? ptr | (0xFFFull << 48) : ptr;
}

static PageTable* get_or_alloc_pt(PageTable* parent, size_t index, int depth, PageFlags flags) {
    if (parent->entries[index] & PAGE_PRESENT) {
        // for (int i = 0; i < depth; i++) kprintf("  ");
        // kprintf("Get old page %x (%x)\n", index, (int) (uintptr_t) parent);

        if (flags) {
            parent->entries[index] |= flags;
        }

        return (PageTable*) canonical_addr(parent->entries[index] & 0xFFFFFFFFF000ull);
    }

    // Allocate new page entry
    // for (int i = 0; i < depth; i++) kprintf("  ");
    // kprintf("Alloc new page %x (%x)\n", index, (int) (uintptr_t) parent);

    PageTable* new_pt = alloc_physical_page();
    kassert(((uintptr_t) new_pt & 0xFFF) == 0, "page tables must be 4KiB aligned");
    parent->entries[index] = ((uint64_t) new_pt) | flags | PAGE_PRESENT;
    return new_pt;
}

static inline void __native_flush_tlb_single(unsigned long addr) {
    asm volatile("invlpg (%0)" ::"r" (addr) : "memory");
}

// Identity map
static Result memmap__view(PageTable* address_space, uintptr_t phys_addr, uintptr_t virt_addr, size_t size, PageFlags flags) {
    size_t page_count = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    kassert((phys_addr & 0xFFFull) == 0, "physical address unaligned (%p)", phys_addr);
    kassert((virt_addr & 0xFFFull) == 0, "virtual address unaligned (%p)", virt_addr);
    kassert(flags & 0xFFF, "invalid flags (%x)", flags);

    // Generate the page table mapping
    for (size_t i = 0; i < page_count; i++) {
        PageTable* table_l3 = get_or_alloc_pt(address_space, (virt_addr >> 39) & 0x1FF, 0, flags); // 512GiB
        PageTable* table_l2 = get_or_alloc_pt(table_l3,      (virt_addr >> 30) & 0x1FF, 1, flags); // 1GiB
        PageTable* table_l1 = get_or_alloc_pt(table_l2,      (virt_addr >> 21) & 0x1FF, 2, flags); // 2MiB
        size_t pte_index = (virt_addr >> 12) & 0x1FF; // 4KiB

        table_l1->entries[pte_index] = (phys_addr & 0xFFFFFFFFF000) | flags | PAGE_PRESENT;
        __native_flush_tlb_single(virt_addr);

        virt_addr += PAGE_SIZE, phys_addr += PAGE_SIZE;
    }

    return RESULT_SUCCESS;
}

static void memdump(uint64_t *buffer, size_t size) {
    int scale = 16;
    int max_pixel = boot_info->fb.width * boot_info->fb.height;
    int width = boot_info->fb.width;

    for (int i = 0; i < size; i++) {
        uint32_t color = 0xFF000000 | ((buffer[i] > 0) ? buffer[i] : 0xFF050505);

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

static uint64_t memmap__probe(PageTable* address_space, uintptr_t virt) {
    size_t l[4] = {
        (virt >> 39) & 0x1FF,
        (virt >> 30) & 0x1FF,
        (virt >> 21) & 0x1FF,
        (virt >> 12) & 0x1FF,
    };

    PageTable* curr = address_space;
    for (size_t i = 0; i < 3; i++) {
        kprintf("Travel %x %x %x\n", (int)(uintptr_t) curr, (int) i, curr->entries[l[i]]);

        if (curr->entries[l[i]] == 0) {
            // kprintf("Didn't find page!!!\n");
            return 0;
        }

        curr = (PageTable*) canonical_addr(curr->entries[l[i]] & 0xFFFFFFFFF000ull);
    }

    return curr->entries[l[3]];
}
