// Multi-head logs, we call that layer C0. When C0 is full, it'll flush
// out to C1 which is a list of logs that are uncompacted. Any two logs
// can be merged since we have a strong monotonic guarantee from the IDs.
#include <kernel.h>
#include "threads.h"

typedef struct {
    _Atomic(KObjectID) k;
    _Atomic(KObject*)  v;
} StoreEntry;

struct StoreLog {
    // C1 uses this to represent newly chained logs
    _Atomic(StoreLog*) next;

    // when compacting this log with the next, this is our
    // destination log.
    _Atomic(StoreLog*) compact;

    // picks the index we write to, this can be bigger than the
    // cap if many users fail to acknowledge the overflow
    atomic_u32 reserve;

    // counts how many entries actually completed their writes
    atomic_u32 commit;

    _Alignas(64) uint32_t cap;

    // series of sorted events, because we're getting IDs from
    // a monotonic source, we never really have to consider
    // putting entries anywhere but the top.
    StoreEntry entries[];
};

struct ObjectStore {
    // compacted logs
    // _Atomic(StoreLog*) c2;

    // uncompacted logs
    _Atomic(StoreLog*) c1;

    // PerCPU logs
    StoreLog* c0[MAX_CORES];
};

static atomic_u64 OBJ_ID_CNT = 0;
static ObjectStore global_store;

static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

static KObjectID get_log_entry(StoreLog* log, uint32_t i, KObject** out_v) {
    KObject*  v = atomic_ldacq(&log->entries[i].v);
    KObjectID k = atomic_ldacq(&log->entries[i].k);

    *out_v = v;
    return k;
}

#if 0
// Concurrent Cooperative Compaction (CCC!!!)
void store_coop(ObjectStore* store) {
    StoreLog* a = atomic_ldacq(&store->c1);
    StoreLog* b = atomic_ldacq(&a->next);

    uint32_t a_cnt = min_u32(a->reserve, a->cap);
    uint32_t b_cnt = min_u32(b->reserve, b->cap);
    uint32_t cap = a_cnt + b_cnt;

    // Since this is gonna be placed into C2, it's immutable and thus doesn't need
    // an alloc
    StoreLog* dst = kheap_alloc(sizeof(StoreLog) + cap*sizeof(StoreEntry));

    uint32_t min = min_u32(a_cnt, b_cnt);
    uint32_t ai = 0, bi = 0, cnt = 0;
    while (cnt < min) {
        KObject *av, *bv;
        KObjectID ak = get_log_entry(a, ai, &av);
        KObjectID bk = get_log_entry(b, bi, &bv);
        if (ak != UINT64_MAX && bk != UINT64_MAX) {
            if (ak < bk) {
                atomic_store_explicit(&dst->entries[cnt].k, ak, memory_order_relaxed);
                atomic_store_explicit(&dst->entries[cnt].v, av, memory_order_relaxed);
                ai += 1;
            } else {
                atomic_store_explicit(&dst->entries[cnt].k, bk, memory_order_relaxed);
                atomic_store_explicit(&dst->entries[cnt].v, bv, memory_order_relaxed);
                bi += 1;
            }
            cnt += 1;
        }
    }

    while (ai < a_cnt) {
        KObject* av;
        KObjectID ak = get_log_entry(a, ai, &av);

        atomic_strlx(&dst->entries[cnt].k, ak);
        atomic_strlx(&dst->entries[cnt].v, av);
        cnt += 1, ai += 1;
    }

    while (bi < b_cnt) {
        KObject* bv;
        KObjectID bk = get_log_entry(b, bi, &bv);

        atomic_strlx(&dst->entries[cnt].k, bk);
        atomic_strlx(&dst->entries[cnt].v, bv);
        cnt += 1, bi += 1;
    }
    dst->reserve = dst->commit = cnt;
    atomic_thread_fence(memory_order_release);

    // Append to C2 (Single-threaded action)
    dst->next = store->c2;
    store->c2 = dst;

    // Advance C1 to skip A & B
    StoreLog* c = atomic_ldacq(&b->next);
    if (atomic_cas_acq_rel(&store->c1, &a, c)) {

    }
}
#endif

enum {
    STORE_INIT_CAP = 1024
};

