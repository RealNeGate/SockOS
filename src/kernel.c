#include <stdint.h>
#include <stddef.h>

//#include "font.h"

typedef struct {
	uintptr_t base, pages;
} MemRegion;

typedef struct {
	uint64_t entries[512];
} PageTable;

// This is all the crap we throw into the loader
typedef struct {
	void*	   entrypoint;
	PageTable* kernel_pml4;

	size_t	   mem_region_count;
	MemRegion* mem_regions;

	struct {
		uint32_t  width, height;
		uint32_t  stride; // in pixels
		uint32_t* pixels;
		// TODO(NeGate): pixel format :p
	} fb;
} BootInfo;

/*static uint16_t cursor;

static void put_char(int ch) {
		int columns = fb_width / 8;
		int rows = fb_height / 8;

		int x = (cursor % columns) * 8;
		int y = (cursor / columns) * 8;
		const uint8_t* bitmap = FONT[(int)ch];

		for (size_t yy = 0; yy < 8; yy++) {
				for (size_t xx = 0; xx < 8; xx++) {
						if (bitmap[yy] & (1 << xx)) {
								fb_pixels[(x+xx) + ((y+yy) * fb_width)] = 0xFFFFFFFF;
						}
				}
		}

		cursor = (cursor + 1) % (columns * rows);
}*/

static BootInfo* boot_info;

void foobar() {
	for (size_t i = 500; i < 1000; i++) {
		boot_info->fb.pixels[i] = 0xFF00FFFF;
	}
}

void kmain(BootInfo* info) {
	boot_info = info;

	for (size_t i = 1000; i < 2000; i++) {
		boot_info->fb.pixels[i] = 0xFF00FF00;
	}

	foobar();
}
