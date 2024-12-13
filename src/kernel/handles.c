// The handle table is built out of a concurrent bitmap, we also wanna grab the lowest IDs first.
#include <kernel.h>

KObject_VMO* vmo_create_physical(uintptr_t addr, size_t size) {
    kassert((addr & (PAGE_SIZE-1)) == 0, "must be page-aligned (%d)", addr);

    KObject_VMO* obj = kheap_zalloc(sizeof(KObject_VMO));
    obj->super.tag = KOBJECT_VMO;
    obj->super.ref_count = 1;
    obj->size = (size + PAGE_SIZE - 1) & -PAGE_SIZE;
    obj->paddr = addr;
    return obj;
}

KObject_Mailbox* vmo_mailbox_create(void) {
    KObject_Mailbox* obj = kheap_alloc(sizeof(KObject_Mailbox) + KOBJECT_MAILBOX_SIZE);
    *obj = (KObject_Mailbox){
        .super= {
            .tag = KOBJECT_VMO, .ref_count = 1,
        },
        .mask = KOBJECT_MAILBOX_SIZE - 1,
    };
    return obj;
}

void vmo_mailbox_send(KObject_Mailbox* mailbox, size_t handle_count, KHandle* handles, size_t data_size, uint8_t* data) {
}

void* env_get_handle(Env* env, KHandle handle, KAccessRights* rights) {
    if (handle == 0) {
        return NULL;
    }

    size_t i = handle / 63ull;
    size_t j = handle % 63ull;

    for (;;) {
        KHandleTable* latest = env->handles;
        KHandleTable* prev = atomic_load_explicit(&latest->prev, memory_order_relaxed);

        if (i >= latest->capacity) {
            return NULL;
        }

        uintptr_t packed = atomic_load_explicit(&latest->entries[i].objects[j], memory_order_acquire);
        u64 bits = latest->entries[i].open;

        // if the MSB is set, we're in an out-dated table and cannot write to it
        if (bits >> 63ull) { continue; }
        // check old table
        if (bits == 0) {
            while (prev != NULL) {
                // we don't care if it's been moved or not, we don't wanna steal the
                // cacheline from our potential fellow writers.
                packed = atomic_load_explicit(&prev->entries[i].objects[j], memory_order_acquire);
                bits = prev->entries[i].open;
                if (bits != 0) {
                    break;
                }
                prev = prev->prev;
            }
        }
        // if handle doesn't exist
        if ((bits & (1ull << j)) == 0) { return NULL; }
        if (rights) { *rights = packed >> 48ull; }
        return (void*) arch_canonical_addr(packed & 0xFFFFFFFFFFFFull);
    }
}

static u64 migrate_handles(KHandleTable* latest, KHandleTable* prev, size_t i) {
    if (i >= prev->capacity) {
        return 0;
    }

    // froze the earliest table first
    KHandleTable* prev_prev = prev->prev;
    if (prev_prev != NULL) {
        migrate_handles(latest, prev_prev, i);
    }

    u64 prev_bits = atomic_load_explicit(&prev->entries[i].open, memory_order_acquire);

    // freeze bits before migrating up
    while (!atomic_compare_exchange_strong(&prev->entries[i].open, &prev_bits, prev_bits | (1ull << 63ull))) {
    }

    // once the bits are frozen, the handle table cannot be modified since it would've needed to
    // CAS the bits in an unfrozen state to do so.
    FOR_N(j, 0, 63) if (prev_bits & (1ull << j)) {
        u64 old = atomic_load_explicit(&prev->entries[i].objects[j], memory_order_acquire);
        // CAS old values, if we lost then someone beat us to it
        atomic_compare_exchange_strong(&prev->entries[i].open, &(u64){ 0 }, old);
    }

    u64 bits = prev_bits & (UINT64_MAX>>1);

    // after this CAS, either (or someone else) have migrated the word. we can retry
    // the insertion now.
    atomic_compare_exchange_strong(&latest->entries[i].open, &(u64){ 0 }, bits);
    return bits;
}

