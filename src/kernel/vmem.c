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

static size_t vmem_node_bin_search(VMem_Node* node, uint32_t key, int start_i) {
    size_t left = start_i, right = node->key_count - 1;
    while (left != right) {
        size_t i = (left + right + 1) / 2;
        if (node->keys[i] > key) { right = i - 1; }
        else { left = i; }
    }

    return left;
}

VMem_Cursor vmem_node_lookup(VMem_AddrSpace* addr_space, uintptr_t key) {
    VMem_Node* node = addr_space->root;
    if (node == NULL) {
        return (VMem_Cursor){ 0 };
    }

    // layers of binary search
    for (;;) {
        uint32_t left = vmem_node_bin_search(node, key, 0);
        if (node->is_leaf) {
            return (VMem_Cursor){ node, left };
        } else {
            node = node->kids[left];
        }
    }
}

void vmem_node_split_child(VMem_Node* x, VMem_Node* y, int idx) {
    VMem_Node* z = kernelfl_alloc(sizeof(VMem_Node) + VMEM_NODE_MAX_VALS*(y->is_leaf ? sizeof(VMem_PageDesc) : sizeof(VMem_Node*)));
    z->next      = NULL;
    z->is_leaf   = y->is_leaf;
    z->key_count = VMEM_NODE_DEGREE - 1;

    for (int i = 0; i < VMEM_NODE_DEGREE; i++) {
        z->keys[i] = y->keys[VMEM_NODE_DEGREE + i];
    }

    // copy higher half of y's values (or kid ptrs)
    if (z->is_leaf) {
        for (int i = 0; i < VMEM_NODE_DEGREE; i++) {
            z->vals[i] = y->vals[VMEM_NODE_DEGREE + i];
        }
    } else {
        for (int i = 0; i < VMEM_NODE_DEGREE; i++) {
            z->kids[i] = y->kids[VMEM_NODE_DEGREE + i];
        }
    }

    y->key_count = VMEM_NODE_DEGREE;
    z->next = y->next;
    y->next = z;

    // shift up
    kassert(!x->is_leaf, "jover");
    for (int i = x->key_count; i >= idx + 1; i--) {
        x->kids[i + 1] = x->kids[i];
    }
    x->kids[idx + 1] = z;

    // A key of y will move to this node. Find the location of
    // new key and move all greater keys one space ahead
    for (int i = x->key_count - 1; i >= idx; i--) {
        x->keys[i + 1] = x->keys[i];
    }

    // Copy the middle key of y to this node
    x->keys[idx] = y->keys[VMEM_NODE_DEGREE];
    x->key_count += 1;
}

// insert range into B-tree
VMem_PageDesc* vmem_node_insert(VMem_AddrSpace* addr_space, uintptr_t key) {
    if (addr_space->root == NULL) {
        // new leaf root
        VMem_Node* node = kernelfl_alloc(sizeof(VMem_Node) + sizeof(VMem_PageDesc)*VMEM_NODE_MAX_VALS);
        node->next      = NULL;
        node->is_leaf   = 1;
        node->key_count = 1;
        node->keys[0] = key;
        addr_space->root = node;
        return &node->vals[0];
    } else {
        VMem_Node* node = addr_space->root;

        // rotate the root
        if (addr_space->root->key_count == VMEM_NODE_MAX_KEYS) {
            VMem_Node* new_node = kernelfl_alloc(sizeof(VMem_Node) + VMEM_NODE_MAX_VALS*sizeof(VMem_Node*));
            new_node->next      = NULL;
            new_node->is_leaf   = 0;
            new_node->key_count = 0;

            // make old root as child of new root
            new_node->kids[0] = addr_space->root;

            // split the old root and move 1 key to the new root
            vmem_node_split_child(new_node, addr_space->root, 0);

            // new root has two children now. decide which of the
            // two children is going to have new key
            int i = 0;
            if (new_node->keys[0] < key) {
                i++;
            }

            node = new_node->kids[i];
            addr_space->root = new_node;
        }

        while (!node->is_leaf) {
            int left = vmem_node_bin_search(node, key, 0);
            VMem_Node* kid = node->kids[left];

            if (kid->key_count == VMEM_NODE_MAX_KEYS) {
                // If the child is full, then split it
                vmem_node_split_child(node, kid, left);

                // After split, the middle key of C[left] goes up and
                // C[left] is splitted into two. See which of the two
                // is going to have the new key
                if (node->keys[left] < key) {
                    kid = node->kids[left + 1];
                }
            }

            node = kid;
        }

        // shift up
        int j = node->key_count;
        while (j-- && node->keys[j] > key) {
            node->keys[j + 1] = node->keys[j];
            node->vals[j + 1] = node->vals[j];
        }

        kassert(node->key_count < VMEM_NODE_MAX_KEYS, "jover");
        node->key_count += 1;

        kassert(j + 1 < node->key_count, "jover");
        node->keys[j + 1] = key;
        return &node->vals[j + 1];
    }
}