void store_alloc(void) {
    FOR_N(i, 0, boot_info->core_count) {
        StoreLog* new_log = kheap_zalloc(sizeof(StoreLog) + STORE_INIT_CAP*sizeof(StoreEntry));
        atomic_strel(&global_store->c0[i], new_log);
    }
}

KObjectID store_put(KObject* obj) {
    KObjectID id = obj->id = ++OBJ_ID_CNT;

    int core_id = cpu_get_index();
    StoreLog* log = atomic_ldacq(&store->c0[core_id]);
    kassert(log, "You forgot to allocate the log");

    // Insertion
    for (;;) {
        // C0 is all core-local writes but it can be snooped by other cores.
        int idx = atomic_add_acq_rel(&log->reserve, 1);
        if (atomic_ldacq(&log->next) == NULL && idx >= log->cap) {
            StoreLog* fresh = kheap_zalloc(sizeof(StoreLog) + log->cap*sizeof(StoreEntry));

            // move our stale log from C0 -> C1
            StoreLog* c1;
            do {
                c1 = atomic_ldacq(&store->c1);
            } while (!atomic_cas_acq_rel(&c1->next, &(KObject*){ NULL }, log));

            atomic_strel(&store->c0[core_id], fresh);
            log = fresh;
            continue;
        }
        // If another core sees K but V is NULL, then the object will be treated
        // as if it doesn't exist because it means we haven't completed the
        // store_put on the function where it has been exposed to the outside
        // world.
        atomic_strel(&log->entries[id].k, id);
        atomic_strel(&log->entries[id].v, obj);
        atomic_add_acq_rel(&log->commit, 1);
        break;
    }

    return id;
}

static KObject* snoop_log(StoreLog* log, KObjectID key) {
    size_t left = 0, right = atomic_ldacq(&log->reserve, 1);
    while (left != right) {
        size_t i = (left + right) / 2;
        KObjectID key_at_idx = atomic_ldacq(&log->entries[i].k);
        if (key_at_idx == key) {
            return atomic_ldacq(&log->entries[i].v);
        } else if (key_at_idx > key) {
            right = i;
        } else {
            left = i + 1;
        }
    }
    return NULL;
}

KObject* store_get(KObjectID id) {
    int core_id = cpu_get_index();

    // Snoop C0, starting with own core
    StoreLog* log = atomic_ldacq(&store->c0[core_id]);
    KObject* obj = snoop_log(log, id);
    if (obj != NULL) { return obj; }

    // Snoop C0 on other cores, we should prioritize some ordering
    // based on NUMA... maybe.
    FOR_N(i, 0, boot_info->core_count) if (i != core_id) {
        log = atomic_ldacq(&store->c0[i]);
        obj = snoop_log(log, id);
        if (obj != NULL) { return obj; }
    }

    // Snoop C1 chain
    log = atomic_ldacq(&store->c1);
    while (log != NULL) {
        obj = snoop_log(log, id);
        if (obj != NULL) { return obj; }
        log = atomic_ldacq(&log->next);
    }

    return NULL;
}

// TODO(NeGate): we want this data sorted...
void store_iter(void fn(KObjectID id, KObject* obj)) {
    int core_id = cpu_get_index();

    // Iter C0
    FOR_N(i, 0, boot_info->core_count) {
        log = atomic_ldacq(&store->c0[i]);
        obj = snoop_log(log, id);
        if (obj != NULL) { return obj; }
    }

    // Snoop C1 chain
    log = atomic_ldacq(&store->c1);
    while (log != NULL) {
        obj = snoop_log(log, id);
        if (obj != NULL) { return obj; }
        log = atomic_ldacq(&log->next);
    }
}

void store_dump_all(void) {
    kprintf("=== OBJECTS ===\n");
    kprintf("%-14s %-16s %-16s  %-14s %-10s\n", "ID", "Type", "Rawptr", "Parent", "Status");
    FOR_N(i, 0, count) {
        KObjectID id = store_events[i].k;
        KObject* obj = store_events[i].v;

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
            kprintf("%-14ld %-10s", env ? env->super.id : 0, status);

            if (t->client.is_dead) {
                kprintf("(Just died)");
            } else if (t->client.is_blocked) {
                kprintf("(Just blocked)");
            }
        } else {
            kprintf("%-14s %-10s", "_", "");
        }
        kprintf("\n");
    }
    kprintf("\n");
}

#if 0
#endif
