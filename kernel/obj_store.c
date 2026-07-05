// Multi-head logs, we call that layer C0. When C0 is full, it'll flush
// out to C1 which is a list of logs that are uncompacted. Any two logs
// can be merged since we have a strong monotonic guarantee from the IDs.
#include "kernel.h"
#include "threads.h"
#include "pci.h"

bool objstore_hm_cmp(const void* a, const void* b) {
    return a == b;
}

uint32_t objstore_hm_hash(void* k) {
    uint64_t h = (uint64_t) k;
    h ^= h >> 33ull;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33ull;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33ull;
    return h;
}

#define NBHM_FN(n) objstore_hm_ ## n
#include <nbhm.h>

static atomic_u64 OBJ_ID_CNT = 0;
static NBHM global_store;

// rip off top bit
static void* strip_ptr(void* p) {
    return (void*) ((uintptr_t) p & ~EBR_PRIME_BIT);
}

// reconstruct top bit (should be the same as bit below it)
static void* unstrip_ptr(void* p) {
    uintptr_t pp = (uintptr_t) p;
    pp |= (pp & (EBR_PRIME_BIT >> 1ull)) << 1ull;
    return (void*) pp;
}

void store_alloc(void) {
    global_store = nbhm_alloc(4000);
}

KObjectID store_put(KObject* obj) {
    KObjectID id = obj->id = ++OBJ_ID_CNT;
    objstore_hm_put(&global_store, (void*) id, strip_ptr(obj));
    return id;
}

KObject* store_get(KObjectID id) {
    void* p = objstore_hm_get(&global_store, (void*) id);
    return unstrip_ptr(p);
}

// TODO(NeGate): we want this data sorted...
void store_iter(void fn(KObjectID id, KObject* obj)) {
    objstore_hm_resize_barrier(&global_store);
    nbhm_for(it, &global_store) {
        fn((KObjectID) it->key, unstrip_ptr(it->val));
    }
}

static void print_obj(KObjectID id, KObject* obj) {
    kprintf("%-14ld %-16s %p  ", id, kobject_name(obj), obj);
    if (obj->tag == KOBJECT_THREAD) {
        Thread* t = (Thread*) obj;
        Env* env  = t->parent;

        const char* status = "???";
        switch (t->client.status) {
            case CLIENT_FRESH:   status = "Fresh"; break;
            case CLIENT_BLOCKED: status = "Blocked"; break;
            case CLIENT_READY:   status = "Running"; break;
            case CLIENT_ZOMBIE:  status = "Zombie"; break;
        }
        kprintf("%-14ld %s", env ? env->super.id : 0, status);

        if (t->client.is_dead) {
            kprintf(" (Just died)");
        } else if (t->client.is_blocked) {
            kprintf(" (Just blocked)");
        }
    } else if (obj->tag == KOBJECT_DEV_PCI) {
        PCI_Device* pci = (PCI_Device*) obj;
        kprintf("%-14s %04x:%04x", "_", pci->vendor_id, pci->device_id);
    } else {
        kprintf("%-14s", "_");
    }
    kprintf("\n");
}

// https://en.wikipedia.org/wiki/Quicksort
static int partition(KObject** A, int lo, int hi) {
    KObjectID pivot = A[lo]->id;
    int i = lo - 1, j = hi + 1;
    for (;;) {
        // move left index to the right
        do { i += 1; } while (A[i]->id < pivot);
        // move right index to the left
        do { j -= 1; } while (A[j]->id > pivot);
        // if indices cross, done
        if (i >= j) { return j; }
        // swap
        KObject* tmp = A[i];
        A[i] = A[j];
        A[j] = tmp;
    }
}

static void quicksort(KObject** A, int lo, int hi) {
    if (lo >= 0 && hi >= 0 && lo < hi) {
        int p = partition(A, lo, hi);
        quicksort(A, lo, p);
        quicksort(A, p + 1, hi);
    }
}

void store_dump_all(void) {
    objstore_hm_resize_barrier(&global_store);
    size_t count = nbhm_count(&global_store);

    kprintf("=== OBJECTS (%zu) ===\n", count);
    kprintf("%-14s %-16s %-16s  %-14s %-10s\n", "ID", "Type", "Rawptr", "Parent", "Status");

    // Publish all nodes
    size_t i = 0;
    KObject** objs = kheap_alloc(count * sizeof(KObject*));
    nbhm_for(it, &global_store) {
        objs[i++] = unstrip_ptr(it->val);
    }

    // Sort
    kassert(count == i, "bad");
    quicksort(objs, 0, count - 1);

    // Print
    FOR_N(i, 0, count) {
        print_obj(objs[i]->id, objs[i]);
    }

    kprintf("\n");
}
