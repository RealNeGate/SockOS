#define KERNEL_VIRTUAL_BASE 0xC0000000

typedef struct {
    uint32_t  width, height;
    uint32_t  stride; // in pixels
    uint32_t* pixels;
} Framebuffer;

enum {
    PAGE_SIZE = 4096
};

typedef struct {
    uint64_t entries[512];
} PageTable;

typedef enum {
    MEM_REGION_USABLE,
    MEM_REGION_RESERVED,
    MEM_REGION_BOOT,
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

// This is all the crap we throw into the loader
typedef struct {
    PageTable* kernel_pml4; // identity mapped
	size_t     pt_used;
    size_t     pt_capacity;
    void*      rsdp;

    MemMap mem_map;

    // the kernel virtual memory is allocated
    // with a simple bump allocator
    size_t kernel_virtual_used;
    Framebuffer fb;
} BootInfo;

// loader.asm needs these to be here
_Static_assert(offsetof(BootInfo, kernel_pml4) == 0, "the loader is sad 2");