uintptr_t vmem_alloc(VMem_AddrSpace* addr_space, size_t size, uintptr_t paddr, VMem_Flags flags) {
    kassert((size & (PAGE_SIZE-1)) == 0, "must be page-aligned (%d)", size);

    // walk all regions until we find a gap big enough (SLOW!!!)
    uintptr_t vaddr = 0xA0000000;
    VMem_Cursor cursor = vmem_node_lookup(addr_space, vaddr);

    // we only want addresses past vaddr so skip one if we're behind it
    if (cursor.node && cursor.node->keys[cursor.index] < vaddr) {
        cursor.index++;
        if (cursor.index >= cursor.node->key_count) {
            cursor.index = 0;
            cursor.node = cursor.node->next;
        }
    }

    int i = cursor.index;
    while (cursor.node) {
        kassert(cursor.node->is_leaf, "fuck");

        int key_count = cursor.node->key_count;
        for (; i < key_count; i++) {
            uintptr_t free_space = cursor.node->keys[i] - vaddr;
            if (free_space > size) {
                ON_DEBUG(VMEM)(kprintf("[vmem] found %d bytes between %p and %p\n", free_space, vaddr, cursor.node->keys[i]));
                goto done;
            }

            // last known address which is free
            vaddr = cursor.node->keys[i] + cursor.node->vals[i].size;
        }

        cursor.node = cursor.node->next;
        i = 0;
    }

    ON_DEBUG(VMEM)(kprintf("[vmem] found no allocations past %p\n", vaddr));

    done:
    VMem_PageDesc* desc = vmem_node_insert(addr_space, vaddr);
    *desc = (VMem_PageDesc){ .valid = 1, .flags = flags, .file_ptr = paddr >> 12ull, .size = size };
    return vaddr;
}

void vmem_add_range(VMem_AddrSpace* addr_space, uintptr_t vaddr, size_t vsize, uintptr_t paddr, VMem_Flags flags) {
    kassert((vaddr & (PAGE_SIZE-1)) == 0, "must be page-aligned (%d)", vaddr);
    kassert((vsize & (PAGE_SIZE-1)) == 0, "must be page-aligned (%d)", vsize);
    kassert((paddr & (PAGE_SIZE-1)) == 0, "must be page-aligned (%d)", paddr);

    VMem_PageDesc* desc = vmem_node_insert(addr_space, vaddr);
    desc->valid    = 1;
    desc->flags    = flags;
    desc->file_ptr = (uintptr_t) (paddr >> 12ull);
    desc->size     = vsize;
}

typedef struct {
    uintptr_t  translated;
    VMem_Flags flags;
} VMem_PTEUpdate;

static _Alignas(4096) const uint8_t VMEM_ZERO_PAGE[4096];
bool vmem_segfault(VMem_AddrSpace* addr_space, uintptr_t access_addr, bool is_write, VMem_PTEUpdate* out_update) {
    VMem_Cursor cursor = vmem_node_lookup(addr_space, access_addr);
    if (cursor.node == NULL) {
        // literally no pages
        return false;
    }

    // check if we're in range
    VMem_PageDesc* desc  = &cursor.node->vals[cursor.index];
    uintptr_t start_addr = cursor.node->keys[cursor.index];
    uintptr_t end_addr   = start_addr + desc->size;
    if (!desc->valid || access_addr >= end_addr) {
        // we're in the gap between page descriptor
        return NULL;
    }

    // attempt to commit page
    uintptr_t actual_page = (uintptr_t) vmem_addrhm_get(&addr_space->commit_table, (void*) access_addr);
    if (actual_page == 0) {
        uintptr_t new_page = 0;
        if (desc->file_ptr != 0) {
            // we're "file" mapped, just view the address
            size_t offset = (access_addr & -PAGE_SIZE) - start_addr;
            new_page = (desc->file_ptr << 12ull) + offset;
        } else {
            void* page = alloc_physical_page();
            memset(page, 0, PAGE_SIZE);
            new_page = kaddr2paddr(page);
        }

        actual_page = (uintptr_t) vmem_addrhm_put_if_null(&addr_space->commit_table, (void*) access_addr, (void*) new_page);
        if (desc->file_ptr != 0) {
            ON_DEBUG(VMEM)(kprintf("[vmem] first touch on file mapped address (%p), paddr=%p\n", access_addr, new_page));
        } else {
            ON_DEBUG(VMEM)(kprintf("[vmem] first touch on private page (%p), paddr=%p\n", access_addr, new_page));
            if (actual_page != new_page) {
                free_physical_chunk(paddr2kaddr(new_page));
            }
        }
    }

    out_update->translated = actual_page;
    out_update->flags = desc->flags;
    return true;
}
