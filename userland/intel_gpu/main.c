#include <beans.h>

#define IPC_RING_IMPL
#include "../ipc_ring.h"

#include <emmintrin.h>

typedef uint8_t u8;

static uint64_t tsc_freq;
static bool cursor_state;
static int cursor_x = 0, cursor_y = 0;

void* memset(void* buffer, int c, size_t n) {
    char* buf = (char*)buffer;
    for (size_t i = 0; i < n; i++) {
        buf[i] = c;
    }
    return buffer;
}

void* memcpy(void* dest, const void* src, size_t n) {
    u8* d = (u8*)dest;
    u8* s = (u8*)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return (void*)dest;
}

typedef struct {
    uintptr_t base_dsm;
    uintptr_t gmch_ctrl;

    // VMOs for the BARs
    KHandle bar0, bar2;

    // Mapped BARs
    size_t ggtt_size;
    volatile uint32_t* mmio;
    volatile uint64_t* ggtt;
    volatile uint8_t* main_mem;

    // Cursor stuff
    size_t cursor_paddr;
    size_t cursor_gpu_addr;
    uint32_t* cursor_data;
} IntelGPU;

static IntelGPU gpu;
static void igpu_mmio_write(uint32_t reg, uint32_t val) { gpu.mmio[reg / 4] = val; }
static uint32_t igpu_mmio_read(uint32_t reg) { return gpu.mmio[reg / 4]; }

static uint32_t buf_config(int start, int size) { return start | ((start + size - 1) << 16u); }

static void allocate_buffers(void) {
    // Disable planes
    igpu_mmio_write(0x70080, 0); // Cursor
    igpu_mmio_write(0x70180, 0); // A1
    igpu_mmio_write(0x70084, gpu.cursor_gpu_addr); // CUR_BASE_A
    for (int i = 0; i < 3; i++) {
        uint32_t addr = 0x70100 + (i * 0x100);
        igpu_mmio_write(addr + 0x9C, igpu_mmio_read(addr + 0x9C));
    }

    syscall(SYS_sleep, 16666);

    // igpu_mmio_write(0x7017C, buf_config(0, 32));  // CUR_BUF_CFG_A
    // igpu_mmio_write(0x7027C, buf_config(32, 160)); // PLANE_BUF_CFG_1_A

    igpu_mmio_write(0x7017C, buf_config(0, 32));  // CUR_BUF_CFG_A
    igpu_mmio_write(0x7027C, buf_config(32, 160)); // PLANE_BUF_CFG_1_A

    igpu_mmio_write(0x70140, 0x00000000 | 32);
    igpu_mmio_write(0x70240, 0x00000000 | 160);

    // Disable all transition watermarks
    igpu_mmio_write(0x70168, 0);
    igpu_mmio_write(0x70268, 0);
    igpu_mmio_write(0x70368, 0);
    igpu_mmio_write(0x70468, 0);

    igpu_mmio_write(0x70084, gpu.cursor_gpu_addr); // CUR_BASE_A

    // Re-enable planes
    igpu_mmio_write(0x70180, 0x84000000); // A1

    // Refresh planes
    for (int i = 0; i < 3; i++) {
        uint32_t addr = 0x70100 + (i * 0x100);
        igpu_mmio_write(addr + 0x9C, igpu_mmio_read(addr + 0x9C));
    }

    syscall(SYS_sleep, 16666);

    igpu_mmio_write(0x70080, 0b100111);   // CUR_CONTROL
    igpu_mmio_write(0x70088, 0x00100010); // CUR_POS
    igpu_mmio_write(0x70084, gpu.cursor_gpu_addr); // CUR_BASE_A
}

