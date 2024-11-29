////////////////////////////////
// Virtual memory subsystem
////////////////////////////////
//
// We maintain 3 main structures for the virtual memory:
// * Page range -> properties.
// * Software map from page -> physical address.
// * Hardware page table.

// This is what's stored in the virtual address tree
typedef enum {
    VMEM_PROT_READ   = 1u << 0u,
    VMEM_PROT_WRITE  = 1u << 1u,
    VMEM_PROT_EXEC   = 1u << 2u,
    VMEM_PROT_USER   = 1u << 3u,
} VMem_Prot;

typedef union {
    uint64_t valid : 1;
    uint64_t props : 7;
    // for file mapped memory
    uint64_t file_index : 24;
} VMem_PageDesc;

// commits: CommittedTable
//
// void* page = alloc_page()
// if put_if_null(commits, vaddr, page) != page {
//   // another thread committed a page at the same time
//   free_page(page)
// }
