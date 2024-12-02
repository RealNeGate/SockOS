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
    VMEM_PAGE_READ   = 1u << 0u,
    VMEM_PAGE_WRITE  = 1u << 1u,
    VMEM_PAGE_EXEC   = 1u << 2u,
    VMEM_PAGE_USER   = 1u << 3u,
} VMem_Flags;

typedef struct {
    uint64_t valid : 1;
    uint64_t flags : 7;
} VMem_PageDesc;

typedef struct {
    // [start, end)
    size_t start, end;
    VMem_Flags flags;

    // bootleg file mapping
    uintptr_t paddr;
    size_t psize;
} VMem_Range;

typedef struct {
    // TODO(NeGate): interval trees
    size_t range_count;
    VMem_Range ranges[16];

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

VMem_Range* vmem_add_range(VMem_AddrSpace* addr_space, uintptr_t vaddr, size_t vsize, uintptr_t paddr, size_t psize, VMem_Flags flags) {
    kassert((vaddr & (PAGE_SIZE-1)) == 0, "must be page-aligned (%d)", vaddr);
    kassert((vsize & (PAGE_SIZE-1)) == 0, "must be page-aligned (%d)", vsize);
    kassert((paddr & (PAGE_SIZE-1)) == 0, "must be page-aligned (%d)", paddr);
    kassert((psize & (PAGE_SIZE-1)) == 0, "must be page-aligned (%d)", psize);
    kassert(addr_space->range_count < 16, "too many ranges");

    VMem_Range* range = &addr_space->ranges[addr_space->range_count++];
    range->flags = flags;
    range->start = vaddr;
    range->end   = vaddr + vsize;
    range->paddr = paddr;
    range->psize = psize;
    return range;
}

typedef struct {
    uintptr_t     translated;
    VMem_PageDesc desc;
} VMem_PTEUpdate;

static VMem_Range* vmem_get_page(VMem_AddrSpace* addr_space, uintptr_t addr) {
    for (size_t i = 0; i < addr_space->range_count; i++) {
        if (addr_space->ranges[i].start >= addr && addr < addr_space->ranges[i].end) {
            return &addr_space->ranges[i];
        }
    }

    return NULL;
}

static _Alignas(4096) const uint8_t VMEM_ZERO_PAGE[4096];
bool vmem_segfault(VMem_AddrSpace* addr_space, uintptr_t access_addr, bool is_write, VMem_PTEUpdate* out_update) {
    VMem_Range* r = vmem_get_page(addr_space, access_addr);

    kprintf("SEGFAULT on %p!!!\n", access_addr);
    kprintf("  %d\n", r->flags);

    if (r == NULL) {
        // page not nice :(
        halt();
        return false;
    }

    // attempt to commit page
    void* actual_page = vmem_addrhm_get(&addr_space->commit_table, (void*) access_addr);
    if (actual_page == NULL) {
        void* new_page = NULL;
        if (r->paddr != 0) {
            // we're "file" mapped, just view the address
            size_t offset = (access_addr & -PAGE_SIZE) - r->start;
            new_page = (void*) (r->paddr + offset);

            kprintf("[segfault] file mapped address (%p) was accessed, paddr=%p\n", access_addr, new_page);
        } else {
            void* new_page = alloc_physical_page();
            memset(new_page, 0, PAGE_SIZE);

            kprintf("[segfault] first touch on private page (%p), paddr=%p\n", access_addr, new_page);
        }

        actual_page = vmem_addrhm_put_if_null(&addr_space->commit_table, (void*) access_addr, new_page);
        if (actual_page != new_page && r->paddr == 0) {
            free_physical_chunk(new_page);
        }
    }

    out_update->translated = (uintptr_t) actual_page;
    out_update->desc = (VMem_PageDesc){ .valid = 1, .flags = r->flags };
    return true;
}
