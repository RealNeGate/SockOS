#include <stdint.h>
#include <stddef.h>
#include "font.h"

typedef struct {
    uintptr_t base, pages;
} MemRegion;

typedef struct {
    uint64_t entries[512];
} PageTable;

// This is all the crap we throw into the loader
typedef struct {
    void*      entrypoint;
    PageTable* kernel_pml4;

    size_t     mem_region_count;
    MemRegion* mem_regions;

    struct {
        uint32_t  width, height;
        uint32_t  stride; // in pixels
        uint32_t* pixels;
        // TODO(NeGate): pixel format :p
    } fb;
} BootInfo;

static BootInfo* boot_info;
static uint16_t  cursor;

// core components
#include "kernel/common.h"
#include "arch/x64/irq.c"

static void put_char(int ch) {
    int columns = (boot_info->fb.width - 16) / 16;
    int rows = (boot_info->fb.height - 16) / 16;

    int x = (cursor % columns) * 16;
    int y = (cursor / columns) * 16;
    const uint8_t* bitmap = FONT[(int)ch];

    for (size_t yy = 0; yy < 16; yy++) {
        for (size_t xx = 0; xx < 16; xx++) {
            if (bitmap[yy / 2] & (1 << (xx / 2))) {
                boot_info->fb.pixels[(8 + x + xx) + ((8 + y + yy) * boot_info->fb.stride)] = 0xFFFFFFFF;
            }
        }
    }

    cursor = (cursor + 1) % (columns * rows);
}

void foobar(void) {
    for (size_t i = 500; i < 1000; i++) {
        boot_info->fb.pixels[i] = 0xFF00FFFF;
    }
}

void kmain(BootInfo* info) {
    boot_info = info;

    // Draw fancy background
    uint64_t gradient_x = (boot_info->fb.width + 255) / 256;
    uint64_t gradient_y = (boot_info->fb.height + 255) / 256;

    for (size_t j = 0; j < boot_info->fb.height; j++) {
        uint32_t g = ((j / gradient_y) / 2) + 0x7F;

        for (size_t i = 0; i < boot_info->fb.width; i++) {
            uint32_t b = ((i / gradient_x) / 2) + 0x7F;

            boot_info->fb.pixels[i + (j * boot_info->fb.stride)] = 0xFF000000 | (g << 16) | (b << 8);
        }
    }

    put_char('A');
    put_char('A');
    put_char('A');
    put_char('A');
    put_char('A');
    put_char('A');
    put_char('A');
    put_char('A');

    // interrupts
    irq_startup();
    while (1) {}
}