static void intel_gpu_driver_init(KHandle display_pci) {
    // BDSM_0_2_0_PCI - Mirror of Base Data of Stolen Memory
    gpu.base_dsm = syscall(SYS_pci_read_config_32, display_pci, 0x5C) & 0xFFFFFu;

    // GTTMMADR, low 8MB is MMIO, high 8MB is global GTT
    size_t size;
    gpu.bar0 = syscall(SYS_pci_get_bar, display_pci, 0, &size);
    gpu.mmio = mmap(0, gpu.bar0, 0, size, PROT_READ | PROT_WRITE, 0);
    syscall(SYS_test, gpu.mmio);
    gpu.ggtt = (volatile uint64_t*) ((uint8_t*) gpu.mmio + 0x800000);

    // GMADR, 4GiB region for main memory
    gpu.bar2 = syscall(SYS_pci_get_bar, display_pci, 2, &size);
    gpu.main_mem = mmap(0, gpu.bar2, 0, size, PROT_READ | PROT_WRITE, 0);

    // GGC_0_0_0_PCI - GMCH Graphics Control
    gpu.gmch_ctrl = syscall(SYS_pci_read_config_32, display_pci, 0x50);

    uint64_t gtt_size = (gpu.gmch_ctrl >> 6u) & 3;
    gpu.ggtt_size = 1u << (20u + gtt_size);

    static const uint8_t cursor_raw[56*56*4] = {
        #embed "../desktop/cursor.bin"
    };

    uintptr_t cursor_paddr;
    gpu.cursor_data = (uint32_t*) syscall(SYS_mpin, 0, 0, 4*4096, &cursor_paddr);
    for (size_t j = 0; j < 56; j++) {
        for (size_t i = 0; i < 56; i++) {
            const uint8_t* pixel = &cursor_raw[(j*56 + i) * 4];
            gpu.cursor_data[j*64 + i] = (pixel[3] << 24u) | (pixel[0] << 16u) | (pixel[1] << 8u) | (pixel[2] << 0u);
        }
    }

    for (size_t i = 0; i < 4096; i += 64/4) {
        _mm_clflush(&gpu.cursor_data[i]);
    }

    gpu.cursor_paddr = cursor_paddr;

    uintptr_t scratch_paddr;
    syscall(SYS_mpin, 0, 0, 4096, &scratch_paddr);

    // Initialize GTT
    {
        size_t gtt_start = ((1920*1080*4) + 4095) / 4096;
        size_t gtt_end   = gpu.ggtt_size / sizeof(uint64_t);

        // Carve out 16KiB for the cursor
        gpu.cursor_gpu_addr = gtt_start * 4096;
        for (size_t i = 0; i < 4; i++) {
            gpu.ggtt[gtt_start++] = cursor_paddr + i*4096 + 1;
        }

        // leave the UEFI framebuffer at the base of the stolen memory untouched
        for (size_t i = gtt_start; i < gtt_end; i++) {
            gpu.ggtt[i] = scratch_paddr | 1;
        }

        volatile uint32_t head = *(volatile uint32_t*) &gpu.ggtt[gtt_end - 1];
    }

    allocate_buffers();
}

static void intel_gpu_cursor_set(bool on) {
    if (cursor_state != on) {
        if (on) {
            igpu_mmio_write(0x70088, 0x00100010); // CUR_POS
            igpu_mmio_write(0x70080, 0b100111);   // CUR_CONTROL
        } else {
            igpu_mmio_write(0x70080, 0); // CUR_CONTROL
        }
        igpu_mmio_write(0x70084, gpu.cursor_gpu_addr); // CUR_BASE_A
    }
    cursor_state = on;
}

