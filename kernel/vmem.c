#include <kernel.h>
#include "threads.h"

enum {
    VMEM_WORKING_SET_OFFSET = 1,
};

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
#include <nbhm.h>

static size_t vmem_node_bin_search(VMem_Node* node, uint32_t key) {
    size_t left = 0, right = node->key_count;
    if (node->is_leaf) {
        while (left != right) {
            size_t i = (left + right) / 2;
            uint64_t key_at_idx = node->keys[i];
            if (key_at_idx == key) {
                return i;
            } else if (key_at_idx > key) {
                right = i;
            } else {
                left = i + 1;
            }
        }
        return left - 1;
    } else {
        while (left != right) {
            size_t i = (left + right) / 2;
            uint64_t key_at_idx = node->keys[i];
            if (key_at_idx > key) {
                right = i;
            } else {
                left = i + 1;
            }
        }
        return left;
    }
}

VMem_Cursor vmem_cursor_first(Env* env) {
    VMem_Node* node = env->addr_space.root;
    while (!node->is_leaf) {
        node = node->kids[0];
    }
    return (VMem_Cursor){ node, 0 };
}

VMem_Cursor vmem_cursor_next(VMem_Cursor cur) {
    if (++cur.index == cur.node->key_count) {
        cur.node = cur.node->next;
        cur.index = 0;
    }
    return cur;
}

uintptr_t vmem_cursor_key(VMem_Cursor cur) {
    return cur.node->keys[cur.index];
}

bool vmem_cursor_eq(VMem_Cursor* a, VMem_Cursor* b) {
    return a->node == b->node && a->index == b->index;
}

