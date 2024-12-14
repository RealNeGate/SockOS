// Arch-independent kernel stuff
#pragma once
#include <common.h>
#include <boot_info.h>

enum {
    CHUNK_SIZE = 2*1024*1024
};

#define NBHM_VIRTUAL_ALLOC(size)     memset(kernelfl_alloc(size), 0, size)
#define NBHM_VIRTUAL_FREE(ptr, size) kernelfl_free(ptr)
#define NBHM_ASSERT(x) kassert(x, ":(")
#define NBHM_REALLOC(ptr, size) kernelfl_realloc(ptr, size)
#include <nbhm.h>

typedef struct Env Env;
typedef struct Thread Thread;
typedef struct CPUState CPUState;

typedef struct KObject_VMO KObject_VMO;
typedef struct KObject KObject;

typedef unsigned int KHandle;

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

void rwlock_lock_shared(RWLock* lock);
void rwlock_unlock_shared(RWLock* lock);
void rwlock_lock_exclusive(RWLock* lock);
void rwlock_unlock_exclusive(RWLock* lock);

////////////////////////////////
// Kernel heap
////////////////////////////////
typedef struct KernelFreeList KernelFreeList;
struct KernelFreeList {
    // next node is directly after the end size of this one
    uint64_t size     : 62;
    uint64_t is_free  : 1;
    uint64_t has_next : 1;
    // used to track coalescing correctly
    KernelFreeList* prev;
    char data[];
};

extern KernelFreeList* kernel_free_list;

void* kheap_realloc(void* obj, size_t obj_size);
void* kheap_alloc(size_t obj_size);
void* kheap_zalloc(size_t obj_size);
void kheap_free(void* obj);

////////////////////////////////
// Kernel pool
////////////////////////////////
void* kpool_alloc_chunk(void);
void* kpool_alloc_page(void);
void  kpool_free_chunk(void* ptr);
void  kpool_free_page(void* ptr);

void kpool_init(MemMap* restrict mem_map);
void kpool_subdivide(int num_cores);

////////////////////////////////
// Virtual memory
////////////////////////////////
//
// We maintain 3 main structures for the virtual memory in the Env struct:
// * Page range -> properties.
// * Software map from page -> physical address.
// * Hardware page table.
typedef enum {
    VMEM_PAGE_WRITE  = 1u << 0u,
    VMEM_PAGE_EXEC   = 1u << 1u,
    VMEM_PAGE_USER   = 1u << 2u,
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

typedef struct {
    uintptr_t  translated;
    VMem_Flags flags;
} VMem_PTEUpdate;

uintptr_t vmem_map(Env* env, KHandle vmo, size_t offset, size_t size, VMem_Flags flags);
void vmem_add_range(Env* env, KHandle vmo, uintptr_t vaddr, size_t offset, size_t vsize, VMem_Flags flags);

bool vmem_protect(Env* env, uintptr_t addr, size_t size, VMem_Flags flags);
bool vmem_segfault(Env* env, uintptr_t access_addr, bool is_write, VMem_PTEUpdate* out_update);

////////////////////////////////
// Kernel objects
////////////////////////////////
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
        KOBJECT_MAILBOX,
    } tag;

    atomic_int ref_count;
};

// chunk of virtual memory which can be shared across environments
struct KObject_VMO {
    KObject super; // tag = KOBJECT_VMO

    // page-aligned
    size_t size;

    // simple physical mapping
    uintptr_t paddr;
};

enum { KOBJECT_MAILBOX_SIZE = 65536 };
typedef struct {
    uint16_t handle_count;
    uint16_t byte_count;
    // who is the message being sent to, 0 for "any"
    uint32_t rx_id, tx_id;
    uint8_t data[];
} Message;

typedef struct {
    KObject super; // tag = KOBJECT_MAILBOX

    // TODO(NeGate): we must remove this lock
    // later, it will become an IPC bottleneck.
    Lock lock;

    size_t mask;
    _Atomic(u64) bot, top;

    // each message is aligned to 8b and has the same header
    uint8_t data[];
} KObject_Mailbox;

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

KObject_VMO* vmo_create_physical(uintptr_t addr, size_t size);

KObject_Mailbox* vmo_mailbox_create(void);
void vmo_mailbox_send(KObject_Mailbox* mailbox, size_t handle_count, KHandle* handles, size_t data_size, uint8_t* data);
void vmo_mailbox_recv(KObject_Mailbox* mailbox, size_t handle_count, KHandle* handles, size_t data_size, uint8_t* data);

////////////////////////////////
// Environment (memory + resources accessible bound to some set of threads)
////////////////////////////////
struct Env {
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

        // B+ tree for intervals
        VMem_Node* root;

        // virtual addresses -> committed pages
        NBHM commit_table;

        // hardware page table
        PageTable* hw_tables;
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

Thread* thread_create(Env* env, ThreadEntryFn* entrypoint, uintptr_t arg, uintptr_t stack, size_t stack_size, bool is_user);
void thread_kill(Thread* thread);

////////////////////////////////
// Scheduler
////////////////////////////////
typedef struct {
    Thread *curr, *first, *last;
} ThreadQueue;

struct PerCPU_Scheduler {
    // sum of the weight of all active tasks
    u64 total_weight;

    ThreadQueue active;
    ThreadQueue waiters;
};

void sched_init(void);
void sched_yield(void);
void sched_wait(u64 timeout);

Thread* sched_try_switch(u64 now_time, u64* restrict out_wake_us);

void tq_append(ThreadQueue* tq, Thread* t);

////////////////////////////////
// Arch-specific
////////////////////////////////
PerCPU* cpu_get(void);

void arch_init(int core_id);
uintptr_t arch_canonical_addr(uintptr_t p);

CPUState new_thread_state(void* entrypoint, uintptr_t arg, uintptr_t stack, size_t stack_size, bool is_user);
_Noreturn void do_context_switch(CPUState* state, uintptr_t addr_space);

void* memmap_view(PageTable* address_space, uintptr_t phys_addr, uintptr_t virt_addr, size_t size, VMem_Flags flags);
void memmap_unview(PageTable* address_space, uintptr_t virt_addr, size_t size);
bool memmap_translate(PageTable* address_space, uintptr_t virt, u64* out);

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
