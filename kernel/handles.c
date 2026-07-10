// The handle table is built out of a concurrent bitmap, we also wanna grab the lowest IDs first.
#include <kernel.h>
#include "threads.h"

bool handles_cmp(const void* a, const void* b) {
    return a == b;
}

uint32_t handles_hash(const void* k) {
    uint64_t h = (uint64_t) k;
    h ^= h >> 33ull;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33ull;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33ull;
    return h;
}

#define NBHM_FN(n) handles_ ## n
#include <nbhm.h>

const char* kobject_name(KObject* obj) {
    switch (obj->tag) {
        case KOBJECT_ENV:     return "ENV";
        case KOBJECT_THREAD:  return "THREAD";
        case KOBJECT_VMO:     return "VMO";
        case KOBJECT_MAILBOX: return "MAILBOX";
        case KOBJECT_EVENT:   return "EVENT";
        case KOBJECT_DEV_PCI: return "DEV_PCI";
        default: return "???";
    }
}

KObject_VMO* vmo_create_physical(uintptr_t addr, size_t size, VMem_Flags flags) {
    kassert((addr & (PAGE_SIZE-1)) == 0, "must be page-aligned (%d)", addr);

    KObject_VMO* obj = kheap_zalloc(sizeof(KObject_VMO));
    obj->super.tag = KOBJECT_VMO;
    obj->size = (size + PAGE_SIZE - 1) & -PAGE_SIZE;
    obj->paddr = addr;
    obj->flags = flags;
    if (addr == 0) {
        size_t init_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
        if (init_pages < 4)   { init_pages = 4;   }
        if (init_pages > 100) { init_pages = 100; }

        obj->pages = nbhm_alloc(init_pages);
    }
    STORE_PUT(obj);
    return obj;
}

KObject_Mailbox* mailbox_create(void) {
    KObject_Mailbox* obj = kheap_zalloc(sizeof(KObject_Mailbox));
    *obj = (KObject_Mailbox){
        .super = { .tag = KOBJECT_MAILBOX },
    };
    STORE_PUT(obj);
    return obj;
}

KObject_Event* event_create(void) {
    KObject_Event* obj = kheap_zalloc(sizeof(KObject_Event));
    *obj = (KObject_Event){
        .super = { .tag = KOBJECT_EVENT },
    };
    STORE_PUT(obj);
    return obj;
}

Thread* event_signal(KObject_Event* restrict event) {
    // need to be ordered like this
    u64 tail  = atomic_add_acq_rel(&event->tail, 1);
    Thread* t = atomic_ldacq(&event->waiting_thread);
    if (t != NULL && atomic_cas_acq_rel(&event->waiting_thread, &t, NULL)) {
        // wake up acknowledged however many signals came before it
        atomic_strel(&event->head, tail + 1);
        return t;
    }
    return NULL;
}

bool event_wait(KObject_Event* restrict event, Thread* thread) {
    u64 tail = atomic_ldacq(&event->tail);
    u64 head = atomic_ldacq(&event->head);
    // if tail isn't head, that means there's already a signal (or signals)
    if (head != tail && atomic_cas_acq_rel(&event->head, &head, tail)) {
        return false;
    }

    // we expect the scheduler locked here
    thread->client.is_blocked = true;
    thread->wait_obj = event;
    return atomic_cas_acq_rel(&event->waiting_thread, &(Thread*){ NULL }, thread);
}

void* env_get_handle(Env* env, KObjectID id, KAccessRights* out_rights) {
    KObject* obj = store_get(id);
    if (obj == NULL) {
        return NULL;
    }

    void* p = handles_get(&env->access_rights, (void*) id);
    KAccessRights rights = (((uint64_t) p) - 1) & KACCESS_MASK;
    if (p == NULL || (rights & KACCESS_READ) == 0) {
        return NULL;
    }

    if (out_rights) {
        *out_rights = rights;
    }
    return obj;
}

KObjectID env_grant_rights(Env* env, KAccessRights rights, KObject* obj) {
    rights |= KACCESS_READ;
    handles_put(&env->access_rights, (void*) obj->id, (void*) (uintptr_t) ((rights & KACCESS_MASK) + 1));
    return obj->id;
}

void env_ungrant_rights(Env* env, KObject* obj) {
    handles_remove(&env->access_rights, (void*) obj->id);
}

