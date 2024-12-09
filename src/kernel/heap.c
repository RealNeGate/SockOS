#include <kernel.h>

KernelFreeList* kernel_free_list;

static KernelFreeList* kheap_next(KernelFreeList* n) {
    return n->has_next ? (KernelFreeList*) &n->data[n->size - sizeof(KernelFreeList)] : NULL;
}

static void kheap_dump(void) {
    kprintf("[kheap] dump\n");
    for (KernelFreeList* list = kernel_free_list; list; list = kheap_next(list)) {
        kprintf("        %p size=%d (%s)\n", &list->data[0], list->size, list->is_free ? "free" : "used");
    }
}

void* kheap_zalloc(size_t obj_size) {
    void* dst = kheap_alloc(obj_size);
    memset(dst, 0x0, obj_size);
    return dst;
}

void* kheap_alloc(size_t obj_size) {
    // add free list header and 8b padding
    obj_size = (obj_size + 7) & ~7;
    obj_size += sizeof(KernelFreeList);

    for (KernelFreeList* list = kernel_free_list; list; list = kheap_next(list)) {
        if (list->is_free) {
            if (list->size == obj_size) {
                // perfect fit
                list->is_free = false;

                ON_DEBUG(KHEAP)(kprintf("[kheap] alloc(%d) = %p\n", obj_size, &list->data[0]));
                return &list->data[0];
            } else if (list->size > obj_size) {
                size_t full_size = list->size;

                // split
                bool had_next = list->has_next;
                KernelFreeList* split = (KernelFreeList*) &list->data[obj_size - sizeof(KernelFreeList)];
                list->is_free  = false;
                list->has_next = true;
                list->size = obj_size;

                split->size = full_size - obj_size;
                split->has_next = had_next;
                split->is_free = true;
                split->prev = list;

                ON_DEBUG(KHEAP)(kprintf("[kheap] alloc(%d) = %p\n", obj_size, &list->data[0]));
                return &list->data[0];
            }
        }
    }

    kassert(0, "Ran out of kernel memory");
}

void kheap_free(void* obj) {
    KernelFreeList* list = &((KernelFreeList*) obj)[-1];
    kassert(!list->is_free, "not allocated... you can't free it");

    list->is_free = true;

    // if there's free space ahead, merge with it
    KernelFreeList* next = kheap_next(list);
    if (next->is_free) {
        list->size += next->size;
    }

    KernelFreeList* prev = list->prev;
    if (list->prev && list->prev->is_free) {
        prev->size += list->size;
        list = prev;
    }

    memset(list->data, 0xCD, list->size - sizeof(KernelFreeList));
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

