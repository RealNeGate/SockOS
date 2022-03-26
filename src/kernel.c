#include <stdint.h>
#include <stddef.h>

typedef struct {
	uintptr_t base, pages;
} MemRegion;

typedef struct {
	uint64_t entries[512];
} PageTable;

// This is all the crap we throw into the loader
typedef struct {
	void* entrypoint;
	PageTable* kernel_pml4;

	size_t mem_region_count;
	MemRegion* mem_regions;

	struct {
		uint32_t width, height;
		uint32_t stride; // in pixels
		uint32_t* pixels;
		// TODO(NeGate): pixel format :p
	} fb;
} BootInfo;

void kernel_main(BootInfo* info) {
	for (size_t i = 0; i < 10000; i++) {
		info->fb.pixels[i] = 0xFF00FF00;
	}
}
