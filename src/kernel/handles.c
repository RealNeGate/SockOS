// The handle table is built out of a concurrent bitmap, we also wanna grab the lowest IDs first.
#include <kernel.h>

KObject_VMO* vmo_create_physical(uintptr_t addr, size_t size) {
    kassert((addr & (PAGE_SIZE-1)) == 0, "must be page-aligned (%d)", addr);

    KObject_VMO* obj = kheap_zalloc(sizeof(KObject_VMO));
    obj->super.tag = KOBJECT_VMO;
    obj->super.ref_count = 1;
    obj->size = size;
    obj->paddr = addr;
    return obj;
}

void* env_get_handle(Env* env, KHandle handle, KAccessRights* rights) {
    if (handle == 0) {
        return NULL;
    }

    size_t i = handle / 63ull;
    size_t j = handle % 63ull;

    for (;;) {
        KHandleTable* latest = env->handles;
        uintptr_t packed = atomic_load_explicit(&latest->entries[i].objects[j], memory_order_acquire);
        u64 bits = latest->entries[i].open;

        // if the MSB is set, we're in an out-dated table and cannot write to it
        if (bits >> 63ull) { continue; }
        // if handle doesn't exist
        if ((bits & (1ull << j)) == 0) { return NULL; }
        if (rights) { *rights = packed >> 48ull; }
        return (void*) arch_canonical_addr(packed & 0xFFFFFFFFFFFFull);
    }
}

KHandle env_open_handle(Env* env, KAccessRights rights, KObject* obj) {
    u64 packed = ((u64) obj & 0xFFFFFFFFFFFFull) | ((u64)rights << 48ull);

    for (;;) retry: {
        KHandleTable* latest = env->handles;

        // if resize in progress, help out
        // ...

        // find free bit and steal it
        FOR_N(i, 0, latest->capacity) {
            u64 bits = latest->entries[i].open;
            int free_bit = 0;
            do {
                // if the MSB is set, we're in an out-dated table and cannot write to it
                if (bits >> 63ull) { goto retry; }
                // all IDs taken, skip over
                if (bits == (UINT64_MAX>>1)) { goto skip; }
                // attempt to claim ID, if we didn't it just means another thread did
                // which is good for them (we don't get jealous, we're a girl's girl).
                free_bit = __builtin_ffsll(~bits) - 1;
            } while (!atomic_compare_exchange_strong(&latest->entries[i].open, &bits, bits | (1ull << free_bit)));

            // the ID is claimed but still mapped to nothing, once we perform this write all threads will
            // consider the handle as visible.
            ON_DEBUG(ENV)(kprintf("[env] opened Handle-%d\n", i*63 + free_bit));
            atomic_store_explicit(&latest->entries[i].objects[free_bit], packed, memory_order_release);
            return i*63 + free_bit;

            skip:;
        }
    }

    // no bit was free, concurrent resize
    kassert(0, "todo: resize handle table");
}

// we don't have to mark the objects entry as NULL because if another thread opened the same ID, it
// would write the objects entry before making it's ID visible to the rest of the process.
bool env_close_handle(Env* env, KHandle handle) {
    if (handle == 0) {  // cannot close the null handle
        return false;
    }

    size_t i = handle / 63ull;
    uint64_t mask = 1ull << (handle % 63ull);

    for (;;) retry: {
        KHandleTable* latest = env->handles;
        if (i >= latest->capacity) {
            return false;
        }

        u64 bits = latest->entries[i].open;
        do {
            // if the MSB is set, we're in an out-dated table and cannot write to it
            if (bits >> 63ull) { goto retry; }
            // if the bit isn't closed we should throw an error
            if ((bits & mask) == 0) {
                return false;
            }
            // attempt to remove ID, since it's possible to lose the CAS due to
            // a separate ID being closed we retry on failure.
        } while (!atomic_compare_exchange_strong(&latest->entries[i].open, &bits, bits & ~mask));

        ON_DEBUG(ENV)(kprintf("[env] closed Handle-%d\n", handle));
        return true;
    }
}

