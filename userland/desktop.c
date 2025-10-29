#include "beans.h"
#include <stdbool.h>

typedef uint8_t u8;
#include "../src/boot/term_font.c"

static KHandle mailbox;

int terminal_used;
char terminal_buffer[10000];
void _putchar(char ch) {
    terminal_buffer[terminal_used++] = ch;
}

void request_handler(void* arg) {
    uint64_t msg[4];
    uint64_t fn = syscall(SYS_mailbox_wait, mailbox, msg);
    for (;;) {
        // process message
        // syscall(SYS_test, msg[0]);

        // reply and wait for the next message
        fn = syscall(SYS_mailbox_reply, mailbox, msg, msg[0] + 1, 0);
    }
}

#include "../src/kernel/printf.c"

// Bochs video
enum {
    VBE_ID          = 0x0,
    VBE_XRES        = 0x1,
    VBE_YRES        = 0x2,
    VBE_BPP         = 0x3,
    VBE_ENABLE      = 0x4,
    VBE_BANK        = 0x5,
    VBE_VIRT_WIDTH  = 0x6,
    VBE_VIRT_HEIGHT = 0x7,
    VBE_X_OFFSET    = 0x8,
    VBE_Y_OFFSET    = 0x9
};

uint8_t mult = 0;
static void draw_char(uint32_t* pixels, int stride, int char_x, int char_y, int ch) {
    int offset_x = 16 + char_x * 9;
    int offset_y = 16 + char_y * 16;
    for (int y = 0; y < 16; y++) {
        uint8_t byte = term_font[16 * (uint8_t) ch + y];
        for (int x = 0; x < 8; x++) {
            bool is_fg = (byte >> x) & 1;
            if (!is_fg) {
                continue;
            }
            int pixel_x = offset_x + x;
            int pixel_y = offset_y + y;
            pixels[pixel_x + pixel_y*stride] = 0xFFFFFFFF;
        }
    }
}

void draw(int width, int height, int stride, uint32_t* pixels) {
    uint64_t gradient_x = 64; // (width + 255) / 256;
    uint64_t gradient_y = 64; // (height + 255) / 256;

    for (size_t j = 0; j < height; j++) {
        uint32_t g = (j % gradient_y) * 4;
        for (size_t i = 0; i < width; i++) {
            uint32_t b = ((i + mult) % gradient_x) * 4;
            pixels[i + (j * stride)] = 0xFF000000 | (g << 16) | (b << 8);
        }
    }

    // draw text
    int i = 0;
    int x = 0, y = 0;
    while (i < terminal_used) {
        char ch = terminal_buffer[i++];
        if (ch == '\n') {
            x = 0, y += 1;
        } else {
            draw_char(pixels, stride, x, y, ch);
            x += 1;
        }
    }
}

static int bochs_vbe_driver(KHandle display_pci, uint64_t freq) {
    size_t size;
    int fb_bar = syscall(SYS_pci_get_bar, display_pci, 0, &size);
    uint32_t* fb = (uint32_t*) syscall(SYS_mmap, fb_bar, 0, size);

    int vbe_bar = syscall(SYS_pci_get_bar, display_pci, 2, &size);
    volatile uint16_t* display_mmio = (volatile uint16_t*) syscall(SYS_mmap, vbe_bar, 0, size);

    volatile uint16_t* vbe = &display_mmio[0x280];
    int width  = vbe[VBE_XRES], height = vbe[VBE_YRES];
    int stride = vbe[VBE_VIRT_WIDTH];
    int buffer = 0;

    int i = 0;
    for (;;) {
        uint64_t start = __rdtsc();

        draw(width, height, stride, &fb[buffer ? (stride * height) : 0]);
        mult += 1;

        // swap buffers
        vbe[VBE_Y_OFFSET] = buffer ? height : 0;
        buffer = (buffer + 1) % 2;

        uint64_t elapsed = (__rdtsc() - start) / freq;
        if (elapsed < 16666) {
            syscall(SYS_sleep, 16666 - elapsed);
        }

        i = syscall(SYS_mailbox_send, mailbox, 0, i, 0, 0);
    }
}

