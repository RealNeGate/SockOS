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
//   void* NBHM_FN(get)(NBHM* hm, void* key);
//   void* NBHM_FN(put)(NBHM* hm, void* key, void* val);
//   void* NBHM_FN(put_if_null)(NBHM* hm, void* key, void* val);
//   void NBHM_FN(resize_barrier)(NBHM* hm);
//
#ifndef NBHM_H
#define NBHM_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>

// personal debooging stuff
#define NBHM__DEBOOGING 1

#if NBHM__DEBOOGING
#define NBHM__BEGIN(name)      spall_begin_event(name, 0)
#define NBHM__END()            spall_end_event(0)
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

NBHM nbhm_alloc(size_t initial_cap);
void nbhm_free(NBHM* hs);

// for spooky stuff
static NBHM_Entry* nbhm_array(NBHM* hs) { return hs->latest->data; }
static size_t nbhm_count(NBHM* hs)      { return hs->latest->count; }
static size_t nbhm_capacity(NBHM* hs)   { return hs->latest->cap; }

#define nbhm_for(it, hs) for (NBHM_Entry *it = nbhm_array(hs), **_end_ = &it[nbhm_capacity(hs)]; it != _end_; it++) if (*it != NULL)
#endif // NBHM_H

#ifdef NBHM_IMPL
int NBHM_TOMBSTONE;
int NBHM_NO_MATCH_OLD;

atomic_flag nbhm_free_thread_init;

NBHM_FreeNode NBHM_DUMMY;

_Atomic(NBHM_FreeNode*) nbhm_free_head = &NBHM_DUMMY;
_Atomic(NBHM_FreeNode*) nbhm_free_tail = &NBHM_DUMMY;

_Atomic uint32_t nbhm_free_done;
_Atomic uint32_t nbhm_free_count;

NBHM nbhm_alloc(size_t initial_cap) {
    size_t cap = nbhm_compute_cap(initial_cap);
    NBHM_Table* table = kheap_zalloc(sizeof(NBHM_Table) + cap*sizeof(NBHM_Entry));
    nbhm_compute_size(table, cap);
    return (NBHM){ .latest = table };
}

void nbhm_free(NBHM* hs) {
    NBHM_Table* curr = hs->latest;
    while (curr) {
        NBHM_Table* next = curr->prev;
        kheap_free(curr);
        curr = next;
    }
}

void sleep(u64 timeout);

static uint64_t states[256];
int nbhm_thread_fn(void* arg) {
    for (;;) retry: {
        #if NBHM__DEBOOGING
        spall_begin_event("wait", 666);
        #endif

        // Use a futex to avoid polling too hard
        uint32_t old;
        while (old = nbhm_free_count, old == nbhm_free_done) {
            // No futex support, just sleep once we run out of tasks
            // ...
            sleep(10000);
        }

        #if NBHM__DEBOOGING
        spall_end_event(666);
        #endif

        NBHM_FreeNode* p = atomic_load_explicit(&nbhm_free_head, memory_order_relaxed);
        do {
            if (p->next == NULL) {
                // it's empty
                goto retry;
            }
        } while (!atomic_compare_exchange_strong(&nbhm_free_head, &p, p->next));
        NBHM_Table* table = p->next->table;

        // Handling deferred freeing without blocking up the normie threads
        #if NBHM__DEBOOGING
        spall_begin_event("scan", 666);
        #endif
        // "snapshot" the current statuses, once the other threads either advance or aren't in the
        // hashset functions we know we can free.
        for (int i = 0; i < boot_info->core_count; i++) {
            // mark sure no ptrs refer to prev
            states[i] = boot_info->cores[i].ebr_time;
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
        for (int i = 0; i < boot_info->core_count; i++) {
            uint64_t before_t = states[i];
            uint64_t now_t = boot_info->cores[i].ebr_time;
            if (before_t & NBHM_PINNED_BIT) {
                do {
                    now_t = boot_info->cores[i].ebr_time;
                } while (before_t == now_t);
            }
        }
        #if NBHM__DEBOOGING
        spall_end_event(666);
        #endif

        // no more refs, we can immediately free
        // NBHM_VIRTUAL_FREE(table, sizeof(NBHM_Table) + table->cap*sizeof(NBHM_Entry));
        ON_DEBUG(NBHM)(kprintf("[nbhm] free %p\n", table));

        nbhm_free_done++;
    }
}
#endif // NBHM_IMPL

// Templated implementation
#ifdef NBHM_FN
extern int NBHM_TOMBSTONE;
extern int NBHM_NO_MATCH_OLD;

extern atomic_flag nbhm_free_thread_init;

extern _Atomic(NBHM_FreeNode*) nbhm_free_head;
extern _Atomic(NBHM_FreeNode*) nbhm_free_tail;

extern _Atomic uint32_t nbhm_free_done;
extern _Atomic uint32_t nbhm_free_count;

extern int nbhm_thread_fn(void*);

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
    PerCPU* cpu = cpu_get();
    uint64_t t = atomic_load_explicit(&cpu->ebr_time, memory_order_relaxed);
    atomic_store_explicit(&cpu->ebr_time, t + NBHM_PINNED_BIT, memory_order_release);
}

// flips the top bit off AND increments time by one
static void NBHM_FN(exit_pinned)(void) {
    PerCPU* cpu = cpu_get();
    uint64_t t = atomic_load_explicit(&cpu->ebr_time, memory_order_relaxed);
    atomic_store_explicit(&cpu->ebr_time, t + NBHM_PINNED_BIT + 1, memory_order_release);
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

        if (!atomic_flag_test_and_set(&nbhm_free_thread_init)) {
            // spin up kernel thread which does freeing work
            thread_create(NULL, nbhm_thread_fn, 0, (uintptr_t) kpool_alloc_page(), 4096, false);
        }

        NBHM_FreeNode* new_node = kheap_alloc(sizeof(NBHM_FreeNode));
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

        // TODO(NeGate): signal futex
        // WakeByAddressSingle(&nbhm_free_count);

        prev = NULL;
        NBHM__END();
    }
    return prev;
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

            NBHM_Table* new_top = kheap_zalloc(sizeof(NBHM_Table) + new_cap*sizeof(NBHM_Entry));
            nbhm_compute_size(new_top, new_cap);

            // CAS latest -> new_table, if another thread wins the race we'll use its table
            new_top->prev = latest;
            if (!atomic_compare_exchange_strong(&hs->latest, &latest, new_top)) {
                kheap_free(new_top);
                prev = atomic_load(&latest->prev);
            } else {
                prev   = latest;
                latest = new_top;

                size_t s = sizeof(KernelFreeList) + sizeof(NBHM_Table) + new_cap*sizeof(NBHM_Entry);
                ON_DEBUG(NBHM)(kprintf("[nbhm] resize to %d bytes (cap=%d)\n", s, new_cap));
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
            if (prev != NULL) {
                v = NBHM_FN(raw_lookup)(hm, prev, h, key);
            }

            NBHM_FN(exit_pinned)();
            NBHM__END();
            return v;
        } else if (NBHM_FN(cmp)(k, key)) {
            NBHM_FN(exit_pinned)();
            NBHM__END();
            return v;
        }

        // inc & wrap around
        i = (i == cap-1) ? 0 : i + 1;
    } while (i != first);

    NBHM_ASSERT(0);
}

void* NBHM_FN(put_if_null)(NBHM* hm, void* key, void* val) {
    NBHM__BEGIN("put");

    NBHM_ASSERT(val);
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
