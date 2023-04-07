#define KERNEL_VIRTUAL_BASE 0xC0000000

typedef struct {
    uint32_t  width, height;
    uint32_t  stride; // in pixels
    uint32_t* pixels;
} Framebuffer;

enum {
    PAGE_SIZE = 4096,
    KERNEL_STACK_SIZE = 0x200000,
};

typedef struct {
    uint64_t entries[512];
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
    uint64_t type;
    uint64_t base;
    uint64_t pages;
} MemRegion;

typedef struct {
    size_t nregions;
    size_t cap;
    MemRegion* regions;
} MemMap;

typedef struct {
    // used for interrupts
    uint8_t* kernel_stack;
    uint8_t* kernel_stack_top;

    uint8_t* user_stack_scratch;
} PerCPU;

// This is all the crap we throw into the loader
typedef struct {
    PageTable* kernel_pml4; // identity mapped
    PerCPU main_cpu;

    void* rsdp;
    MemMap mem_map;

    uintptr_t elf_physical_ptr;

    // the kernel virtual memory is allocated
    // with a simple bump allocator
    size_t kernel_virtual_used;
    Framebuffer fb;

    // This is initialized by the kernel but put into
    // place by the boot EFI
    uint32_t tss[26];
} BootInfo;

// loader.asm & irq.asm needs these to be here
_Static_assert(offsetof(BootInfo, kernel_pml4) == 0, "the loader is sad");

_Static_assert(offsetof(PerCPU, kernel_stack_top) == 8, "the irq.asm is sad");
_Static_assert(offsetof(PerCPU, user_stack_scratch) == 16, "the irq.asm is sad");
