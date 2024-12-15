#pragma once

#include <common.h>

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

    // NBHM crap
    _Alignas(64) _Atomic uint64_t ebr_time;

    // Logging
    struct LogBuffer* log_buffer;

    // Scheduler info
    struct Thread* current_thread;
    PerCPU_Scheduler* sched;

    #ifdef __x86_64__
    u64 gdt[7];

    // 2 byte limit, 8 byte base
    u16 gdt_pointer[5];

    // This is initialized by the kernel but put into
    // place by the boot EFI
    u32 tss[26];
    #endif
};

// This is all the crap we throw into the loader
typedef struct {
    PageTable* kernel_pml4;
    uintptr_t elf_virtual_entry;
    // identity map starts somewhere after the
    // ELF's loaded position (aligned to 1GiB).
    uintptr_t identity_map_ptr;

    u64 lapic_base;
    u64 ioapic_base;
    u64 tsc_freq;
    u64 apic_tick_in_tsc;

    i32 core_count;
    PerCPU cores[256];

    u64 rsdp_addr;
    MemMap mem_map;

    uintptr_t elf_virtual_ptr;
    uintptr_t elf_physical_ptr;
    size_t elf_mapped_size;

    Framebuffer fb;
} BootInfo;

// loader.s & irq.s needs these to be here
_Static_assert(offsetof(BootInfo, kernel_pml4) == 0, "the loader is sad");
_Static_assert(offsetof(BootInfo, elf_virtual_entry) == 8, "the loader is sad");
_Static_assert(offsetof(BootInfo, identity_map_ptr) == 16, "the loader.s & irq.s are sad");
_Static_assert(offsetof(PerCPU, kernel_stack_top) == 16, "the irq.s & bootstrap.s is sad");
_Static_assert(offsetof(PerCPU, user_stack_scratch) == 24, "the irq.s is sad");

extern BootInfo* boot_info;

// our identity map is somewhere in the higher half
static void* paddr2kaddr(uintptr_t p) { return (void*) (boot_info->identity_map_ptr + p); }
static uintptr_t kaddr2paddr(void* p) { return (uintptr_t) p - boot_info->identity_map_ptr; }

