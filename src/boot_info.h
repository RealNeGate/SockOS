#define KERNEL_VIRTUAL_BASE 0xC0000000

enum {
    PAGE_SIZE = 4096
};

typedef struct {
    uint64_t entries[512];
} PageTable;

// This is used by the kernel but filled in by the EFI stub
typedef struct {
    uintptr_t base, pages;
} MemRegion;

// This is all the crap we throw into the loader
typedef struct {
    void*      entrypoint;
    PageTable* kernel_pml4; // identity mapped
    void*      rsdp;

    size_t     mem_region_count;
    MemRegion* mem_regions;

    // the kernel virtual memory is allocated
    // with a simple bump allocator
    size_t kernel_virtual_used;

    struct {
        uint32_t  width, height;
        uint32_t  stride; // in pixels
        uint32_t* pixels;
        // TODO(NeGate): pixel format :p
    } fb;
} BootInfo;

// loader.asm needs these to be here
_Static_assert(offsetof(BootInfo, entrypoint) == 0, "the loader is sad");
_Static_assert(offsetof(BootInfo, kernel_pml4) == 8, "the loader is sad 2");
