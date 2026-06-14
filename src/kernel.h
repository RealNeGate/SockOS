// Arch-independent kernel stuff
#pragma once
#include <common.h>
#include "boot_info.h"
#include "kernel/printf.h"

enum {
    CHUNK_SIZE = 2*1024*1024,
};

typedef struct Env Env;
typedef struct Thread Thread;
typedef struct CPUState CPUState;

typedef struct KObject_VMO KObject_VMO;
typedef struct KObject KObject;

typedef unsigned int KHandle;

// Kernel Array Bounds Check
#define kabc(i, arr) kassert(i < ELEM_COUNT(arr), "Out of bounds access of %s[%d]", #arr, i)
#define kassert(cond, ...) ((cond) ? 0 : (kprintf("%s:%d: assertion failed!\n  %s\n  ", __FILE__, __LINE__, #cond), kprintf(__VA_ARGS__), kprintf("\n\n"), arch_backtrace(), __builtin_trap()))
#define panic(...) (kprintf("%s:%d: panic!\n", __FILE__, __LINE__), kprintf(__VA_ARGS__), __builtin_trap())

////////////////////////////////
// Spin-lock
////////////////////////////////
typedef _Atomic(uint32_t) Lock;
void spin_lock(Lock* lock);
void spin_unlock(Lock* lock);

// bootleg stdio.h
void kprintf(const char *fmt, ...);
void print_ring_init(void);

void arch_backtrace(void);
uint64_t get_time_ticks(void);

PerCPU* cpu_get(void);
size_t cpu_get_index(void);
void sleep(u64 timeout);

////////////////////////////////
// Kernel heap
////////////////////////////////
void  kheap_init(MemMap* mem_map);
void  kheap_multicore(size_t num_cores);
void* kheap_alloc(size_t size);
void* kheap_zalloc(size_t size);
void  kheap_free(void* obj, size_t size);
void  kheap_dump(void);

#define NBHM_ASSERT(x) kassert(x, ":(")
#include "nbhm.h"

////////////////////////////////
// Profiling
////////////////////////////////
void spall_header(void);
void spall_begin_event(const char* name, int tid);
void spall_end_event(int tid);

////////////////////////////////
// Read-write lock
////////////////////////////////
// bottom bit means there's an exclusive lock.
typedef _Atomic(uint32_t) RWLock;

bool rwlock_try_lock_shared(RWLock* lock);
bool rwlock_is_exclusive(RWLock* lock);

void rwlock_lock_shared(RWLock* lock);
void rwlock_unlock_shared(RWLock* lock);
void rwlock_lock_exclusive(RWLock* lock);
void rwlock_unlock_exclusive(RWLock* lock);

////////////////////////////////
// Virtual memory
////////////////////////////////
//
// We maintain 3 main structures for the virtual memory in the Env struct:
// * Page range -> properties.
// * Software map from page -> physical address.
// * Hardware page table.
typedef enum {
    VMEM_PAGE_WRITE     = 1u << 0u,
    VMEM_PAGE_EXEC      = 1u << 1u,
    VMEM_PAGE_KERNEL    = 1u << 2u,
    VMEM_PAGE_PINNED    = 1u << 3u,
    VMEM_PAGE_UNCACHED  = 1u << 4u,
    VMEM_PAGE_WRITETHRU = 1u << 5u,
} VMem_Flags;

// B tree nodes
enum {
    VMEM_NODE_DEGREE   = 8,
    VMEM_NODE_MAX_KEYS = VMEM_NODE_DEGREE*2 - 1,
    VMEM_NODE_MAX_VALS = VMEM_NODE_DEGREE*2,
};

typedef struct {
    uint64_t valid : 1;
    uint64_t flags : 7;
    uint64_t vmo_handle : 32;

    // range
    size_t offset, size;
} VMem_PageDesc;

typedef struct VMem_Node VMem_Node;
struct VMem_Node {
    VMem_Node* next;

    uint8_t is_leaf   : 1;
    uint8_t key_count : 7;

    uintptr_t keys[VMEM_NODE_MAX_KEYS];
    union {
        VMem_Node* kids[0];    // [VMEM_NODE_MAX_VALS]
        VMem_PageDesc vals[0]; // [VMEM_NODE_MAX_VALS]
    };
};

typedef struct {
    VMem_Node* node;
    size_t index;
} VMem_Cursor;

// virtual addresses -> committed pages
typedef NBHM VMem_WorkingSet;