VMem_Cursor vmem_node_lookup(Env* env, uintptr_t key) {
    VMem_Node* node = env->addr_space.root;
    if (node == NULL) {
        return (VMem_Cursor){ 0 };
    }

    // kprintf("LOOKUP %p %p\n", env, key);

    // layers of binary search
    for (;;) {
        int index = vmem_node_bin_search(node, key);
        if (node->is_leaf) {
            if (index < 0) {
                return (VMem_Cursor){ 0 };
            }

            return (VMem_Cursor){ node, index };
        } else {
            kassert(index <= node->key_count, "OOB %d <= %d", index, node->key_count);
            kassert(node->kids[index] != NULL, "bad B+ tree, %d", index);
            node = node->kids[index];
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

void vmem_cursor_remove(Env* env, VMem_Node* node, int start, int count) {
    if (node != NULL) {
        // kprintf("REMOVE %d\n", count);

        // shift down
        node->key_count -= count;
        FOR_N(i, start + count - 1, node->key_count) {
            node->keys[i] = node->keys[i + count];
            node->vals[i] = node->vals[i + count];
        }
    }
}

void vmem_node_remove(Env* env, uintptr_t key) {
    VMem_Cursor cursor = vmem_node_lookup(env, key);
    vmem_cursor_remove(env, cursor.node, cursor.index, 1);
}

// insert range into B+ tree
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

        int left = vmem_node_bin_search(node, key);
        while (!node->is_leaf) {
            VMem_Node* kid = node->kids[left];
            kassert(kid, "Bad node, %p[%d] = %p (%p)", node, left, kid, key);

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
            left = vmem_node_bin_search(node, key);
        }

        kassert(node->key_count > 0, "shouldn't have empty leaf nodes");
        // kprintf("INSERT @ %p[%d] %p %p\n", node, left, key, node->keys[node->key_count - 1]);

        if (left >= 0 && node->keys[left] == key) {
            return &node->vals[left];
        }
        left += 1;

        // shift up
        FOR_REV_N(j, left, node->key_count) {
            node->keys[j + 1] = node->keys[j];
            node->vals[j + 1] = node->vals[j];
        }

        kassert(node->key_count < VMEM_NODE_MAX_KEYS, "jover");
        node->key_count += 1;

        kassert(left < node->key_count, "jover");
        node->keys[left] = key;
        return &node->vals[left];
    }
}

void vmem_split(Env* env, VMem_Cursor cursor, uintptr_t vaddr) {
    if (cursor.node == NULL) {
        return;
    }

    VMem_PageDesc* desc  = &cursor.node->vals[cursor.index];
    uintptr_t start_addr = cursor.node->keys[cursor.index];
    uintptr_t end_addr   = start_addr + desc->size;
    if (end_addr > vaddr) {
        uintptr_t clip = vaddr - start_addr;
        VMem_PageDesc cpy = *desc;

        if (clip != 0 && clip != desc->size) {
            desc->size = clip;

            // Split into High-half
            VMem_PageDesc* hi = vmem_node_insert(env, vaddr);
            *hi = cpy;
            hi->offset += clip;
            hi->size   -= clip;
        }
    }
}

void vmem_unmap(Env* env, uintptr_t vaddr, size_t size) {
    ON_DEBUG(VMEM)(kprintf("[vmem] unmap(%p, %p, %#zx)\n", env, vaddr, size));

    VMem_Cursor bot_cursor = vmem_node_lookup(env, vaddr);
    if (bot_cursor.node != NULL) {
        VMem_Cursor top_cursor = vmem_node_lookup(env, vaddr + size);
        vmem_split(env, top_cursor, vaddr + size);
        vmem_split(env, bot_cursor, vaddr);

        // Remove any descriptors from the middle of the range
        VMem_Cursor cursor = vmem_node_lookup(env, vaddr);
        ON_DEBUG(VMEM)(kprintf("[vmem] Unmap [%p - %p], Starting at %p\n", vaddr, vaddr + size - 1, vmem_cursor_key(cursor)));

        cursor = vmem_cursor_next(bot_cursor);

        // TODO(NeGate): this is easily some of the most
        // dogshit handling i could do... let's optimize that in the future
        while (cursor.node) {
            size_t key_count = cursor.node->key_count;
            size_t start_i   = cursor.index;
            while (cursor.index < key_count) {
                VMem_PageDesc* desc = &cursor.node->vals[cursor.index];
                uintptr_t start_addr = cursor.node->keys[cursor.index];
                uintptr_t end_addr   = start_addr + desc->size;
                if (start_addr >= vaddr + size) {
                    vmem_cursor_remove(env, cursor.node, start_i, cursor.index - start_i);
                    return;
                }

                ON_DEBUG(VMEM)(kprintf("[vmem] Desc unmap [%p - %p]\n", start_addr, end_addr - 1));
                cursor.index++;
            }

            // Remove the remaining piece of the node
            vmem_cursor_remove(env, cursor.node, start_i, cursor.node->key_count - start_i);

            cursor.node = cursor.node->next;
            cursor.index = 0;
        }
    }
}

void vmem_dump(Env* env) {
    kprintf("MEM DUMP %p\n", env);

    VMem_Cursor cursor = vmem_cursor_first(env);
    while (cursor.node) {
        size_t key_count = cursor.node->key_count;
        while (cursor.index < key_count) {
            VMem_PageDesc* desc = &cursor.node->vals[cursor.index];
            uintptr_t start_addr = cursor.node->keys[cursor.index];
            uintptr_t end_addr   = start_addr + desc->size;

            if (!desc->valid) {
                kprintf("[%p - %p] FREE\n", start_addr, end_addr);
            } else if (desc->vmo) {
                if (desc->vmo->paddr) {
                    kprintf("[%p - %p] VIEW: [OBJ-%d:  %p)\n", start_addr, end_addr, desc->vmo->super.id, desc->vmo->paddr + desc->offset);
                } else {
                    kprintf("[%p - %p] VIEW: [OBJ-%d + %zu)\n", start_addr, end_addr, desc->vmo->super.id, desc->offset);
                }
            } else {
                kprintf("[%p - %p]\n", start_addr, end_addr);
            }
            cursor.index++;
        }
        kprintf("VVV\n");

        cursor.node = cursor.node->next;
        cursor.index = 0;
    }
    kprintf("\n");
}

uintptr_t vmem_map(Env* env, KObject_VMO* vmo, uintptr_t vaddr, size_t offset, size_t size, VMem_Flags flags) {
    kassert((size & (PAGE_SIZE-1)) == 0, "must be page-aligned (%d)", size);

    // walk all regions until we find a gap big enough (SLOW!!!)
    if (vaddr == 0) {
        vaddr = 0xA0000000;
        VMem_Cursor cursor = vmem_node_lookup(env, vaddr);

        // we only want addresses past vaddr so skip one if we're behind it
        if (cursor.node && cursor.node->keys[cursor.index] < vaddr) {
            cursor.index++;
            if (cursor.index >= cursor.node->key_count) {
                cursor.index = 0;
                cursor.node  = cursor.node->next;
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
    } else {
        // Clear out the pages in this range
        vmem_unmap(env, vaddr, size);
    }

    done:
    ON_DEBUG(VMEM)(kprintf("[vmem] map(%p, %#zx) = %p\n", env, size, vaddr));

    VMem_PageDesc* desc = vmem_node_insert(env, vaddr);
    *desc = (VMem_PageDesc){ .valid = 1, .flags = flags, .vmo = vmo, .offset = offset, .size = size };

    if (flags & VMEM_PAGE_PINNED) {
        // commit all the pages now
        char* kaddr = kheap_alloc(size);
        kassert(((uintptr_t) kaddr & (PAGE_SIZE - 1)) == 0, "BAD ALIGN! %p", kaddr);

        memset(kaddr, 0, size);

        FOR_N(i, 0, size / PAGE_SIZE) {
            vmem_commit_page(env, vaddr + i*PAGE_SIZE, kaddr + i*PAGE_SIZE);
        }
        // *out_paddr = kaddr2paddr(kaddr);
    }
    return vaddr;
}

bool vmem_protect(Env* env, uintptr_t addr, size_t size, VMem_Flags flags) {
    return false;
}

void vmem_commit_page(Env* env, uintptr_t vaddr, void* kaddr) {
    vmem_addrhm_put(&env->addr_space.working_set, (void*) (vaddr + VMEM_WORKING_SET_OFFSET), (void*) kaddr2paddr(kaddr));
}

void vmem_add_range(Env* env, KObject_VMO* vmo, uintptr_t vaddr, size_t offset, size_t vsize, VMem_Flags flags) {
    kassert((vaddr & (PAGE_SIZE-1)) == 0, "must be page-aligned (%d)", vaddr);
    kassert((vsize & (PAGE_SIZE-1)) == 0, "must be page-aligned (%d)", vsize);

    vmem_unmap(env, vaddr, vsize);

    VMem_PageDesc* desc = vmem_node_insert(env, vaddr);
    *desc = (VMem_PageDesc){ .valid = 1, .flags = flags, .vmo = vmo, .offset = offset, .size = vsize };
}

uintptr_t vmem_translate(VMem_WorkingSet* ws, uintptr_t vaddr) {
    return (uintptr_t) vmem_addrhm_get(ws, (char*) (vaddr + VMEM_WORKING_SET_OFFSET));
}

uintptr_t vmem_try_commit(Env* env, VMem_PageDesc* desc, uintptr_t access_addr, uintptr_t start_addr, uintptr_t end_addr) {
    // Find the page's working set
    VMem_WorkingSet* ws = &env->addr_space.working_set;
    uintptr_t in_space_addr = access_addr;
    if (desc->vmo != 0) {
        // Translate address into VMO space
        size_t offset = (access_addr & -PAGE_SIZE) - start_addr;
        in_space_addr = desc->offset + offset;

        KObject_VMO* vmo = desc->vmo;
        ON_DEBUG(VMEM)(kprintf("[vmem] first touch on OBJ-%d (%p => VMO:%p)\n", vmo->super.id, access_addr, in_space_addr));

        if (vmo->paddr) {
            // physical addresses don't get cached in the working set, we're
            // better off just not putting entries into a hash map.
            uintptr_t new_page = vmo->paddr + in_space_addr;
            arch_pte_update(env, access_addr & -PAGE_SIZE, new_page, desc->flags);
            return 0;
        }

        // TODO(NeGate): implement pager behavior
        ws = &vmo->pages;
    } else {
        ON_DEBUG(VMEM)(kprintf("[vmem] first touch on private page (%p)\n", access_addr));
    }

    // attempt to commit page in working set
    uintptr_t actual_page = (uintptr_t) vmem_addrhm_get(ws, (void*) (in_space_addr + VMEM_WORKING_SET_OFFSET));
    if (actual_page == 0) {
        void* page = kheap_alloc_page();
        uintptr_t new_page = kaddr2paddr(page);

        actual_page = (uintptr_t) vmem_addrhm_put_if_null(ws, (void*) (in_space_addr + VMEM_WORKING_SET_OFFSET), (void*) new_page);
        if (actual_page != new_page) {
            kheap_free_page(paddr2kaddr(new_page));
        }
    }

    arch_pte_update(env, access_addr & -PAGE_SIZE, actual_page, desc->flags);
    return actual_page;
}

static _Alignas(4096) const uint8_t VMEM_ZERO_PAGE[4096];
bool vmem_segfault(Env* env, uintptr_t access_addr, bool is_write) {
    // we don't care where in the page it's located
    access_addr &= -PAGE_SIZE;

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
        return false;
    }

    size_t pages_to_commit = 1;
    Thread* thread = cpu_get()->current_thread;
    if (access_addr == thread->last_touch.next_addr) {
        size_t readahead = access_addr - thread->last_touch.base_addr;
        if (readahead > 32*1024) {
            readahead = 32*1024;
        }

        size_t dist_to_end = end_addr - access_addr;
        if (readahead > dist_to_end) {
            readahead = dist_to_end;
        }

        if (dist_to_end == 0) {
            // If we can't prefetch anymore, kill the readahead
            ON_DEBUG(VMEM)(kprintf("[vmem] prefetch miss %p\n", access_addr));
            thread->last_touch.base_addr = thread->last_touch.next_addr = 0;
        } else {
            ON_DEBUG(VMEM)(kprintf("[vmem] prefetch hit  %p, commit ahead %zu pages (base=%p, dist=%zu)\n", access_addr, readahead / PAGE_SIZE, thread->last_touch.base_addr, readahead));
            thread->last_touch.next_addr = access_addr + readahead;
            pages_to_commit = readahead / PAGE_SIZE;
        }
    } else {
        ON_DEBUG(VMEM)(kprintf("[vmem] prefetch miss %p\n", access_addr));
        thread->last_touch.base_addr = access_addr;
        thread->last_touch.next_addr = access_addr + PAGE_SIZE;
    }

    // kprintf("%p %zu (%p %p)\n", access_addr, pages_to_commit, start_addr, end_addr);
    FOR_N(i, 0, pages_to_commit) {
        vmem_try_commit(env, desc, access_addr + i*PAGE_SIZE, start_addr, end_addr);
    }
    return true;
}

