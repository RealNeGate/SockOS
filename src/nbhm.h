////////////////////////////////
// NBHM - Non-blocking hashmap
////////////////////////////////
// You wanna intern lots of things on lots of cores? this is for you. It's
// inspired by Cliff's non-blocking hashmap.
//
// To use it, you'll need to define NBHM_FN and then include the header:
//
//   #define NBHM_FN(n) XXX_hm_ ## n
//   #include <nbhm.h>
//
// This will compile implementations of the hashset using
//
//   bool NBHM_FN(cmp)(const void* a, const void* b);
//   uint32_t NBHM_FN(hash)(const void* a);
//
// The exported functions are:
//
//   void* NBHM_FN(get)(NBHM* hs, void* key);
//   void* NBHM_FN(put)(NBHM* hs, void* key, void* val);
//   void* NBHM_FN(put_if_null)(NBHM* hs, void* key, void* val);
//   void NBHM_FN(resize_barrier)(NBHM* hs);
//
#ifndef NBHM_H
#define NBHM_H

#if __STDC_HOSTED__
#include <threads.h>
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>

#ifndef NBHM_VIRTUAL_ALLOC
// Virtual memory allocation (since the tables are generally nicely page-size friendly)
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define NBHM_VIRTUAL_ALLOC(size)     VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)
#define NBHM_VIRTUAL_FREE(ptr, size) VirtualFree(ptr, size, MEM_RELEASE)
#else
#include <sys/mman.h>

#define NBHM_VIRTUAL_ALLOC(size)     mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)
#define NBHM_VIRTUAL_FREE(ptr, size) munmap(ptr, size)
#endif
#endif

// traditional heap ops
#ifndef NBHM_REALLOC
#define NBHM_REALLOC(ptr, size) realloc(ptr, size)
#endif // NBHM_REALLOC

#if __STDC_HOSTED__
#define NBHM_ASSERT(x) assert(x)
#elif !defined(NBHM_ASSERT)
#error "Missing NBHM_ASSERT(x), please define one"
#endif

// personal debooging stuff
#define NBHM__DEBOOGING 0

#if NBHM__DEBOOGING
#define NBHM__BEGIN(name)      spall_auto_buffer_begin(name, sizeof(name) - 1, NULL, 0)
#define NBHM__END()            spall_auto_buffer_end()
#else
#define NBHM__BEGIN(name)
#define NBHM__END()
#endif

// for the time in the ebr entry
#define NBHM_PINNED_BIT (1ull << 63ull)
#define NBHM_PRIME_BIT  (1ull << 63ull)

enum {
    NBHM_LOAD_FACTOR = 75,
    NBHM_MOVE_AMOUNT = 256,
};

typedef struct NBHM_EBREntry {
    _Atomic(struct NBHM_EBREntry*) next;
    _Atomic(uint64_t) time;

    // keep on a separate cacheline to avoid false sharing
    _Alignas(64) int id;
} NBHM_EBREntry;

typedef struct {
    _Atomic(void*) key;
    _Atomic(void*) val;
} NBHM_Entry;

typedef struct NBHM_Table NBHM_Table;
struct NBHM_Table {
    _Atomic(NBHM_Table*) prev;

    uint32_t cap;

    // reciprocals to compute modulo
    uint64_t a, sh;

    // tracks how many entries have
    // been moved once we're resizing
    _Atomic uint32_t moved;
    _Atomic uint32_t move_done;
    _Atomic uint32_t count;

    NBHM_Entry data[];
};

typedef struct {
    _Atomic(NBHM_Table*) latest;
} NBHM;

typedef struct NBHM_FreeNode NBHM_FreeNode;
struct NBHM_FreeNode {
    _Atomic(NBHM_FreeNode*) next;
    NBHM_Table* table;
};

static size_t nbhm_compute_cap(size_t y) {
    // minimum capacity
    if (y < 64) {
        y = 64;
    } else {
        y = ((y + 1) / 3) * 4;
    }

    size_t cap = 1ull << (64 - __builtin_clzll(y - 1));
    return cap - ((16 + sizeof(NBHM_Table)) / sizeof(NBHM_Entry));
}

