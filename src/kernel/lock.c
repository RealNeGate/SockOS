#include <kernel.h>

void spin_lock(Lock* lock) {
    // we shouldn't be spin locking...
    while (!atomic_compare_exchange_strong(lock, &(u32){ 0 }, 1)) {
        asm volatile ("pause");
    }
}

void spin_unlock(Lock* lock) {
    atomic_exchange(lock, 0);
}

bool rwlock_try_lock_shared(RWLock* lock) {
    u32 old = atomic_load_explicit(lock, memory_order_acquire);
    do {
        // it's being exclusively held rn
        if (old == 1) { return false; }
        // if there's no hold, let's take it (or if there's one we should tick up)
    } while (!atomic_compare_exchange_strong(lock, &old, old + 2));
    return true;
}

bool rwlock_is_exclusive(RWLock* lock) {
    return atomic_load_explicit(lock, memory_order_acquire) == 1;
}

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