KHandle env_open_handle(Env* env, KAccessRights rights, KObject* obj) {
    u64 packed = ((u64) obj & 0xFFFFFFFFFFFFull) | ((u64)rights << 48ull);

    KHandleTable* latest = env->handles;
    KHandleTable* prev = atomic_load_explicit(&latest->prev, memory_order_relaxed);

    for (;;) retry: {
        // if resize in progress, help out
        if (prev != NULL) {
            // migrate up some entries
            size_t old, new;
            do {
                old = atomic_load_explicit(&prev->claimed, memory_order_acquire);
                new = old + 4;
                if (new > prev->capacity) { new = prev->capacity; }
            } while (!atomic_compare_exchange_strong(&prev->claimed, &old, new));

            ON_DEBUG(ENV)(kprintf("[env] migrating [Handle-%d..Handle-%d]\n", old*63, new*63 - 1));
            // ON_DEBUG(ENV)(kprintf("[env] migrating [%d, %d)\n", old, new));

            FOR_N(i, old, new) {
                migrate_handles(latest, prev, i);
            }

            u64 done = atomic_fetch_add(&prev->done, new - old) + (new - old);
            if (done == prev->capacity) {
                ON_DEBUG(ENV)(kprintf("[env] finished migrating all entries\n"));
                prev = prev->prev;
                latest->prev = prev;
            }

        }

        // find free bit and steal it
        FOR_N(i, 0, latest->capacity) {
            u64 bits = atomic_load_explicit(&latest->entries[i].open, memory_order_acquire);
            int free_bit = 0;
            for (;;) {
                // if the MSB is set, we're in an out-dated table and cannot write to it
                if (bits >> 63ull) {
                    latest = env->handles;
                    prev = atomic_load_explicit(&latest->prev, memory_order_relaxed);
                    goto retry;
                }
                // if IDs are 0, try to migrate up
                if (bits == 0 && prev != NULL) {
                    bits = migrate_handles(latest, prev, i);
                    // if it's still 0, we're like, actually zero this time
                    if (bits != 0) { continue; }
                }
                // all IDs taken, skip over
                if (bits == (UINT64_MAX>>1)) { break; }
                // attempt to claim ID, if we didn't it just means another thread did
                // which is good for them (we don't get jealous, we're a girl's girl).
                free_bit = __builtin_ffsll(~bits) - 1;
                // we first need to steal the empty handles slot, then CAS the bits.
                // This way, after freezing the open bits during migration, we'll see
                // the final write to the handles published by that point.
                u64 null = 0;
                if (!atomic_compare_exchange_strong(&latest->entries[i].objects[free_bit], &null, packed)) {
                    // we lost, this means someone claimed the slot faster and thus
                    // the free_bit is no longer free, there might be more bits set
                    // in the same word so we can retry for there.
                    continue;
                }
                // we successfully write the bit (this write doesn't have much true sharing due to the previous CAS)
                if (atomic_compare_exchange_strong(&latest->entries[i].open, &bits, bits | (1ull << free_bit))) {
                    ON_DEBUG(ENV)(kprintf("[env] opened Handle-%d\n", i*63 + free_bit));
                    return i*63 + free_bit;
                }
            }
            skip:;
        }

        // no bit was free, concurrent resize
        size_t new_cap = latest->capacity * 2;
        KHandleTable* new_table = kheap_zalloc(sizeof(KHandleTable) + new_cap*sizeof(KHandleEntry));

        new_table->prev = latest;
        new_table->capacity = new_cap;

        // CAS latest -> new_table, if another thread wins the race we'll use its table
        if (!atomic_compare_exchange_strong(&env->handles, &latest, new_table)) {
            kheap_free(new_table);
            prev = atomic_load(&latest->prev);
        } else {
            prev   = latest;
            latest = new_table;

            size_t s = sizeof(KernelFreeList) + sizeof(KHandleTable) + new_cap*sizeof(KHandleEntry);
            ON_DEBUG(ENV)(kprintf("[env] resize handle table to %d bytes (cap=%d)\n", s, new_cap));
        }
    }
}

bool env_close_handle(Env* env, KHandle handle) {
    if (handle == 0) {  // cannot close the null handle
        return false;
    }

    size_t i = handle / 63ull;
    size_t j = handle % 63ull;
    uint64_t mask = 1ull << j;

    for (;;) retry: {
        KHandleTable* latest = env->handles;
        if (i >= latest->capacity) {
            return false;
        }

        KHandleTable* prev = atomic_load_explicit(&latest->prev, memory_order_relaxed);
        u64 bits = latest->entries[i].open;
        do {
            // if the MSB is set, we're in an out-dated table and cannot write to it
            if (bits >> 63ull) { goto retry; }
            // if IDs are 0, try to migrate up
            if (bits == 0 && prev != NULL) {
                bits = migrate_handles(latest, prev, i);
                // if it's still 0, we're like, actually zero this time
                if (bits != 0) { continue; }
            }
            // if the bit isn't closed we should throw an error
            if ((bits & mask) == 0) {
                return false;
            }
            // attempt to remove ID, since it's possible to lose the CAS due to
            // a separate ID being closed we retry on failure.
        } while (!atomic_compare_exchange_strong(&latest->entries[i].open, &bits, bits & ~mask));

        // remove the object from the table, if it was NULL it means someone's already
        // freed it. we removed the objects[j] entry *AFTER* the bit unset so if migration
        // chooses to freeze these values, we aren't caught mutating objects.
        if (atomic_exchange(&latest->entries[i].objects[j], 0) == 0) {
            return false;
        }

        ON_DEBUG(ENV)(kprintf("[env] closed Handle-%d\n", handle));
        return true;
    }
}

