#include <common.h>
#include <stdatomic.h>
#include <boot_info.h>
#include <elf.h>
#include "font.h"

BootInfo* boot_info;
static uint16_t cursor;

static void put_char(int ch);
static void put_string(const char* str);
static void put_number(uint64_t x, uint8_t base);
static void kprintf(char *fmt, ...);
static void kernel_halt(void);

#define kassert(cond, ...) ((cond) ? 0 : (kprintf("%s:%d: assertion failed!\n  %s\n  ", __FILE__, __LINE__, #cond), kprintf(__VA_ARGS__), __builtin_trap()))
#define panic(...) (kprintf("%s:%d: panic!\n", __FILE__, __LINE__), kprintf(__VA_ARGS__), __builtin_trap())

// core components
#include "str.c"

#ifdef __x86_64__
#include "arch/x64/x64.c"
#endif

// forward decls
#include "threads.c"

#ifdef __x86_64__
#include "arch/x64/acpi.c"
#include "arch/x64/mem.c"
#include "arch/x64/irq.c"
#include "arch/x64/syscall.c"
#endif

// components which depend on architecture stuff
#define IMPL
#include "threads.c"

static int itoa(uint64_t i, uint8_t base, uint8_t *buf) {
    static const char bchars[] = "0123456789ABCDEF";

    int      pos   = 0;
    int      o_pos = 0;
    int      top   = 0;
    uint8_t tbuf[64];

    if (i == 0 || base > 16) {
        buf[0] = '0';
        buf[1] = '\0';
        return 2;
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
    return o_pos + 1;
}

static void put_number(uint64_t number, uint8_t base) {
    uint8_t buffer[65];
    itoa(number, base, buffer);
    buffer[64] = 0;

    // serial port writing
    for (int i = 0; buffer[i]; i++) {
        while ((io_in8(0x3f8+5) & 0x20) == 0) {}

        io_out8(0x3f8, buffer[i]);
    }
}

static void put_string(const char* str) {
    for (; *str; str++) io_out8(0x3f8, *str);
}

static void put_buffer(const uint8_t* buf, int size) {
    if (size >= 0) {
        for (int i = 0; i < size; i++) io_out8(0x3f8, buf[i]);
    } else {
        for (int i = 0; buf[i]; i++) io_out8(0x3f8, buf[i]);
    }
}

static void put_char(int ch) {
    io_out8(0x3f8, ch);
}

static void draw_sprite(uint32_t color, int ch) {
    int columns = (boot_info->fb.width - 16) / 16;
    int rows = (boot_info->fb.height - 16) / 16;

    int x = (cursor % columns) * 16;
    int y = (cursor / columns) * 16;

    const uint8_t* bitmap = FONT[(int)ch];

    for (size_t yy = 0; yy < 16; yy++) {
        for (size_t xx = 0; xx < 16; xx++) {
            if (bitmap[yy / 2] & (1 << (xx / 2))) {
                boot_info->fb.pixels[(8 + x + xx) + ((8 + y + yy) * boot_info->fb.stride)] = color;
            }
        }
    }

    cursor = (cursor + 1) % (columns * rows);
}

#define _PRINT_BUFFER_LEN 128
static void kprintf(char *fmt, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, fmt);

    uint8_t obuf[_PRINT_BUFFER_LEN];
    uint32_t min_len = 0;
    for (char *c = fmt; *c != 0; c++) {
        if (*c != '%') {
            int i = 0;
            for (; *c != 0 && *c != '%'; i++) {
                if (i > _PRINT_BUFFER_LEN) {
                    put_buffer(obuf, i);
                    i = 0;
                }

                obuf[i] = *c;
                c++;
            }

            if (i > 0) {
                put_buffer(obuf, i);
            }
            c--;
            continue;
        }

        int64_t precision = -1;
        consume_moar:
        c++;
        switch (*c) {
            case '.': {
                c++;
                if (*c != '*') {
                    continue;
                }

                precision = __builtin_va_arg(args, int64_t);
                goto consume_moar;
            } break;
            case '0': {
                c++;
                min_len = *c - '0';
                goto consume_moar;
            } break;
            case 's': {
                uint8_t *s = __builtin_va_arg(args, uint8_t *);
                put_buffer(s, precision);
            } break;
            case 'd': {
                int64_t i = __builtin_va_arg(args, int64_t);
                put_number(i, 10);
            } break;
            case 'x': {
                uint64_t i = __builtin_va_arg(args, uint64_t);

                uint8_t tbuf[64];
                int sz = itoa(i, 16, tbuf);

                int pad_sz = min_len - (sz - 1);
                while (pad_sz > 0) {
                    put_char('0');
                    pad_sz--;
                }

                put_buffer(tbuf, sz - 1);
                min_len = 0;
            } break;
            case 'p': {
                uint64_t i = __builtin_va_arg(args, uint64_t);
                uint8_t tbuf[64];
                int sz = itoa(i, 16, tbuf);
                int pad_sz = 16 - (sz - 1);
                put_char('0');
                put_char('x');
                while (pad_sz > 0) {
                    put_char('0');
                    pad_sz--;
                }
                put_buffer(tbuf, sz - 1);
                min_len = 0;
            } break;
            case 'b': {
                uint64_t i = __builtin_va_arg(args, uint64_t);

                uint8_t tbuf[64];
                int sz = itoa(i, 2, tbuf);

                int pad_sz = min_len - (sz - 1);
                while (pad_sz > 0) {
                    put_char('0');
                    pad_sz--;
                }

                put_buffer(tbuf, sz - 1);
                min_len = 0;
            } break;
        }
    }

    __builtin_va_end(args);
}

void foobar(void) {
    for (size_t i = 500; i < 1000; i++) {
        boot_info->fb.pixels[i] = 0xFF00FFFF;
    }
}

#define STR2(x) #x
#define STR(x) STR2(x)

// this aligns start address to 16 and terminates byte array with explict 0
// which is not really needed, feel free to change it to whatever you want/need
#define INCBIN(name, file) \
__asm__(".section .rodata\n" \
    ".global incbin_" STR(name) "_start\n" \
    ".balign 16\n" \
    "incbin_" STR(name) "_start:\n" \
    ".incbin \"" file "\"\n" \
    \
    ".global incbin_" STR(name) "_end\n" \
    ".balign 1\n" \
    "incbin_" STR(name) "_end:\n" \
    ".byte 0\n" \
); \
extern __attribute__((aligned(16))) const uint8_t incbin_ ## name ## _start[]; \
extern                              const uint8_t incbin_ ## name ## _end[]

INCBIN(test_program, "test2.elf");

static void kernel_halt(void) {
    // TODO(NeGate): we should add some key to spawn a shell once we have that
    kprintf("No tasks running...\n");

    // wait forever
    halt();
}

void kmain(BootInfo* restrict info) {
    boot_info = info;
	kprintf("Beginning kernel boot...\n");

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

    init_physical_page_alloc(&info->mem_map);

    #if 0
    kprintf("%p\n", alloc_physical_page());
    kprintf("%p\n", alloc_physical_page());

    void* p = alloc_physical_page();
    kprintf("%p\n", p);

    kprintf("%p\n", alloc_physical_page());

    free_physical_page(p);
    kprintf("free %p\n", p);

    kprintf("%p\n", alloc_physical_page());
    #endif

    parse_acpi(boot_info->rsdp);

    // interrupts
    threads_init();

    // Threadgroup* toy_process;
    threadgroup_spawn(incbin_test_program_start, incbin_test_program_end - incbin_test_program_start, NULL);
    irq_startup();
}