static void nbhm_compute_size(NBHM_Table* table, size_t cap) {
    // reciprocals to compute modulo
    #if defined(__GNUC__) || defined(__clang__)
    table->sh = 64 - __builtin_clzll(cap);
    #else
    uint64_t sh = 0;
    while (cap > (1ull << sh)){ sh++; }
    table->sh = sh;
    #endif

    table->sh += 63 - 64;

    #if (defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__)
    uint64_t d,e;
    __asm__("div %[v]" : "=a"(d), "=d"(e) : [v] "r"(cap), "a"(cap - 1), "d"(1ull << table->sh));
    table->a = d;
    #elif defined(_MSC_VER)
    uint64_t rem;
    table->a = _udiv128(1ull << table->sh, cap - 1, cap, &rem);
    #else
    #error "Unsupported target"
    #endif

    table->cap = cap;
}

static NBHM nbhm_alloc(size_t initial_cap) {
    size_t cap = nbhm_compute_cap(initial_cap);
    NBHM_Table* table = NBHM_VIRTUAL_ALLOC(sizeof(NBHM_Table) + cap*sizeof(NBHM_Entry));
    nbhm_compute_size(table, cap);
    return (NBHM){ .latest = table };
}

static void nbhm_free(NBHM* hs) {
    NBHM_Table* curr = hs->latest;
    while (curr) {
        NBHM_Table* next = curr->prev;
        NBHM_VIRTUAL_FREE(curr, sizeof(NBHM_Table) + curr->cap*sizeof(NBHM_Entry));
        curr = next;
    }
}

// for spooky stuff
static NBHM_Entry* nbhm_array(NBHM* hs) { return hs->latest->data; }
static size_t nbhm_count(NBHM* hs)      { return hs->latest->count; }
static size_t nbhm_capacity(NBHM* hs)   { return hs->latest->cap; }

#define nbhm_for(it, hs) for (NBHM_Entry *it = nbhm_array(hs), **_end_ = &it[nbhm_capacity(hs)]; it != _end_; it++) if (*it != NULL)
#endif // NBHM_H

#ifdef NBHM_IMPL
#if defined(_WIN32)
#pragma comment(lib, "synchronization.lib")
#endif

int NBHM_TOMBSTONE;
int NBHM_NO_MATCH_OLD;

#if __STDC_HOSTED__
_Thread_local bool nbhm_ebr_init;
_Thread_local NBHM_EBREntry nbhm_ebr;
atomic_int nbhm_ebr_count;
_Atomic(NBHM_EBREntry*) nbhm_ebr_list;

atomic_flag nbhm_free_thread_init;

NBHM_FreeNode NBHM_DUMMY;

_Atomic(NBHM_FreeNode*) nbhm_free_head = &NBHM_DUMMY;
_Atomic(NBHM_FreeNode*) nbhm_free_tail = &NBHM_DUMMY;

_Atomic uint32_t nbhm_free_done;
_Atomic uint32_t nbhm_free_count;

