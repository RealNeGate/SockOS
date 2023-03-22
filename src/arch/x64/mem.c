typedef enum Result {
    RESULT_SUCCESS,

    // memory mapping errors
    RESULT_OUT_OF_PAGE_TABLES,
    RESULT_ALLOCATION_UNALIGNED,
} Result;

struct {
    size_t capacity;
    size_t used;
    PageTable tables[512];
} muh_pages = { .capacity = 512 };

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
        uint64_t pml4_index  = (virt >> 39) & 0x1FF; // 512GB
        uint64_t pdpte_index = (virt >> 30) & 0x1FF; // 1GB
        uint64_t pde_index   = (virt >> 21) & 0x1FF; // 2MB
        uint64_t pte_index   = (virt >> 12) & 0x1FF; // 4KB

        // 512GB
        PageTable* table_l3;
        if (address_space->entries[pml4_index] == 0) {
            // Allocate new L4 entry
            if (muh_pages.used + 1 >= muh_pages.capacity) return RESULT_OUT_OF_PAGE_TABLES;

            table_l3 = &muh_pages.tables[muh_pages.used++];
            memset(table_l3, 0, sizeof(PageTable));

            address_space->entries[pml4_index] = ((uint64_t)table_l3) | 3;
        } else {
            // Used the bits 51:12 as the table address
            table_l3 = (PageTable*)(address_space->entries[pml4_index] & 0xFFFFFFFFFFFFF000);
        }

        // 1GB
        PageTable* table_l2;
        if (table_l3->entries[pdpte_index] == 0) {
            // Allocate new L3 entry
            if (muh_pages.used + 1 >= muh_pages.capacity) return RESULT_OUT_OF_PAGE_TABLES;

            table_l2 = &muh_pages.tables[muh_pages.used++];
            memset(table_l2, 0, sizeof(PageTable));

            table_l3->entries[pdpte_index] = ((uint64_t)table_l2) | 3;
        } else {
            // Used the bits 51:12 as the table address
            table_l2 = (PageTable*)(table_l3->entries[pdpte_index] & 0xFFFFFFFFFFFFF000);
        }

        // 2MB
        PageTable* table_l1;
        if (table_l2->entries[pde_index] == 0) {
            // Allocate new L2 entry
            if (muh_pages.used + 1 >= muh_pages.capacity) return RESULT_OUT_OF_PAGE_TABLES;

            table_l1 = &muh_pages.tables[muh_pages.used++];
            memset(table_l1, 0, sizeof(PageTable));

            table_l2->entries[pde_index] = ((uint64_t)table_l1) | 3;
        } else {
            // Used the bits 51:12 as the table address
            table_l1 = (PageTable*)(table_l2->entries[pde_index] & 0xFFFFFFFFFFFFF000);
        }

        // 4KB
        // | 3 is because we make the pages both PRESENT and WRITABLE
        table_l1->entries[pte_index] = (phys_addr & 0xFFFFFFFFFFFFF000) | 3;
        virt += PAGE_SIZE, phys_addr += PAGE_SIZE;
    }

    *dst = v;
    return RESULT_SUCCESS;
}