uintptr_t vmem_map(Env* env, KHandle vmo, uintptr_t vaddr, size_t offset, size_t size, VMem_Flags flags, uintptr_t* out_paddr);
void vmem_add_range(Env* env, KHandle vmo, uintptr_t vaddr, size_t offset, size_t vsize, VMem_Flags flags);
VMem_Cursor vmem_node_lookup(Env* env, uintptr_t key);

// maps a kernel page to a virtual address.
void vmem_commit_page(Env* env, uintptr_t vaddr, void* kaddr);

uintptr_t vmem_translate(VMem_WorkingSet* ws, uintptr_t vaddr);
uintptr_t vmem_try_commit(Env* env, VMem_PageDesc* desc, uintptr_t access_addr, uintptr_t start_addr, uintptr_t end_addr);

bool vmem_protect(Env* env, uintptr_t addr, size_t size, VMem_Flags flags);
bool vmem_segfault(Env* env, uintptr_t access_addr, bool is_write);

void vmem_dump(Env* env);

VMem_Cursor vmem_cursor_first(Env* env);
VMem_Cursor vmem_cursor_next(VMem_Cursor cur);

////////////////////////////////
// Kernel objects
////////////////////////////////
typedef struct {
    Lock lock;
    Thread* thread;
} WaitQueue;

typedef enum {
    KACCESS_,
} KAccessRights;

// these exist beyond environments (and can be shared across them), this is
// the header for all of them.
struct KObject {
    enum {
        // processes
        KOBJECT_ENV,
        KOBJECT_THREAD,
        // memory
        KOBJECT_VMO,
        // IPC
        KOBJECT_PIPE,
        KOBJECT_MAILBOX,
        KOBJECT_EVENT,
        // Devices
        KOBJECT_DEV_PCI,
    } tag;
};

// chunk of virtual memory which can be shared across environments
struct KObject_VMO {
    KObject super; // tag = KOBJECT_VMO
    VMem_Flags flags;
    size_t size; // page-aligned

    // simple physical mapping, if paddr=0 then we use the working set
    uintptr_t paddr;
    VMem_WorkingSet pages;
};

// Ring buffer of stacks
typedef struct {
    KObject super; // tag = KOBJECT_MAILBOX
    u32 cap_log2;
    void* handler_pc;

    WaitQueue tx_wait; // blocked on send()

    _Alignas(64) atomic_u64 head;
    _Alignas(64) atomic_u64 tail;

    atomic_u64 ids_n_items[];
} KObject_Mailbox;

typedef struct KObject_Event KObject_Event;
struct KObject_Event {
    KObject super; // tag = KOBJECT_EVENT

    _Alignas(64) _Atomic(Thread*) waiting_thread;
};

// 10 cache lines worth of handles
typedef struct {
    // each bit (except MSB) represents whether or not one of the handles is alive.
    // MSB represents the bitfield can no longer be written to because it has been
    // moved.
    _Atomic(u64) open;

    // each ptr is tagged on the top 16bits
    _Atomic(u64) objects[63];
} KHandleEntry;

typedef struct KHandleTable KHandleTable;
struct KHandleTable {
    _Atomic(KHandleTable*) prev;

    // item migration work
    _Atomic(size_t) claimed, done;

    size_t capacity;
    KHandleEntry entries[];
};

typedef struct PCI_Device PCI_Device;

enum { PCI_MAX_DEVICES = 20 };
extern int pci_dev_count;
extern PCI_Device* pci_devs[PCI_MAX_DEVICES];

KObject_VMO* vmo_create_physical(uintptr_t addr, size_t size, VMem_Flags flags);

KObject_Mailbox* mailbox_create(size_t max_requests);
// return the thread we'll be using the respond
Thread* mailbox_send(KObject_Mailbox* mailbox);
// hand the thread back, we're now waiting for new messages
bool mailbox_recv(KObject_Mailbox* mailbox, Thread* thread);

KObject_Event* event_create(void);
Thread* event_signal(KObject_Event* restrict event);
bool event_wait(KObject_Event* restrict event, Thread* thread);

////////////////////////////////
// Environment (memory + resources accessible bound to some set of threads)
////////////////////////////////
struct Env {
    KObject super; // tag = KOBJECT_ENV

    Lock lock;

    Thread* first_in_env;
    Thread* last_in_env;