int nbhm_thread_fn(void* arg) {
    #if NBHM__DEBOOGING
    spall_auto_thread_init(111111, SPALL_DEFAULT_BUFFER_SIZE);
    #endif

    for (;;) retry: {
        // Use a futex to avoid polling too hard
        uint32_t old;
        while (old = nbhm_free_count, old == nbhm_free_done) {
            #if defined(_WIN32)
            int WaitOnAddress(volatile void* addr, void* compare_addr, size_t addr_size, unsigned long millis);
            WaitOnAddress(&nbhm_free_count, &old, 4, -1);
            #elif defined(__linux__)
            int futex_rc = futex(SYS_futex, &nbhm_free_count, FUTEX_WAIT, old, NULL, NULL, 0);
            NBHM_ASSERT(futex_rc >= 0);
            #elif defined(__APPLE__)
            int __ulock_wait(uint32_t operation, void *addr, uint64_t value, uint32_t timeout);
            int res = __ulock_wait(0x01000001, &nbhm_free_count, old, 0);
            NBHM_ASSERT(res >= 0);
            #else
            // No futex support, just sleep once we run out of tasks
            thrd_sleep(&(struct timespec){.tv_nsec=100000000}, NULL);
            #endif
        }

        NBHM_FreeNode* p = atomic_load_explicit(&nbhm_free_head, memory_order_relaxed);
        do {
            if (p->next == NULL) {
                // it's empty
                goto retry;
            }
        } while (!atomic_compare_exchange_strong(&nbhm_free_head, &p, p->next));
        NBHM_Table* table = p->next->table;

        // Handling deferred freeing without blocking up the normie threads
        int state_count  = nbhm_ebr_count;
        uint64_t* states = NBHM_REALLOC(NULL, state_count * sizeof(uint64_t));

        NBHM__BEGIN("scan");
        NBHM_EBREntry* us = &nbhm_ebr;
        // "snapshot" the current statuses, once the other threads either advance or aren't in the
        // hashset functions we know we can free.
        for (NBHM_EBREntry* list = atomic_load(&nbhm_ebr_list); list; list = list->next) {
            // mark sure no ptrs refer to prev
            if (list != us && list->id < state_count) {
                states[list->id] = list->time;
            }
        }

        // important bit is that pointers can't be held across the critical sections, they'd need
        // to reload from `NBHM.latest`.
        //
        // Here's the states of our "epoch" critical section thingy:
        //
        // UNPINNED(id) -> PINNED(id) -> UNPINNED(id + 1) -> UNPINNED(id + 1) -> ...
        //
        // survey on if we can free the pointer if the status changed from X -> Y:
        //
        //   # YES: if we started unlocked then we weren't holding pointers in the first place.
        //   UNPINNED(A) -> PINNED(A)
        //   UNPINNED(A) -> UNPINNED(A)
        //   UNPINNED(A) -> UNPINNED(B)
        //
        //   # YES: if we're locked we need to wait until we've stopped holding pointers.
        //   PINNED(A)   -> PINNED(B)     we're a different call so we've let it go by now.
        //   PINNED(A)   -> UNPINNED(B)   we've stopped caring about the state of the pointer at this point.
        //
        //   # NO: we're still doing shit, wait a sec.
        //   PINNED(A)   -> PINNED(A)
        //
        // these aren't quite blocking the other threads, we're simply checking what their progress is concurrently.
        for (NBHM_EBREntry* list = atomic_load(&nbhm_ebr_list); list; list = list->next) {
            if (list != us && list->id < state_count && (states[list->id] & NBHM_PINNED_BIT)) {
                uint64_t before_t = states[list->id], now_t;
                do {
                    // idk, maybe this should be a better spinlock
                    now_t = atomic_load(&list->time);
                } while (before_t == now_t);
            }
        }
        NBHM__END();

        // no more refs, we can immediately free
        NBHM_VIRTUAL_FREE(table, sizeof(NBHM_Table) + table->cap*sizeof(NBHM_Entry));
        NBHM_REALLOC(states, 0);

        nbhm_free_done++;

        #if NBHM__DEBOOGING
        spall_auto_buffer_flush();
        #endif
    }
}
#endif

#endif // NBHM_IMPL

// Templated implementation
#ifdef NBHM_FN
extern int NBHM_TOMBSTONE;
extern int NBHM_NO_MATCH_OLD;

#if __STDC_HOSTED__
extern _Thread_local bool nbhm_ebr_init;
extern _Thread_local NBHM_EBREntry nbhm_ebr;
extern _Atomic(int) nbhm_ebr_count;
extern _Atomic(NBHM_EBREntry*) nbhm_ebr_list;

extern atomic_flag nbhm_free_thread_init;

extern _Atomic(NBHM_FreeNode*) nbhm_free_head;
extern _Atomic(NBHM_FreeNode*) nbhm_free_tail;