static void intel_gpu_driver_poll(void) {
    #if 0
    printf("GGTT Size: %zu (%zu)\n", gpu.ggtt_size, gpu.gmch_ctrl);
    printf("Cursor PAddr: %p\n", gpu.cursor_paddr);
    printf("Mult: %d\n", mult);
    printf("VGA: %#x\n", igpu_mmio_read(0x41000));

    printf("    PS_CTRL_1_A %#x\n", igpu_mmio_read(0x68180));
    printf("    PS_CTRL_2_A %#x\n", igpu_mmio_read(0x68280));
    printf("    PS_CTRL_3_A %#x\n", igpu_mmio_read(0x68380));

    for (int pipe = 0; pipe < 3; pipe++) {
        printf("Pipe %d\n", pipe);
        printf("    WM_LINETIME %#x\n", igpu_mmio_read(0x45270 + pipe*4));

        if (1) {
            uint32_t addr = 0x70000 + (pipe * 0x1000);

            printf("  Cursor (addr=%#x)\n", addr);
            printf("    BUF_CFG %#x\n", igpu_mmio_read(addr + 0x17C));
            printf("    CTL     %#x\n", igpu_mmio_read(addr + 0x80));
            printf("    BASE    %#x\n", igpu_mmio_read(addr + 0x84));
            printf("    POS     %#x\n", igpu_mmio_read(addr + 0x88));
            printf("    FBC_CTL %#x\n", igpu_mmio_read(addr + 0xA0));
            printf("    CUR_PAL %#x\n", igpu_mmio_read(addr + 0x90));
            for (int i = 0; i < 8; i++) {
                printf("    WM%d     %#x\n", i, igpu_mmio_read(addr + 0x140 + i*4));
            }
        }

        for (int plane = 0; plane < 3; plane++) {
            uint32_t addr = 0x70100 + (pipe * 0x1000) + (plane * 0x100);

            printf("  Plane %d (addr=%#x)\n", plane, addr);
            if ((pipe == 0 && plane == 2) || (igpu_mmio_read(addr+0x80) >> 31ull)) { // is the plane enabled?
                printf("    BUF_CFG %#x\n", igpu_mmio_read(addr + 0x17C));
                printf("    CTL     %#x\n", igpu_mmio_read(addr + 0x80));
                printf("    SURF    %#x\n", igpu_mmio_read(addr + 0x9C));
                printf("    OFFSET  %#x\n", igpu_mmio_read(addr + 0xA4));
                printf("    L_SURF  %#x\n", igpu_mmio_read(addr + 0xB0));
                printf("    POS     %#x\n", igpu_mmio_read(addr + 0x8C));
                printf("    SIZE    %#x\n", igpu_mmio_read(addr + 0x90));
                printf("    STRIDE  %#x\n", igpu_mmio_read(addr + 0x88));
                for (int i = 0; i < 8; i++) {
                    printf("    WM%d     %#x\n", i, igpu_mmio_read(addr + 0x140 + i*4));
                }
            } else {
                printf("    NOT ENABLED\n");
            }
        }
    }
    #endif

    igpu_mmio_write(0x70088, (cursor_x << 16u) | cursor_y); // CUR_POS
    igpu_mmio_write(0x70084, gpu.cursor_gpu_addr); // CUR_BASE_A

    #if 0
    size_t gtt_start = ((1920*1080*4) + 4095) / 4096;
    size_t gtt_end   = gpu.ggtt_size / 4096;
    printf("GGTT [%zu - %zu]\n", gtt_start, gtt_end);
    printf("Cursor @ %p\n", gpu.cursor_gpu_addr);

    for (int i = gtt_start - 4; i < gtt_start + 4; i++) {
        printf("GGTT %d %#lx\n", i, gpu.ggtt[i]);
    }
    #endif
}

int _start(KHandle display_pci) {
    tsc_freq = syscall(SYS_tsc_freq);
    intel_gpu_driver_init(display_pci);

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

    for (;;) {
        uint64_t start = __rdtsc();

        size_t len;
        char* packet;
        for (;;) {
            packet = ipc_try_read(&int_in, &len);
            if (packet == NULL) {
                break;
            }
            cursor_x += packet[1];
            cursor_y += packet[2];
            ipc_read_release(&int_in);
        }
        intel_gpu_driver_poll();

        uint64_t elapsed = (__rdtsc() - start) / tsc_freq;
        if (elapsed < 16666) {
            syscall(SYS_sleep, 16666 - elapsed);
        }
    }

    return 0;
}
