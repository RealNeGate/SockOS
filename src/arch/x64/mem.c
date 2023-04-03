typedef enum Result {
    RESULT_SUCCESS,

    // memory mapping errors
    RESULT_OUT_OF_PAGE_TABLES,
    RESULT_ALLOCATION_UNALIGNED,
} Result;

enum {
    BITMAP_ALLOC_WORD_CAP = (4096 - sizeof(size_t[2])) / sizeof(uint64_t),
    BITMAP_ALLOC_SPACE_COVERED = BITMAP_ALLOC_WORD_CAP * 64 * 4096,
};

// this is what we use to allocate physical memory pages, it's a page big
typedef struct BitmapAllocPage BitmapAllocPage;
struct BitmapAllocPage {
    BitmapAllocPage* next;
    // we just track the popcount since once we're at 4 pages left
    uint32_t pages_used, cap;
    uint64_t used[BITMAP_ALLOC_WORD_CAP];
};
_Static_assert(sizeof(BitmapAllocPage) == 4096, "BitmapAllocPage must be a page big");

struct {
    size_t capacity;
    size_t used;
    _Alignas(4096) PageTable tables[512];
} muh_pages = { .capacity = 512 };

static _Alignas(4096) BitmapAllocPage root_alloc_page;

static void init_physical_page_alloc(MemMap* restrict mem_map) {
    #if 0
    FOREACH_N(i, 0, mem_map->nregions) {
        MemRegion* restrict region = &mem_map->regions[i];

        size_t used = sizeof(BitmapAllocPage);
        BitmapAllocPage* page = (BitmapAllocPage*) ;

        while (used < BITMAP_ALLOC_SPACE_COVERED) {
            used += BITMAP_ALLOC_SPACE_COVERED;
        }
    }
    #endif
}

static uintptr_t alloc_physical_page(void) {
    // find page with some
    // BitmapAllocPage* p = &root_alloc_page;
    return 0;
}

static PageTable* get_or_alloc_pt(PageTable* parent, size_t index, int depth) {
    if (parent->entries[index] != 0) {
        for (int i = 0; i < depth; i++) kprintf("  ");
        // kprintf("Get old page %x (%x)\n", index, (int) (uintptr_t) parent);

        return (PageTable*)(parent->entries[index] & 0xFFFFFFFFFFFFF000);
    }

    // Allocate new L3 entry
    if (muh_pages.used + 1 >= muh_pages.capacity) {
        return NULL;
    }

    for (int i = 0; i < depth; i++) kprintf("  ");
    kprintf("Alloc new page %x (%x)\n", index, (int) (uintptr_t) parent);

    PageTable* new_pt = &muh_pages.tables[muh_pages.used++];
    memset(new_pt, 0, sizeof(PageTable));

    kassert(((uintptr_t) new_pt & 0xFFF) == 0 && "page tables must be 4KiB aligned");
    parent->entries[index] = ((uint64_t) new_pt) | 3;
    return new_pt;
}

typedef struct {
    uint64_t paddr;
    uint64_t vaddr;
    bool present;
    bool write;
    bool nex;
    bool user;
    bool accessed;
    bool dirty;
} PageInfo;

PageInfo get_page_info(PageTable* pml4_table, uint64_t vaddr) {
    PageInfo result;
    PageTable* pml3_table = get_or_alloc_pt(pml4_table, (vaddr >> 39) & 0x1FF, 0); // 512GiB
    PageTable* pml2_table = get_or_alloc_pt(pml3_table, (vaddr >> 30) & 0x1FF, 1); // 1GiB
    PageTable* pml1_table = get_or_alloc_pt(pml2_table, (vaddr >> 21) & 0x1FF, 2); // 2MiB
    size_t pte_index = (vaddr >> 12) & 0x1FF; // 4KiB
    uint64_t pte_entry = pml1_table->entries[pte_index];
    result.present = pte_entry & 1;
    result.vaddr = vaddr;
    if (!result.present) {
        return result;
    }
    result.paddr = pte_entry & (uint64_t)0x8ffffffffffff000ull;
    result.write    = (pte_entry >> 1) & 1;
    result.user     = (pte_entry >> 2) & 1;
    result.accessed = (pte_entry >> 5) & 1;
    result.dirty    = (pte_entry >> 6) & 1;
    result.nex      = (pte_entry >> 63) & 1;
    return result;
}

static inline void __native_flush_tlb_single(unsigned long addr) {
    asm volatile("invlpg (%0)" ::"r" (addr) : "memory");
}

// View will map a physical memory region into virtual memory.
static Result memmap__view(PageTable* address_space, uintptr_t phys_addr, size_t size, void** dst) {
    size_t page_count = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (phys_addr & 0xFFFull) {
        return RESULT_ALLOCATION_UNALIGNED;
    }
    uintptr_t virt = boot_info->kernel_virtual_used;
    boot_info->kernel_virtual_used += page_count*PAGE_SIZE;

    // Generate the page table mapping
    void* v = (void*) virt;
    for (size_t i = 0; i < page_count; i++) {
        PageTable* table_l3 = get_or_alloc_pt(address_space, (virt >> 39) & 0x1FF, 0); // 512GiB
        PageTable* table_l2 = get_or_alloc_pt(table_l3,      (virt >> 30) & 0x1FF, 1); // 1GiB
        PageTable* table_l1 = get_or_alloc_pt(table_l2,      (virt >> 21) & 0x1FF, 2); // 2MiB
        size_t pte_index = (virt >> 12) & 0x1FF; // 4KiB

        // | 3 is because we make the pages both PRESENT and WRITABLE
        kprintf("%x\n", phys_addr);
        for(;;){}
        table_l1->entries[pte_index] = (phys_addr & 0xFFFFFFFFFFFFF000) | 3;
        __native_flush_tlb_single(virt);

        virt += PAGE_SIZE, phys_addr += PAGE_SIZE;
    }

    *dst = v;
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
        kprintf("Travel %x %x %x\n", (int)(uintptr_t) curr, (int) i, (int) l[i]);

        if (curr->entries[l[i]] == 0) {
            kprintf("Didn't find page!!!\n");
            return 0;
        }

        curr = (PageTable*) (curr->entries[l[i]] & 0xFFFFFFFFFFFFF000);
    }

    return curr->entries[l[3]];
}
