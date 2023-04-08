#include <common.h>

#define KERNEL_VIRTUAL_BASE 0xC0000000

typedef struct {
    u32  width, height;
    u32  stride; // in pixels
    u32* pixels;
} Framebuffer;

enum {
    PAGE_SIZE = 4096,
    KERNEL_STACK_SIZE = 0x200000,
};

typedef struct {
    u64 entries[512];
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

typedef struct {
    // used for interrupts
    u8* kernel_stack;
    u8* kernel_stack_top;

    u8* user_stack_scratch;
} PerCPU;

// This is all the crap we throw into the loader
typedef struct {
    PageTable* kernel_pml4; // identity mapped
    PerCPU main_cpu;

    u64 tsc_freq;
    u32 core_ids[256];
    i32 core_count;

    void* rsdp;
    MemMap mem_map;

    uintptr_t elf_physical_ptr;

    // the kernel virtual memory is allocated
    // with a simple bump allocator
    size_t kernel_virtual_used;
    Framebuffer fb;

    // This is initialized by the kernel but put into
    // place by the boot EFI
    u32 tss[26];
} BootInfo;

// loader.asm & irq.asm needs these to be here
_Static_assert(offsetof(BootInfo, kernel_pml4) == 0, "the loader is sad");

_Static_assert(offsetof(PerCPU, kernel_stack_top) == 8, "the irq.asm is sad");
_Static_assert(offsetof(PerCPU, user_stack_scratch) == 16, "the irq.asm is sad");
