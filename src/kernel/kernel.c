#include <common.h>
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

static void put_char(int ch);
static void put_number(uint32_t x);

// core components
#include "kernel/str.c"

#include "arch/x64/mem.c"
#include "arch/x64/irq.c"

static int itoa(uint32_t i, uint8_t base, uint16_t* buf) {
    static const char bchars[] = "0123456789ABCDEF";

    int pos   = 0;
    int o_pos = 0;
    int top   = 0;
    uint16_t tbuf[32];

    if (i == 0 || base > 16) {
        buf[0] = '0';
        buf[1] = '\0';
        return 0;
    }

    while (i != 0) {
        tbuf[pos] = bchars[i % base];
        pos++;
        i /= base;
    }
    top = pos--;

    for (o_pos = 0; o_pos < top; pos--, o_pos++) {
        buf[o_pos] = tbuf[pos];
    }
    buf[o_pos] = 0;
    return o_pos;
}

static void put_number(uint32_t number) {
    uint16_t buffer[32];
    itoa(number, 16, buffer);
    buffer[31] = 0;

    // serial port writing
    for (int i = 0; buffer[i]; i++) {
        while ((io_in8(0x3f8+5) & 0x20) == 0) {}

        io_out8(0x3f8, buffer[i]);
        // put_char(buffer[i]);
    }
}

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

    // put_char('A');

    // interrupts
    irq_startup();
    while (1) {}
}
