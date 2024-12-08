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

#define NBHM_VIRTUAL_ALLOC(size)     memset(kernelfl_alloc(size), 0, size)
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

// bottom bit means there's an exclusive lock.
typedef _Atomic(uint32_t) RWLock;

typedef struct {
    // read-write lock is needed since multiple "forward progress" modifications can occur at once, while only
    // one "backwards progress" can occur exclusively.
    //
    //   "forward progress" in this context means the modification to the page table will produce at the same
    //   or less segfaults as the old form, the reason this is important is because it means we can hide the
    //   act of TLB updates and invalidation behind segfaults if a core were to see an out-dated view of the page
    //   table. A simple example
    //
    //   "backwards progress" requires everyone to acknowledge the changes. For instance, unmapping a page requires everyone
    //   to acknowledge it or else the data might be corrupted, miss an important segfault.
    RWLock lock;

    // TODO(NeGate): interval trees
    size_t range_count;
    VMem_Range ranges[16];

    // virtual addresses -> committed pages
    NBHM commit_table;

    // hardware page table
    PageTable* hw_tables;
} VMem_AddrSpace;

uint32_t vmem_addrhm_hash(const void* k) {
    uint32_t* addr = (uint32_t*) &k;

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
    return a == b;
}

#define NBHM_FN(n) vmem_addrhm_ ## n
#define NBHM_IMPL
#include <nbhm.h>

void rwlock_lock_shared(RWLock* lock) {
    uint32_t old;
    do {
        old = atomic_load_explicit(lock, memory_order_acquire);
        // spin lock until exclusive hold is gone
        if (old == 1) { continue; }
        // if there's no hold, let's take it (or if there's one we should tick up)
    } while (!atomic_compare_exchange_strong(lock, &old, old + 2));
}

void rwlock_unlock_shared(RWLock* lock) {
    atomic_fetch_sub_explicit(lock, 2, memory_order_acq_rel);
}

void rwlock_lock_exclusive(RWLock* lock) {
    while (!atomic_compare_exchange_strong(lock, &(uint32_t){ 0 }, 1)) {
        // spin lock...
        asm volatile ("pause");
    }
}

void rwlock_unlock_exclusive(RWLock* lock) {
    atomic_store_explicit(lock, 0, memory_order_release);
}

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
        if (addr >= addr_space->ranges[i].start && addr < addr_space->ranges[i].end) {
            return &addr_space->ranges[i];
        }
    }

    return NULL;
}

static _Alignas(4096) const uint8_t VMEM_ZERO_PAGE[4096];
bool vmem_segfault(VMem_AddrSpace* addr_space, uintptr_t access_addr, bool is_write, VMem_PTEUpdate* out_update) {
    VMem_Range* r = vmem_get_page(addr_space, access_addr);

    if (r == NULL) {
        // page not nice :(
        return false;
    }

    // attempt to commit page
    uintptr_t actual_page = (uintptr_t) vmem_addrhm_get(&addr_space->commit_table, (void*) access_addr);
    if (actual_page == 0) {
        uintptr_t new_page = 0;
        if (r->paddr != 0) {
            // we're "file" mapped, just view the address
            size_t offset = (access_addr & -PAGE_SIZE) - r->start;
            new_page = r->paddr + offset;
        } else {
            void* page = alloc_physical_page();
            memset(page, 0, PAGE_SIZE);
            new_page = kaddr2paddr(page);
        }

        actual_page = (uintptr_t) vmem_addrhm_put_if_null(&addr_space->commit_table, (void*) access_addr, (void*) new_page);
        if (r->paddr != 0) {
            kprintf("[vmem] first touch on file mapped address (%p), paddr=%p\n", access_addr, new_page);
        } else {
            kprintf("[vmem] first touch on private page (%p), paddr=%p\n", access_addr, new_page);
            if (actual_page != new_page) {
                free_physical_chunk(paddr2kaddr(new_page));
            }
        }
    }

    out_update->translated = actual_page;
    out_update->desc = (VMem_PageDesc){ .valid = 1, .flags = r->flags };
    return true;
}
