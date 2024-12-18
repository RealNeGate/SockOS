#include <kernel.h>

static Lock heap_lock;
KernelFreeList* kernel_free_list;

static KernelFreeList* kheap_next(KernelFreeList* n) {
    return n->has_next ? (KernelFreeList*) &n->data[n->size - sizeof(KernelFreeList)] : NULL;
}

void kheap_dump(void) {
    kprintf("[kheap] dump\n");
    for (KernelFreeList* list = kernel_free_list; list; list = kheap_next(list)) {
        kassert(list->cookie == 0xABCDABCD, "bad cookie, heap corruption at %p? %p", list, *(u64*) list);
        kprintf("        [%p, %p) size=%d (%s, %s)\n", list, (char*) list + list->size, list->size, list->is_free ? "free" : "used", list->has_next ? "has next" : "end");
    }
}

void* kheap_zalloc(size_t obj_size) {
    void* dst = kheap_alloc(obj_size);
    memset(dst, 0x0, obj_size);
    return dst;
}

void* kheap_alloc(size_t obj_size) {
    spin_lock(&heap_lock);

    // add free list header and 8b padding
    obj_size = (obj_size + 7) & ~7;
    obj_size += sizeof(KernelFreeList);

    for (KernelFreeList* list = kernel_free_list; list; list = kheap_next(list)) {
        if (list->is_free) {
            if (list->size == obj_size) {
                // perfect fit
                list->is_free = false;

                ON_DEBUG(KHEAP)(kprintf("[kheap] alloc(%d) = %p\n", obj_size, &list->data[0]));
                spin_unlock(&heap_lock);
                return &list->data[0];
            } else if (list->size > obj_size) {
                size_t full_size = list->size;

                // split
                bool had_next = list->has_next;
                KernelFreeList* split = (KernelFreeList*) &list->data[obj_size - sizeof(KernelFreeList)];
                list->is_free  = false;
                list->has_next = true;
                list->size = obj_size;

                split->cookie = 0xABCDABCD;
                split->size = full_size - obj_size;
                split->has_next = had_next;
                split->is_free = true;
                split->prev = list;

                ON_DEBUG(KHEAP)(kprintf("[kheap] alloc(%d) = %p - %p\n", obj_size, &list->data[0], &list->data[obj_size - (sizeof(KernelFreeList) + 1)]));
                spin_unlock(&heap_lock);
                return &list->data[0];
            }
        }
    }

    kheap_dump();
    kassert(0, "Ran out of kernel memory");
    spin_unlock(&heap_lock);
}

void kheap_free(void* obj) {
    spin_lock(&heap_lock);
    KernelFreeList* list = &((KernelFreeList*) obj)[-1];
    kassert(!list->is_free, "not allocated... you can't free it");

    ON_DEBUG(KHEAP)(kprintf("[kheap] free(%p)\n", obj));

    list->is_free = true;

    // if there's free space ahead, merge with it
    KernelFreeList* next = kheap_next(list);
    if (next->is_free) {
        list->has_next = next->has_next;
        list->size += next->size;
    }

    KernelFreeList* prev = list->prev;
    if (list->prev && list->prev->is_free) {
        prev->size += list->size;
        prev->has_next = list->has_next;
        list = prev;
    }

    // memset(list->data, 0xCD, list->size - sizeof(KernelFreeList));
    spin_unlock(&heap_lock);
}

void* kheap_realloc(void* obj, size_t obj_size) {
    if (obj == NULL) {
        return kheap_alloc(obj_size);
    }

    KernelFreeList* list = &((KernelFreeList*) obj)[-1];
    if (obj_size == 0) {
        kheap_free(obj);
    }

    // add free list header and 8b padding
    obj_size = (obj_size + 7) & ~7;
    obj_size += sizeof(KernelFreeList);

    // TODO(NeGate): shrink allocation when obj_size is smaller than list->size
    // ...

    // TODO(NeGate): extend current space
    // ...

    // TODO(NeGate): migrate allocation
    // ...
    kassert(0, "TODO");
}

