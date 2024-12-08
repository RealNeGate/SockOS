////////////////////////////////
// Virtual memory subsystem
////////////////////////////////
//
// We maintain 3 main structures for the virtual memory:
// * Page range -> properties.
// * Software map from page -> physical address.
// * Hardware page table.
#pragma once
#include <common.h>

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

// B tree nodes
enum {
    VMEM_NODE_DEGREE   = 8,
    VMEM_NODE_MAX_KEYS = VMEM_NODE_DEGREE*2 - 1,
    VMEM_NODE_MAX_VALS = VMEM_NODE_DEGREE*2,
};

typedef struct {
    uint64_t valid : 1;
    uint64_t flags : 7;
    // file mapping info
    uintptr_t file_ptr : 56;
    // virtual range
    size_t size;
} VMem_PageDesc;

typedef struct VMem_Node VMem_Node;
struct VMem_Node {
    VMem_Node* next;

    uint8_t is_leaf   : 1;
    uint8_t key_count : 7;

    uintptr_t keys[VMEM_NODE_MAX_KEYS];
    union {
        VMem_Node* kids[0];    // [VMEM_NODE_MAX_VALS]
        VMem_PageDesc vals[0]; // [VMEM_NODE_MAX_VALS]
    };
};

typedef struct {
    VMem_Node* node;
    size_t index;
} VMem_Cursor;

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
    VMem_Node* root;

    // virtual addresses -> committed pages
    NBHM commit_table;

    // hardware page table
    PageTable* hw_tables;
} VMem_AddrSpace;