extern _Atomic uint32_t nbhm_free_done;
extern _Atomic uint32_t nbhm_free_count;

extern int nbhm_thread_fn(void*);
#endif

static void* NBHM_FN(put_if_match)(NBHM* hs, NBHM_Table* latest, NBHM_Table* prev, void* key, void* val, void* exp);

static size_t NBHM_FN(hash2index)(NBHM_Table* table, uint64_t h) {
    // MulHi(h, table->a)
    #if defined(__GNUC__) || defined(__clang__)
    uint64_t hi = (uint64_t) (((unsigned __int128)h * table->a) >> 64);
    #elif defined(_MSC_VER)
    uint64_t hi;
    _umul128(a, b, &hi);
    #else
    #error "Unsupported target"
    #endif

    uint64_t q  = hi >> table->sh;
    uint64_t q2 = h - (q * table->cap);

    NBHM_ASSERT(q2 == h % table->cap);
    return q2;
}

// flips the top bit on
static void NBHM_FN(enter_pinned)(void) {
    #if __STDC_HOSTED__
    uint64_t t = atomic_load_explicit(&nbhm_ebr.time, memory_order_relaxed);
    atomic_store_explicit(&nbhm_ebr.time, t + NBHM_PINNED_BIT, memory_order_release);
    #endif
}

// flips the top bit off AND increments time by one
static void NBHM_FN(exit_pinned)(void) {
    #if __STDC_HOSTED__
    uint64_t t = atomic_load_explicit(&nbhm_ebr.time, memory_order_relaxed);
    atomic_store_explicit(&nbhm_ebr.time, t + NBHM_PINNED_BIT + 1, memory_order_release);
    #endif
}

NBHM_Table* NBHM_FN(move_items)(NBHM* hm, NBHM_Table* latest, NBHM_Table* prev, int items_to_move) {
    NBHM_ASSERT(prev);
    size_t cap = prev->cap;

    // snatch up some number of items
    uint32_t old, new;
    do {
        old = atomic_load(&prev->moved);
        if (old == cap) { return prev; }
        // cap the number of items to copy... by the cap
        new = old + items_to_move;
        if (new > cap) { new = cap; }
    } while (!atomic_compare_exchange_strong(&prev->moved, &(uint32_t){ old }, new));

    if (old == new) {
        return prev;
    }

    NBHM__BEGIN("copying old");
    for (size_t i = old; i < new; i++) {
        void* old_v = atomic_load(&prev->data[i].val);
        void* k     = atomic_load(&prev->data[i].key);

        // freeze the values by adding a prime bit.
        while (((uintptr_t) old_v & NBHM_PRIME_BIT) == 0) {
            uintptr_t primed_v = (old_v == &NBHM_TOMBSTONE ? 0 : (uintptr_t) old_v) | NBHM_PRIME_BIT;
            if (atomic_compare_exchange_strong(&prev->data[i].val, &old_v, (void*) primed_v)) {
                if (old_v != NULL && old_v != &NBHM_TOMBSTONE) {
                    // once we've frozen, we can move it to the new table.
                    // we pass NULL for prev since we already know the entries exist in prev.
                    NBHM_FN(put_if_match)(hm, latest, NULL, k, old_v, &NBHM_NO_MATCH_OLD);
                }
                break;
            }
            // btw, CAS updated old_v
        }
    }
    NBHM__END();

    uint32_t done = atomic_fetch_add(&prev->move_done, new - old);
    done += new - old;

    NBHM_ASSERT(done <= cap);
    if (done == cap) {
        // dettach now
        NBHM__BEGIN("detach");
        latest->prev = NULL;

        #if __STDC_HOSTED__
        if (!atomic_flag_test_and_set(&nbhm_free_thread_init)) {
            thrd_t freeing_thread; // don't care to track it
            thrd_create(&freeing_thread, nbhm_thread_fn, NULL);
        }

        NBHM_FreeNode* new_node = NBHM_REALLOC(NULL, sizeof(NBHM_FreeNode));
        new_node->table = prev;
        new_node->next  = NULL;

        // enqueue, it's a low-size low-contention list i just don't wanna block my lovely normie threads
        NBHM_FreeNode* p = atomic_load_explicit(&nbhm_free_tail, memory_order_relaxed);
        NBHM_FreeNode* old_p = p;
        do {
            while (p->next != NULL) {
                p = p->next;
            }
        } while (!atomic_compare_exchange_strong(&p->next, &(NBHM_FreeNode*){ NULL }, new_node));
        atomic_compare_exchange_strong(&nbhm_free_tail, &old_p, new_node);

        nbhm_free_count++;

        // signal futex
        #if defined(_WIN32)
        extern void WakeByAddressSingle(void* addr);
        WakeByAddressSingle(&nbhm_free_count);
        #elif defined(__linux__)
        int futex_rc = futex(SYS_futex, &nbhm_free_count, FUTEX_WAKE, 1, NULL, NULL, 0);
        NBHM_ASSERT(futex_rc >= 0);
        #elif defined(__APPLE__)
        extern int __ulock_wake(uint32_t operation, void* addr, uint64_t wake_value);
        int res = __ulock_wake(0x01000001, &nbhm_free_count);
        NBHM_ASSERT(res >= 0);
        #endif
        #endif // __STDC_HOSTED__

        prev = NULL;
        NBHM__END();
    }
    return prev;
}

