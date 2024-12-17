#include <stdint.h>
#include <stddef.h>

#include "syscall_helper.h"

typedef unsigned int KHandle;

typedef enum {
    #define X(name, ...) SYS_ ## name,
    #include "../src/kernel/syscall_table.h"

    SYS_MAX,
} SyscallNum;

void foo(void* arg) {
    for (;;) {
        syscall(SYS_test);
    }
}

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

int _start(KHandle bootstrap_channel) {
    uint32_t video_devs[] = { 0x12341111 };
    int display_pci = syscall(SYS_pci_claim_device, 1, video_devs);

    size_t size;
    int fb_bar = syscall(SYS_pci_get_bar, display_pci, 0, &size);
    uint32_t* fb = (uint32_t*) syscall(SYS_mmap, fb_bar, 0, size);

    int vbe_bar = syscall(SYS_pci_get_bar, display_pci, 2, &size);
    volatile uint16_t* display_mmio = (volatile uint16_t*) syscall(SYS_mmap, vbe_bar, 0, size);

    volatile uint16_t* vbe = &display_mmio[0x280];
    int width  = vbe[VBE_XRES], height = vbe[VBE_YRES];
    int stride = vbe[VBE_VIRT_WIDTH];

    uint8_t mult = 0;
    int buffer = 0;
    for (;;) {
        uint32_t* pixels = &fb[buffer ? (stride * height) : 0];
        uint64_t gradient_x = (width + 255) / 256;
        uint64_t gradient_y = (height + 255) / 256;

        for (size_t j = 0; j < height; j++) {
            uint32_t g = ((j / gradient_y) / 2) + 0x7F;
            g = (g + mult) & 0xFF;

            for (size_t i = 0; i < width; i++) {
                uint32_t b = ((i / gradient_x) / 2) + 0x7F;
                b = (b + mult) & 0xFF;

                pixels[i + (j * stride)] = 0xFF000000 | (g << 16) | (b << 8);
            }
        }

        // swap buffers
        vbe[VBE_Y_OFFSET] = buffer ? height : 0;
        buffer = (buffer + 1) % 2;

        syscall(SYS_sleep, 8*1000);
        mult += 1;
    }
    return 0;
}
