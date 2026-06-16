#include <beans.h>

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

static struct {
    int width, height, stride;
    volatile uint32_t* curr_fb;
} view;

static void rect(int x, int y, int w, int h) {
    volatile uint32_t* pixels = view.curr_fb;

    int x2 = x + w;
    int y2 = y + h;

    // Fully clipped
    if (x2 < 0 || y2 < 0) {
        return;
    }

    // Partial Clipping
    if (x2 > view.width)  { x2 = view.width;  }
    if (y2 > view.height) { y2 = view.height; }
    if (x  < 0)           { x  = 0; }
    if (y  < 0)           { y  = 0; }

    int stride = view.stride;
    for (size_t j = y; j < y2; j++) {
        for (size_t i = x; i < x2; i++) {
            pixels[i + j*stride] = 0xFFFF0000;
        }
    }
}

int _start(KHandle display_pci) {
    uint64_t tsc_freq = syscall(SYS_tsc_freq);

    size_t size;
    int fb_bar = syscall(SYS_pci_get_bar, display_pci, 0, &size);
    volatile uint32_t* fb = mmap(0, fb_bar, 0, size, PROT_READ | PROT_WRITE, 0);

    int vbe_bar = syscall(SYS_pci_get_bar, display_pci, 2, &size);
    volatile uint16_t* display_mmio = mmap(0, vbe_bar, 0, size, PROT_READ | PROT_WRITE, 0);

    volatile uint16_t* vbe = &display_mmio[0x280];
    view.width  = vbe[VBE_XRES];
    view.height = vbe[VBE_YRES];
    view.stride = vbe[VBE_VIRT_WIDTH];

    int buffer = 0;
    int mult = 0;
    for (;;) {
        uint64_t start = __rdtsc();

        // TODO(NeGate): receive command buffers from windows on the system
        uint64_t gradient_x = 64; // (width + 255) / 256;
        uint64_t gradient_y = 64; // (height + 255) / 256;

        int stride = view.stride;
        view.curr_fb = &fb[buffer ? (stride * view.height) : 0];
        for (size_t j = 0; j < view.height; j++) {
            uint32_t g = (j % gradient_y) * 4;
            for (size_t i = 0; i < view.width; i++) {
                uint32_t b = ((i + mult) % gradient_x) * 4;
                view.curr_fb[i + j*stride] = 0xFF000000 | (g << 16) | (b << 8);
            }
        }

        // Draw cursor
        rect(0, 0, 64, 64);
        mult += 1;

        // swap buffers
        vbe[VBE_Y_OFFSET] = buffer ? view.height : 0;
        buffer = (buffer + 1) % 2;

        uint64_t elapsed = (__rdtsc() - start) / tsc_freq;
        if (elapsed < 16666) {
            syscall(SYS_sleep, 16666 - elapsed);
        }
    }
}

