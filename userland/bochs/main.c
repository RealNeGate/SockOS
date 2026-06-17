#include <beans.h>

#define IPC_RING_IMPL
#include "../ipc_ring.h"

void* memset(void* buffer, int c, size_t n) {
    char* buf = (char*)buffer;
    for (size_t i = 0; i < n; i++) {
        buf[i] = c;
    }
    return buffer;
}

void* memcpy(void* dest, const void* src, size_t n) {
    char* d = (char*)dest;
    char* s = (char*)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dest;
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

static struct {
    int width, height, stride;
    volatile uint32_t* curr_fb;
} view;

static void rect(int x, int y, int w, int h, bool inv) {
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
    if (inv) {
        uint32_t grad_x = (256*256) / view.width;
        uint32_t grad_y = (256*256) / view.height;
        for (size_t j = y; j < y2; j++) {
            uint32_t g = (j*grad_y) >> 8u;
            for (size_t i = x; i < x2; i++) {
                uint32_t b = (i*grad_x) >> 8u;
                view.curr_fb[i + j*stride] = 0xFF000000 | (g << 16) | (b << 8);
            }
        }
    } else {
        for (size_t j = y; j < y2; j++) {
            for (size_t i = x; i < x2; i++) {
                pixels[i + j*stride] = 0xFFFF0000;
            }
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

    KHandle root_mailbox = syscall(SYS_get_root_mailbox);

    // Find USB mouse
    KHandle mouse_dev = 0;
    IPC_Endpoint int_in = { 0 };
    for (;;) {
        mailbox_send(root_mailbox, 1, 1, 0, NULL, &mouse_dev);
        if (mouse_dev != 0) {
            // Grab endpoint
            KHandle ring_vmo = 0;
            mailbox_send(mouse_dev, 3, 2, 0, NULL, &ring_vmo);

            int_in = ipc_endpoint_from_vmo(ring_vmo, true);
            break;
        }
        syscall(SYS_sleep, 100000);
    }

    int stride = view.stride;
    uint32_t grad_x = (256*256) / view.width;
    uint32_t grad_y = (256*256) / view.height;
    for (int buffer = 0; buffer < 2; buffer++) {
        view.curr_fb = &fb[buffer ? (stride * view.height) : 0];
        rect(0, 0, view.width, view.height, true);
    }

    int cursor_pos[4] = { 0 };

    int buffer = 0;
    int mult = 0;
    for (;;) {
        uint64_t start = __rdtsc();
        view.curr_fb = &fb[buffer ? (stride * view.height) : 0];
        int* curr = &cursor_pos[buffer ? 2 : 0];
        int* prev = &cursor_pos[buffer ? 0 : 2];

        // invalidate cursor from frame[-2]
        rect(curr[0], curr[1], 64, 64, true);
        curr[0] = prev[0], curr[1] = prev[1];

        size_t len;
        char* packet = ipc_try_read(&int_in, &len);
        if (packet) {
            curr[0] += packet[1];
            curr[1] += packet[2];
            ipc_read_release(&int_in);
        }

        // draw cursor
        rect(curr[0], curr[1], 64, 64, false);
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