static void NBHM_FN(ebr_try_init)(void) {
    #if __STDC_HOSTED__
    if (!nbhm_ebr_init) {
        NBHM__BEGIN("init");
        nbhm_ebr_init = true;
        nbhm_ebr.id = nbhm_ebr_count++;

        // add to ebr list, we never free this because i don't care
        // TODO(NeGate): i do care, this is a nightmare when threads die figure it out
        NBHM_EBREntry* old;
        do {
            old = atomic_load_explicit(&nbhm_ebr_list, memory_order_relaxed);
            nbhm_ebr.next = old;
        } while (!atomic_compare_exchange_strong(&nbhm_ebr_list, &old, &nbhm_ebr));
        NBHM__END();
    }
    #endif
}

static void* NBHM_FN(raw_lookup)(NBHM* hs, NBHM_Table* table, uint32_t h, void* key) {
    size_t cap = table->cap;
    size_t first = NBHM_FN(hash2index)(table, h), i = first;
    do {
        void* v = atomic_load(&table->data[i].val);
        void* k = atomic_load(&table->data[i].key);

        if (k == NULL) {
            return NULL;
        } else if (NBHM_FN(cmp)(k, key)) {
            return v != &NBHM_TOMBSTONE ? v : NULL;
        }

        // inc & wrap around
        i = (i == cap-1) ? 0 : i + 1;
    } while (i != first);

    return NULL;
}

