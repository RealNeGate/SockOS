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

void foobar(BootInfo *info) {
	for (size_t i = 500; i < 1000; i++) {
		info->fb.pixels[i] = 0xFF00FFFF;
	}
}

void kmain(BootInfo* info) {
	for (size_t i = 1000; i < 2000; i++) {
		info->fb.pixels[i] = 0xFF00FF00;
	}

	foobar(info);
}
