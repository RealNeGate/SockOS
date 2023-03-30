typedef enum Result {
    RESULT_SUCCESS,

    // memory mapping errors
    RESULT_OUT_OF_PAGE_TABLES,
    RESULT_ALLOCATION_UNALIGNED,
} Result;

struct {
    size_t capacity;
    size_t used;
    _Alignas(4096) PageTable tables[512];
} muh_pages = { .capacity = 512 };

static PageTable* get_or_alloc_pt(PageTable* parent, size_t index, int depth) {
    if (parent->entries[index] != 0) {
        for (int i = 0; i < depth; i++) kprintf("  ");
        kprintf("Get old page %x (%x)\n", index, (int) (uintptr_t) parent);

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

