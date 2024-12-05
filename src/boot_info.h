#include <common.h>

#define KERNEL_VIRTUAL_BASE 0xC0000000

// Physical address
typedef struct { uintptr_t raw; } PAddr;

typedef struct {
    u32  width, height;
    u32  stride; // in pixels
    u32* pixels;
} Framebuffer;

enum {
    PAGE_SIZE = 4096,

    KERNEL_STACK_SIZE   = 0x10000,
    KERNEL_STACK_COOKIE = 0xABCDABCD,

    MAX_CORES = 256,
};

typedef struct {
    _Atomic(u64) entries[512];
} PageTable;

enum {
    KERNEL_CS  = 0x08,
    KERNEL_DS  = 0x10,
    USER_DS    = 0x18,
    USER_CS    = 0x20,
    TSS        = 0x28,
};

typedef enum {
    MEM_REGION_USABLE,
    MEM_REGION_RESERVED,
    MEM_REGION_BOOT,
    MEM_REGION_KSTACK,
    MEM_REGION_KERNEL,
    MEM_REGION_FRAMEBUFFER,
    MEM_REGION_UEFI_BOOT,
    MEM_REGION_UEFI_RUNTIME,
    MEM_REGION_ACPI,
    MEM_REGION_ACPI_NVS,
    MEM_REGION_IO,
    MEM_REGION_IO_PORTS,
} MemRegionType;

typedef struct {
    u64 type;
    u64 base;
    u64 pages;
} MemRegion;

typedef struct {
    size_t nregions;
    size_t cap;
    MemRegion* regions;
} MemMap;

// lock-free queue
typedef struct {
    _Atomic int64_t bot;
    _Atomic int64_t top;
    _Atomic(void*)* data;
} PerCPU_PageAllocator;

typedef struct PageFreeList {
    struct PageFreeList* next;
    char data[];
} PageFreeList;

typedef struct PerCPU_Scheduler PerCPU_Scheduler;
typedef struct PerCPU PerCPU;
struct PerCPU {
    PerCPU* self;

    // used for interrupts
    u8* kernel_stack;
    u8* kernel_stack_top;

    u8* user_stack_scratch;

    u32 core_id, lapic_id;

    // 4KiB page heap
    PageFreeList* heap;

    // 2MiB heap
    _Alignas(64) PerCPU_PageAllocator alloc;

    // Scheduler info
    struct Thread* current_thread;
    PerCPU_Scheduler* sched;
};

// This is all the crap we throw into the loader
typedef struct {
    PageTable* kernel_pml4;
    uintptr_t elf_virtual_entry;
    // identity map starts somewhere after the
    // ELF's loaded position (aligned to 1GiB).
    uintptr_t identity_map_ptr;

    u64 lapic_base;
    u64 tsc_freq;
    u64 apic_tick_in_tsc;

    i32 core_count;
    PerCPU cores[256];

    void* rsdp;
    MemMap mem_map;

    uintptr_t elf_virtual_ptr;
    uintptr_t elf_physical_ptr;
    size_t elf_mapped_size;

    Framebuffer fb;

    // This is initialized by the kernel but put into
    // place by the boot EFI
    u32 tss[26];
} BootInfo;

// loader.s & irq.s needs these to be here
_Static_assert(offsetof(BootInfo, kernel_pml4) == 0, "the loader is sad");
_Static_assert(offsetof(BootInfo, elf_virtual_entry) == 8, "the loader is sad");
_Static_assert(offsetof(BootInfo, identity_map_ptr) == 16, "the loader is sad");
_Static_assert(offsetof(PerCPU, kernel_stack_top) == 16, "the irq.s & bootstrap.s is sad");
_Static_assert(offsetof(PerCPU, user_stack_scratch) == 24, "the irq.s is sad");

PerCPU* cpu_get(void);