    struct {
        // read-write lock is needed since multiple "forward progress" modifications can occur at once, while only
        // one "backwards progress" can occur exclusively.
        //
        //   "forward progress" in this context means the modification to the page table will produce at the same
        //   or less segfaults as the old form, the reason this is important is because it means we can hide the
        //   act of TLB updates and invalidation behind segfaults if a core were to see an out-dated view of the page
        //   table. A simple example
        //
        //   "backwards progress" requires everyone to acknowledge the changes. For instance, unmapping a page requires everyone
        //   to acknowledge it or else the data might be corrupted, miss an important segfault.
        RWLock lock;
        //   when TLB locked, this is the only thread which isn't considered blocked.
        _Atomic(Thread*) tlb_lock;

        // B+ tree for intervals
        VMem_Node* root;

        // commit table
        VMem_WorkingSet working_set;

        // hardware page table
        PageTable* hw_tables;

        // TLB shootdown checkpoint
        _Alignas(64) atomic_u32 checkpoint_done;
    } addr_space;

    // Handle table
    _Atomic(KHandleTable*) handles;
};

Env* env_create(void);
void env_kill(Env* env);
Thread* env_load_elf(Env* env, const u8* program, size_t program_size);

void* env_get_handle(Env* env, KHandle handle, KAccessRights* rights);
KHandle env_open_handle(Env* env, KAccessRights rights, KObject* obj);
bool env_close_handle(Env* env, KHandle handle);

////////////////////////////////
// Threads
////////////////////////////////
typedef int ThreadEntryFn(void*);

Thread* thread_create(Env* env, ThreadEntryFn* entrypoint, uintptr_t arg, uintptr_t stack, size_t stack_size);
void thread_resume(Thread* thread, PerCPU* cpu);
void thread_kill(Thread* thread);

void waitqueue_wait(WaitQueue* wq, Thread* t);
Thread* waitqueue_wake(WaitQueue* wq, PerCPU* cpu);
void waitqueue_broadcast(WaitQueue* wq);

////////////////////////////////
// Scheduler
////////////////////////////////
typedef struct {
    int count;
    Thread* data[256];
} ThreadQueue;

struct PerCPU_Scheduler {
    Lock lock;

    atomic_bool idleing;
    atomic_u64 total_exec_time;

    // in micros
    u64 ideal_exec_time;
    u64 min_exec_time;

    ThreadQueue active;
    ThreadQueue waiters;
};

void sched_init(void);
void sched_yield(void);
void sched_wait(u64 timeout);
int sched_load_balancer(void*);

u64 sched_total_exec_time(PerCPU* cpu, u64 now_time);
Thread* sched_pick_next(PerCPU* cpu, u64 now_time, u64* restrict out_wake_us);

void tq_insert(ThreadQueue* tq, Thread* t, bool is_waiter);
void tq_append(ThreadQueue* tq, Thread* t);

////////////////////////////////
// Arch-specific
////////////////////////////////
PerCPU* cpu_get(void);

void arch_init(int core_id);
void arch_handoff(int core_id);
void arch_wake_up(int core_id);
uintptr_t arch_canonical_addr(uintptr_t p);
void arch_set_address_space(Env* env);
void arch_pte_update(Env* env, uintptr_t access_addr, uintptr_t translated, VMem_Flags flags);

// broadcast to all cores running an Env that we've modified the address space
void arch_tlb_shootdown(Env* env);
void arch_backtrace(void);

CPUState new_thread_state(void* entrypoint, uintptr_t arg, uintptr_t stack, size_t stack_size, bool is_user);
_Noreturn void do_context_switch(CPUState* state, uintptr_t addr_space);

void* memmap_view(PageTable* address_space, uintptr_t phys_addr, uintptr_t virt_addr, size_t size, VMem_Flags flags);
void memmap_unview(PageTable* address_space, uintptr_t virt_addr, size_t size);
bool memmap_translate(PageTable* address_space, uintptr_t virt, u64* out);

// Signal this waiting thread to wake up when the interrupt is hit.
void set_interrupt_line(PCI_Device* pci_dev, KObject_Event* event);

// we emulate I/O ports on all platforms, it's used for PCI mostly
u8 io_in8(u16 port);
u16 io_in16(u16 port);
u32 io_in32(u16 port);

void io_out8(u16 port, u8 value);
void io_out16(u16 port, u16 value);
void io_out32(u16 port, u32 value);

////////////////////////////////
// Utils
////////////////////////////////
uint32_t mur3_32(const void *key, int len, uint32_t h);
int itoa(u64 v, uint8_t *buffer, uint8_t base);
