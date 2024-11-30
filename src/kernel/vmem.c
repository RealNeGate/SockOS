////////////////////////////////
// Virtual memory subsystem
////////////////////////////////
//
// We maintain 3 main structures for the virtual memory:
// * Page range -> properties.
// * Software map from page -> physical address.
// * Hardware page table.

// Forward decls of the arch-specific side of memory
static void* kernelfl_realloc(void* obj, size_t obj_size);
static void* kernelfl_alloc(size_t obj_size);
static void kernelfl_free(void* obj);

#define NBHM_VIRTUAL_ALLOC(size)     kernelfl_alloc(size)
#define NBHM_VIRTUAL_FREE(ptr, size) kernelfl_free(ptr)
#define NBHM_ASSERT(x) kassert(x, ":(")
#define NBHM_REALLOC(ptr, size) kernelfl_realloc(ptr, size)
#include <nbhm.h>

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

typedef struct {
    // interval tree

    // virtual addresses -> committed pages
    NBHM commit_table;

    // hardware page table
    PageTable* hw_tables;
} VMem_AddrSpace;

uint32_t vmem_addrhm_hash(const void* k) {
    uint32_t* addr = (uint32_t*) k;

    // simplified murmur3 32-bit
    uint32_t h = 0;
    for (size_t i = 0; i < sizeof(uintptr_t) / sizeof(uint32_t); i++) {
        uint32_t k = addr[i]*0xcc9e2d51;
        k = ((k << 15) | (k >> 17))*0x1b873593;
        h = (((h^k) << 13) | ((h^k) >> 19))*5 + 0xe6546b64;
    }

    // finalization mix, including key length
    size_t len = sizeof(uintptr_t);
    h = ((h^len) ^ ((h^len) >> 16))*0x85ebca6b;
    h = (h ^ (h >> 13))*0xc2b2ae35;
    return h ^ (h >> 16);
}

bool vmem_addrhm_cmp(const void* a, const void* b) {
    uintptr_t aa = *(uintptr_t*) a;
    uintptr_t bb = *(uintptr_t*) b;
    return aa == bb;
}

#define NBHM_FN(n) vmem_addrhm_ ## n
#define NBHM_IMPL
#include <nbhm.h>

// commits: CommittedTable
//
// void* page = alloc_page()
// if put_if_null(commits, vaddr, page) != page {
//   // another thread committed a page at the same time
//   free_page(page)
// }


// _ _
//