static void* NBHM_FN(put_if_match)(NBHM* hs, NBHM_Table* latest, NBHM_Table* prev, void* key, void* val, void* exp) {
    NBHM_ASSERT(key);

    void *k, *v;
    for (;;) {
        uint32_t cap = latest->cap;
        size_t limit = (cap * NBHM_LOAD_FACTOR) / 100;
        if (prev == NULL && latest->count >= limit) {
            // make resized table, we'll amortize the moves upward
            size_t new_cap = nbhm_compute_cap(limit*2);

            NBHM_Table* new_top = NBHM_VIRTUAL_ALLOC(sizeof(NBHM_Table) + new_cap*sizeof(NBHM_Entry));
            nbhm_compute_size(new_top, new_cap);

            // CAS latest -> new_table, if another thread wins the race we'll use its table
            new_top->prev = latest;
            if (!atomic_compare_exchange_strong(&hs->latest, &latest, new_top)) {
                NBHM_VIRTUAL_FREE(new_top, sizeof(NBHM_Table) + new_cap*sizeof(NBHM_Entry));
                prev = atomic_load(&latest->prev);
            } else {
                prev   = latest;
                latest = new_top;

                // float s = sizeof(NBHM_Table) + new_cap*sizeof(NBHM_Entry);
                // printf("Resize: %.2f KiB (cap=%zu)\n", s / 1024.0f, new_cap);
            }
            continue;
        }

        // key claiming phase:
        //   once completed we'll have a key inserted into the latest
        //   table (the value might be NULL which means that the entry
        //   is still empty but we've at least agreed where the value
        //   goes).
        bool found = false;
        uint32_t h = NBHM_FN(hash)(key);
        size_t first = NBHM_FN(hash2index)(latest, h), i = first;
        do {
            v = atomic_load_explicit(&latest->data[i].val, memory_order_acquire);
            k = atomic_load_explicit(&latest->data[i].key, memory_order_acquire);

            if (k == NULL) {
                // key was never in the table
                if (val == &NBHM_TOMBSTONE) { return NULL; }

                // fight for empty slot
                if (atomic_compare_exchange_strong(&latest->data[i].key, &k, key)) {
                    atomic_fetch_add_explicit(&latest->count, 1, memory_order_relaxed);
                    found = true;
                    break;
                }
            }

            if (NBHM_FN(cmp)(k, key)) {
                found = true;
                break;
            }

            // inc & wrap around
            i = (i == cap-1) ? 0 : i + 1;
        } while (i != first);

        // we didn't claim a key, that means the table is entirely full, retry
        // to use or make a bigger table.
        if (!found) {
            latest = atomic_load(&hs->latest);
            prev   = atomic_load(&latest->prev);
            continue;
        }

        // migration barrier, we only insert our item once we've
        // "logically" moved it
        if (v == NULL && prev != NULL) {
            NBHM_ASSERT(prev->prev == NULL);
            void* old = NBHM_FN(raw_lookup)(hs, prev, h, val);
            if (old != NULL) {
                // the old value might've been primed, we don't want to propagate the prime bit tho
                old = (void*) (((uintptr_t) old) & ~NBHM_PRIME_BIT);

                // if we lost, then we just get replaced by a separate fella (which is fine ig)
                if (atomic_compare_exchange_strong(&latest->data[i].val, &v, old)) {
                    v = old;
                }
            }
        }

        // if the old value is a prime, we've been had (we're resizing)
        if (((uintptr_t) v) & NBHM_PRIME_BIT) {
            continue;
        }

        // if the existing value is:
        // * incompatible with the expected value, we don't write.
        // * equal, we don't write (speed opt, one CAS is slower than no CAS).
        if (v != val &&
            // exp is tombstone, we'll only insert if it's empty (and not a migration)
            (exp != &NBHM_TOMBSTONE || (v == NULL || v == &NBHM_TOMBSTONE))
        ) {
            // value writing attempt, if we lose the CAS it means someone could've written a
            // prime (thus the entry was migrated to a later table). It could also mean we lost
            // the insertion fight to another writer and in that case we'll take their value.
            if (atomic_compare_exchange_strong(&latest->data[i].val, &v, val)) {
                v = val;
            } else {
                // if we see a prime, the entry has been migrated
                // and we should write to that later table. if not,
                // we simply lost the race to update the value.
                uintptr_t v_raw = (uintptr_t) v;
                if (v_raw & NBHM_PRIME_BIT) {
                    continue;
                }
            }
        }

        return v;
    }
}

void* NBHM_FN(put)(NBHM* hm, void* key, void* val) {
    NBHM__BEGIN("put");

    NBHM_ASSERT(val);
    NBHM_FN(ebr_try_init)();

    NBHM_FN(enter_pinned)();
    NBHM_Table* latest = atomic_load(&hm->latest);

    // if there's earlier versions of the table we can move up entries as we go along.
    NBHM_Table* prev = atomic_load(&latest->prev);
    if (prev != NULL) {
        prev = NBHM_FN(move_items)(hm, latest, prev, NBHM_MOVE_AMOUNT);
        if (prev == NULL) {
            latest = atomic_load(&hm->latest);
        }
    }

    void* v = NBHM_FN(put_if_match)(hm, latest, prev, key, val, &NBHM_NO_MATCH_OLD);

    NBHM_FN(exit_pinned)();
    NBHM__END();
    return v;
}