static int intel_gpu_driver(KHandle display_pci, uint64_t freq) {
    // BDSM_0_2_0_PCI - Mirror of Base Data of Stolen Memory
    uintptr_t base_dsm = syscall(SYS_pci_read_config_32, 0x5C) & 0xFFFFFu;

    // GTTMMADR, low 8MB is MMIO, high 8MB is global GTT
    size_t size;
    int bar0 = syscall(SYS_pci_get_bar, display_pci, 0, &size);
    volatile uint8_t* mmio_gtt = (uint8_t*) syscall(SYS_mmap, bar0, 0, size);

    // GMADR, 4GiB region for main memory
    int bar2 = syscall(SYS_pci_get_bar, display_pci, 2, &size);
    volatile uint8_t* main_mem = (uint8_t*) syscall(SYS_mmap, bar2, 0, size);

    // GGC_0_0_0_PCI - GMCH Graphics Control
    uintptr_t gmch_ctrl = syscall(SYS_pci_read_config_32, 0x50);
    size_t gtt_size     = ((gmch_ctrl >> 8ull) << 20ull);

    // Dump the planes
    //   WM       - XXX40
    //   CTL      - XXX80
    //   STRIDE   - XXX88
    //   POS      - XXX8C
    //   SIZE     - XXX90
    //   SURF     - XXX9C
    //   OFFSET   - XXXA4
    //   SURFLIVE - XXXAC
    for (int pipe = 0; pipe < 3; pipe++) {
        printf("Pipe %d\n", pipe);
        for (int plane = 0; plane < 3; plane++) {
            printf("  Plane %d\n", pipe);

            uint32_t addr = 0x70000 + (pipe * 0x01000) + ((plane+1) * 0x00100);
            uint32_t* plane = (uint32_t*) (mmio_gtt + addr);

            for (int i = 0; i < 64; i++) {
                printf("  [%#x] %#x\n", addr + i*4, plane[i]);
            }
        }
    }

    // Initialize GTT
    /*{
        volatile uint64_t* gtt = (volatile uint64_t*) (mmio_gtt + 0x800000);
        size_t gtt_start = (1920*1080*4) / 4096;
        size_t gtt_end   = gtt_size / 4096;

        // leave the UEFI framebuffer at the base of the stolen memory untouched
        for (size_t i = gtt_start; i < gtt_end; i++) {
            gtt[i] = 0;
        }
    }*/

    return 0;
}

int _start(KHandle bootstrap_channel) {
    terminal_used = 0;

    // create & install mailbox
    mailbox = syscall(SYS_mailbox_create, 4);
    for (int i = 0; i < 4; i++) {
        syscall(SYS_thread_create, request_handler, NULL);
    }

    // We have access to the PS/2 interrupts
    // syscall(SYS_thread_create, mailbox, request_handler);

    // BOCHS VBE display
    static uint32_t bochs_pcids[] = { 0x12341111 };
    KHandle display_pci = syscall(SYS_pci_claim_device, 1, bochs_pcids);
    if (display_pci != 0) {
        bochs_vbe_driver(display_pci, bootstrap_channel);
    }

    uint64_t info[4];
    int fb_bar = syscall(SYS_fb_grab, info);
    uint32_t* fb = (uint32_t*) syscall(SYS_mmap, fb_bar, 0, info[3]);

    // Intel HD graphics
    static uint32_t i915_pcids[] = { 0x80863E92 };
    display_pci = syscall(SYS_pci_claim_device, 1, i915_pcids);
    if (display_pci != 0) {
        printf("Hello, World! handle=%p\n", display_pci);
        intel_gpu_driver(display_pci, bootstrap_channel);
    }

    for (;;) {
        uint64_t start = __rdtsc();

        draw(info[0], info[1], info[2], fb);
        mult += 1;

        uint64_t elapsed = (__rdtsc() - start) / bootstrap_channel;
        if (elapsed < 16666) {
            syscall(SYS_sleep, 16666 - elapsed);
        }
    }

    return 0;
}

