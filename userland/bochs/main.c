#include <beans.h>

#define IPC_RING_IMPL
#include "../ipc_ring.h"

typedef int CmdBufHandle;
typedef struct {
    enum {
        CMD_CLIP,
        CMD_FILL,

        // CMD_IMAGE,
    } type;

    int x, y, w, h;

    union {
        uint32_t color; // CMD_FILL
    };
} Cmd;

typedef struct {
    // including the border
    int x, y, w, h;

    //
    KHandle cmd_buf_vmo;
    size_t  cmd_buf_cap;
    Cmd*    cmd_buf;
} Window;

// HACK(NeGate): make a real allocator
static uint64_t active_win;
static Window windows[64];

// hash cells, each represents 128x128
enum { CELL_SIZE = 128 };
static uint32_t prev_cells[32][32];
static uint32_t curr_cells[32][32];

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

static Window* alloc_window(void) {
    if (active_win == UINT64_MAX) {
        return 0;
    }

    int i = __builtin_ffsll(~active_win) - 1;
    active_win |= 1ull << i;

    memset(&windows[i], 0, sizeof(Window));
    return &windows[i];
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

static int mini(int a, int b) { return a < b ? a : b; }
static int maxi(int a, int b) { return a < b ? b : a; }
static void rect(int x, int y, int w, int h, uint32_t color) {
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
    for (int j = y; j < y2; j++) {
        for (int i = x; i < x2; i++) {
            pixels[i + j*stride] = color;
        }
    }
}

static void dirty_rect(int x, int y, int w, int h) {
    int xx = x / CELL_SIZE, yy = y / CELL_SIZE;
    int x2 = (x + w + CELL_SIZE - 1) / CELL_SIZE;
    int y2 = (y + h + CELL_SIZE - 1) / CELL_SIZE;

    uint32_t hash = x;
    hash = hash*31 + y;
    hash = hash*31 + w;
    hash = hash*31 + h;

    for (int j = yy; j < y2; j++) {
        for (int i = xx; i < x2; i++) {
            curr_cells[j][i] = curr_cells[j][i]*31 + hash;
        }
    }
}

static int cursor_x = 0, cursor_y = 0;
static void redraw_cell(int cell_x, int cell_y) {
    int x = cell_x*CELL_SIZE, y = cell_y*CELL_SIZE;

    // clip the cell
    int w = mini(x + CELL_SIZE, view.width) - x;
    int h = mini(y + CELL_SIZE, view.height) - y;

    int stride = view.stride;
    volatile uint32_t* pixels = &view.curr_fb[y*stride + x];

    // draw background
    uint32_t grad_x = (256*256) / view.width;
    uint32_t grad_y = (256*256) / view.height;
    for (size_t j = 0; j < h; j++) {
        uint32_t g = ((y+j)*grad_y) >> 8u;
        for (size_t i = 0; i < w; i++) {
            uint32_t b = ((x+i)*grad_x) >> 8u;
            pixels[i + j*stride] = 0xFF000000 | (g << 16) | (b << 8);
        }
    }

    // draw cursor (if in range)
    {
        int rx = cursor_x - x;
        int ry = cursor_y - y;
        // clip upper
        int rx2 = mini(rx + 16, CELL_SIZE);
        int ry2 = mini(ry + 16, CELL_SIZE);
        // clip lower
        rx = maxi(rx, 0);
        ry = maxi(ry, 0);
        if (rx < rx2 && ry < ry2) {
            for (size_t j = ry; j < ry2; j++) {
                for (size_t i = rx; i < rx2; i++) {
                    pixels[i + j*stride] = 0xFFFF0000;
                }
            }
        }
    }
}

int _start(KHandle display_pci) {
    uint64_t tsc_freq = syscall(SYS_tsc_freq);

    PCI_Desc pci;
    int res = syscall(SYS_pci_claim_device, display_pci, &pci);

    volatile uint32_t* fb = mem_map(NULL_HANDLE, 0, pci.bars[0], 0, pci.sizes[0], PROT_RW, 0);
    volatile uint16_t* display_mmio = mem_map(NULL_HANDLE, 0, pci.bars[2], 0, pci.sizes[2], PROT_RW, 0);

    volatile uint16_t* vbe = &display_mmio[0x280];
    view.width  = vbe[VBE_XRES];
    view.height = vbe[VBE_YRES];
    view.stride = vbe[VBE_VIRT_WIDTH];

    KHandle root_mailbox = get_root_mailbox();

    // Find USB mouse
    /* KHandle mouse_dev = 0;
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
    } */

    int stride = view.stride;
    uint32_t grad_x = (256*256) / view.width;
    uint32_t grad_y = (256*256) / view.height;
    for (int buffer = 0; buffer < 2; buffer++) {
        view.curr_fb = &fb[buffer ? (stride * view.height) : 0];
        rect(0, 0, view.width, view.height, true);
    }

    for (int j = 0; j < 32; j++) {
        for (int i = 0; i < 32; i++) {
            prev_cells[j][i] = 0xFFFFFFFF;
        }
    }

    int buffer = 0;
    int mult = 0;
    for (;;) {
        uint64_t start = __rdtsc();
        view.curr_fb = &fb[buffer ? (stride * view.height) : 0];

        /* size_t len;
        char* packet;
        for (;;) {
            packet = ipc_try_read(&int_in, &len);
            if (packet == NULL) {
                break;
            }
            cursor_x += packet[1];
            cursor_y += packet[2];
            ipc_read_release(&int_in);
        } */

        // clear cells
        int cell_w = (view.width + CELL_SIZE - 1) / CELL_SIZE;
        int cell_h = (view.height + CELL_SIZE - 1) / CELL_SIZE;
        for (int j = 0; j < cell_h; j++) {
            for (int i = 0; i < cell_w; i++) {
                curr_cells[j][i] = 0;
            }
        }

        // compose all commands
        dirty_rect(cursor_x, cursor_y, 16, 16);

        // incremental redraw
        for (int j = 0; j < cell_h; j++) {
            for (int i = 0; i < cell_w; i++) {
                if (prev_cells[j][i] != curr_cells[j][i]) {
                    redraw_cell(i, j);
                    prev_cells[j][i] = curr_cells[j][i];

                    view.curr_fb[j*stride + i] = 0xFFFFFFFF;
                } else {
                    view.curr_fb[j*stride + i] = 0xFF000000;
                }
            }
        }
        cursor_x += 1;
        mult += 1;

        // swap buffers
        vbe[VBE_Y_OFFSET] = buffer ? view.height : 0;
        // buffer = (buffer + 1) % 2;

        uint64_t elapsed = (__rdtsc() - start) / tsc_freq;
        if (elapsed < 16666) {
            thread_sleep(16666 - elapsed);
        }
    }
}