void* NBHM_FN(remove)(NBHM* hm, void* key) {
    NBHM__BEGIN("remove");

    NBHM_ASSERT(key);
    NBHM_FN(ebr_try_init)();

    NBHM_FN(enter_pinned)();
    NBHM_Table* latest = atomic_load(&hm->latest);

    // if there's earlier versions of the table we can move up entries as we go along.
    NBHM_Table* prev = atomic_load(&latest->prev);
    if (prev != NULL) {
        prev = NBHM_FN(move_items)(hm, latest, prev, NBHM_MOVE_AMOUNT);
        if (prev == NULL) {
            latest = atomic_load(&hm->latest);
        }
    }

    void* v = NBHM_FN(put_if_match)(hm, latest, prev, key, &NBHM_TOMBSTONE, &NBHM_NO_MATCH_OLD);

    NBHM_FN(exit_pinned)();
    NBHM__END();
    return v;
}

void* NBHM_FN(get)(NBHM* hm, void* key) {
    NBHM__BEGIN("get");

    NBHM_ASSERT(key);
    NBHM_FN(ebr_try_init)();

    NBHM_FN(enter_pinned)();
    NBHM_Table* latest = atomic_load(&hm->latest);

    // if there's earlier versions of the table we can move up entries as we go along.
    NBHM_Table* prev = atomic_load(&latest->prev);
    if (prev != NULL) {
        prev = NBHM_FN(move_items)(hm, latest, prev, NBHM_MOVE_AMOUNT);
        if (prev == NULL) {
            latest = atomic_load(&hm->latest);
        }
    }

    uint32_t cap = latest->cap;
    uint32_t h = NBHM_FN(hash)(key);
    size_t first = NBHM_FN(hash2index)(latest, h), i = first;

    void *k, *v;
    do {
        v = atomic_load(&latest->data[i].val);
        k = atomic_load(&latest->data[i].key);

        if (k == NULL) {
            // first time seen, maybe the entry hasn't been moved yet
            if (prev == NULL) {
                v = NBHM_FN(raw_lookup)(hm, prev, h, key);
            }
            break;
        } else if (NBHM_FN(cmp)(k, key)) {
            return v;
        }

        // inc & wrap around
        i = (i == cap-1) ? 0 : i + 1;
    } while (i != first);

    NBHM_FN(exit_pinned)();
    NBHM__END();
    return v;
}

void* NBHM_FN(put_if_null)(NBHM* hm, void* key, void* val) {
    NBHM__BEGIN("put");

    NBHM_ASSERT(val);
    NBHM_FN(ebr_try_init)();

    NBHM_FN(enter_pinned)();
    NBHM_Table* latest = atomic_load(&hm->latest);

    // if there's earlier versions of the table we can move up entries as we go along.
    NBHM_Table* prev = atomic_load(&latest->prev);
    if (prev != NULL) {
        prev = NBHM_FN(move_items)(hm, latest, prev, NBHM_MOVE_AMOUNT);
        if (prev == NULL) {
            latest = atomic_load(&hm->latest);
        }
    }

    void* v = NBHM_FN(put_if_match)(hm, latest, prev, key, val, &NBHM_TOMBSTONE);

    NBHM_FN(exit_pinned)();
    NBHM__END();
    return v;
}

// waits for all items to be moved up before continuing
void NBHM_FN(resize_barrier)(NBHM* hm) {
    NBHM__BEGIN("resize_barrier");
    NBHM_FN(ebr_try_init)();

    NBHM_FN(enter_pinned)();
    NBHM_Table *prev, *latest = atomic_load(&hm->latest);
    while (prev = atomic_load(&latest->prev), prev != NULL) {
        NBHM_FN(move_items)(hm, latest, prev, prev->cap);
    }

    NBHM_FN(exit_pinned)();
    NBHM__END();
}

#undef NBHM_FN
#endif // NBHM_FN
