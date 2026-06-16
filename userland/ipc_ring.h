#ifndef IPC_RING_H
#define IPC_RING_H


#include <beans.h>

typedef struct {
    uint64_t items[];
} IPC_Slot;

typedef struct {
    size_t cap, block_size;

    // Written by the consumer
    _Alignas(64) _Atomic(uint64_t) head;

    // Written by the producer
    _Alignas(64) _Atomic(uint64_t) tail;

    _Alignas(64) _Atomic(uint64_t) items[];
} IPC_Ring;

typedef struct {
    IPC_Ring* ring;
    int index;

    // producer starts at false, consumer starts at true
    bool cycle_bit;
} IPC_Endpoint;

IPC_Ring* ipc_ring_alloc(size_t block_size, size_t cap, KHandle* out_vmo);
char* ipc_read_acquire(IPC_Endpoint* restrict ep, uint64_t* out_len);
void ipc_read_release(IPC_Endpoint* restrict ep);
char* ipc_write_reserve(IPC_Endpoint* restrict ep);
void ipc_write_commit(IPC_Endpoint* restrict ep, uint64_t len);

#endif /* IPC_RING_H */

#ifdef IPC_RING_IMPL
static char* ipc_slot_buffer(IPC_Ring* ring) {
    return (char*) &ring->items[ring->cap];
}

IPC_Ring* ipc_ring_alloc(size_t block_size, size_t cap, KHandle* out_vmo) {
    // TODO(NeGate): probably should check for overflow
    size_t size = sizeof(IPC_Ring);
    size += cap*sizeof(uint64_t);
    size += cap*block_size;
    // round to page
    size = (size + 4095ull) & -4096ull;

    KHandle ring_vmo = syscall(SYS_vmo_create, 0, size);
    IPC_Ring* ring = mmap(0, ring_vmo, 0, size, PROT_READ | PROT_WRITE, 0);
    ring->cap = cap;
    ring->block_size = block_size;

    if (out_vmo) { *out_vmo = ring_vmo; }
    return ring;
}

IPC_Endpoint ipc_endpoint(IPC_Ring* ring, bool is_consumer) {
    return (IPC_Endpoint){ ring, 0, is_consumer };
}

char* ipc_read_acquire(IPC_Endpoint* restrict ep, uint64_t* out_len) {
    IPC_Ring* ring = ep->ring;
    int index = ep->index;

    // TODO(NeGate): futex here to wait in case the ring is blocked up
    uint64_t item;
    do {
        item = atomic_load_explicit(&ring->items[index], memory_order_acquire);
    } while ((item & 1) != ep->cycle_bit);

    char* slots = ipc_slot_buffer(ring);
    char* buf = &slots[index * ring->block_size];
    *out_len = item >> 1ull;

    // advance index
    if (++ep->index == ring->cap) {
        ep->index = 0;
    }
    return buf;
}

void ipc_read_release(IPC_Endpoint* restrict ep) {
    IPC_Ring* ring = ep->ring;
    atomic_store_explicit(&ring->items[ep->index], !ep->cycle_bit, memory_order_release);
}

char* ipc_write_reserve(IPC_Endpoint* restrict ep) {
    IPC_Ring* ring = ep->ring;
    int index = ep->index;

    // TODO(NeGate): futex here to wait in case the ring is blocked up
    uint64_t item;
    do {
        item = atomic_load_explicit(&ring->items[index], memory_order_acquire);
    } while ((item & 1) != ep->cycle_bit);

    char* slots = ipc_slot_buffer(ring);
    return &slots[index * ring->block_size];
}

void ipc_write_commit(IPC_Endpoint* restrict ep, uint64_t len) {
    IPC_Ring* ring = ep->ring;
    int index = ep->index;
    atomic_store_explicit(&ring->items[index], (len << 1) | !ep->cycle_bit, memory_order_release);

    // advance index
    if (++ep->index == ring->cap) {
        ep->index = 0;
    }
}

#endif /* IPC_RING_IMPL */
