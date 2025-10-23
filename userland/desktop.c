#include "beans.h"

static KHandle mailbox;
void request_handler(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) {
    syscall(SYS_mailbox_yield, mailbox, a + 1);
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

uint8_t mult = 0;
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
}

int _start(KHandle bootstrap_channel) {
    uint32_t video_devs[] = { 0x12341111 };
    KHandle display_pci = syscall(SYS_pci_claim_device, 1, video_devs);

    size_t size;
    int fb_bar = syscall(SYS_pci_get_bar, display_pci, 0, &size);
    uint32_t* fb = (uint32_t*) syscall(SYS_mmap, fb_bar, 0, size);

    int vbe_bar = syscall(SYS_pci_get_bar, display_pci, 2, &size);
    volatile uint16_t* display_mmio = (volatile uint16_t*) syscall(SYS_mmap, vbe_bar, 0, size);

    volatile uint16_t* vbe = &display_mmio[0x280];
    int width  = vbe[VBE_XRES], height = vbe[VBE_YRES];
    int stride = vbe[VBE_VIRT_WIDTH];
    int buffer = 0;

    // create & install mailbox
    mailbox = syscall(SYS_mailbox_create, 8192, 4, request_handler);

    int i = 0;
    for (;;) {
        uint64_t start = __rdtsc();

        draw(width, height, stride, &fb[buffer ? (stride * height) : 0]);
        mult += 1;

        // swap buffers
        vbe[VBE_Y_OFFSET] = buffer ? height : 0;
        buffer = (buffer + 1) % 2;

        uint64_t elapsed = (__rdtsc() - start) / bootstrap_channel;
        if (elapsed < 16666) {
            syscall(SYS_sleep, 16666 - elapsed);
        }

        i = syscall(SYS_mailbox_send, mailbox, i);
    }
    return 0;
}

