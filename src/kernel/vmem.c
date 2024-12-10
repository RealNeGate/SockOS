#include <kernel.h>

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

static size_t vmem_node_bin_search(VMem_Node* node, uint32_t key, int start_i) {
    size_t left = start_i, right = node->key_count - 1;
    while (left != right) {
        size_t i = (left + right + 1) / 2;
        if (node->keys[i] > key) { right = i - 1; }
        else { left = i; }
    }

    return left;
}

VMem_Cursor vmem_node_lookup(Env* env, uintptr_t key) {
    VMem_Node* node = env->addr_space.root;
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
    VMem_Node* z = kheap_alloc(sizeof(VMem_Node) + VMEM_NODE_MAX_VALS*(y->is_leaf ? sizeof(VMem_PageDesc) : sizeof(VMem_Node*)));
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
VMem_PageDesc* vmem_node_insert(Env* env, uintptr_t key) {
    if (env->addr_space.root == NULL) {
        // new leaf root
        VMem_Node* node = kheap_alloc(sizeof(VMem_Node) + sizeof(VMem_PageDesc)*VMEM_NODE_MAX_VALS);
        node->next      = NULL;
        node->is_leaf   = 1;
        node->key_count = 1;
        node->keys[0] = key;
        env->addr_space.root = node;
        return &node->vals[0];
    } else {
        VMem_Node* node = env->addr_space.root;

        // rotate the root
        if (env->addr_space.root->key_count == VMEM_NODE_MAX_KEYS) {
            VMem_Node* new_node = kheap_alloc(sizeof(VMem_Node) + VMEM_NODE_MAX_VALS*sizeof(VMem_Node*));
            new_node->next      = NULL;
            new_node->is_leaf   = 0;
            new_node->key_count = 0;

            // make old root as child of new root
            new_node->kids[0] = env->addr_space.root;

            // split the old root and move 1 key to the new root
            vmem_node_split_child(new_node, env->addr_space.root, 0);

            // new root has two children now. decide which of the
            // two children is going to have new key
            int i = 0;
            if (new_node->keys[0] < key) {
                i++;
            }

            node = new_node->kids[i];
            env->addr_space.root = new_node;
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

uintptr_t vmem_map(Env* env, KHandle vmo, size_t offset, size_t size, VMem_Flags flags) {
    kassert((size & (PAGE_SIZE-1)) == 0, "must be page-aligned (%d)", size);

    // walk all regions until we find a gap big enough (SLOW!!!)
    uintptr_t vaddr = 0xA0000000;
    VMem_Cursor cursor = vmem_node_lookup(env, vaddr);

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
    VMem_PageDesc* desc = vmem_node_insert(env, vaddr);
    *desc = (VMem_PageDesc){ .valid = 1, .flags = flags, .vmo_handle = vmo, .offset = offset, .size = size };
    return vaddr;
}

bool vmem_protect(Env* env, uintptr_t addr, size_t size, VMem_Flags flags) {
    return false;
}

void vmem_add_range(Env* env, KHandle vmo, uintptr_t vaddr, size_t offset, size_t vsize, VMem_Flags flags) {
    kassert((vaddr & (PAGE_SIZE-1)) == 0, "must be page-aligned (%d)", vaddr);
    kassert((vsize & (PAGE_SIZE-1)) == 0, "must be page-aligned (%d)", vsize);

    VMem_PageDesc* desc = vmem_node_insert(env, vaddr);
    desc->valid      = 1;
    desc->flags      = flags;
    desc->vmo_handle = 0;
    desc->offset     = offset;
    desc->size       = vsize;

    if (vmo != 0) {
        KObject_VMO* vmo_ptr = env_get_handle(env, vmo, NULL);
        kassert(vmo_ptr->super.tag == KOBJECT_VMO, "expected object to be a VMO");
        desc->vmo_handle = vmo;
    }
}

static _Alignas(4096) const uint8_t VMEM_ZERO_PAGE[4096];
bool vmem_segfault(Env* env, uintptr_t access_addr, bool is_write, VMem_PTEUpdate* out_update) {
    VMem_Cursor cursor = vmem_node_lookup(env, access_addr);
    if (cursor.node == NULL) {
        // literally no pages
        return false;
    }

    // check if we're in range
    VMem_PageDesc* desc  = &cursor.node->vals[cursor.index];
    uintptr_t start_addr = cursor.node->keys[cursor.index];
    uintptr_t end_addr   = start_addr + desc->size;
    if (!desc->valid || access_addr < start_addr || access_addr >= end_addr) {
        // we're in the gap between page descriptor
        return NULL;
    }

    ON_DEBUG(VMEM)(kprintf("[vmem] first touch %p, landed in range %p-%p\n", access_addr, start_addr, end_addr-1));

    // attempt to commit page
    uintptr_t actual_page = (uintptr_t) vmem_addrhm_get(&env->addr_space.commit_table, (void*) access_addr);
    if (actual_page == 0) {
        uintptr_t new_page = 0;
        if (desc->vmo_handle != 0) {
            // we're mapped to a VMO, just view the address
            // TODO(NeGate): implement pager behavior.
            KObject_VMO* vmo_ptr = env_get_handle(env, desc->vmo_handle, NULL);

            size_t offset = (access_addr & -PAGE_SIZE) - start_addr;
            new_page = vmo_ptr->paddr + offset;

            ON_DEBUG(VMEM)(kprintf("[vmem] first touch on VMO (%p), paddr=%p\n", access_addr, new_page));
        } else {
            void* page = kpool_alloc_page();
            memset(page, 0, PAGE_SIZE);
            new_page = kaddr2paddr(page);
        }

        actual_page = (uintptr_t) vmem_addrhm_put_if_null(&env->addr_space.commit_table, (void*) access_addr, (void*) new_page);
        if (actual_page != new_page && desc->vmo_handle == 0) {
            ON_DEBUG(VMEM)(kprintf("[vmem] first touch on private page (%p), paddr=%p\n", access_addr, new_page));
            kpool_free_page(paddr2kaddr(new_page));
        }
    }

    out_update->translated = actual_page;
    out_update->flags = desc->flags;
    return true;
}
