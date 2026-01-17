#include <beans.h>
#include <common.h>
#include "../src/kernel/printf.c"

#define USBSTS_CNR (1<<11)

static volatile uint32_t* mmio;

static KHandle log_stream;
static char* log_buffer;
static int log_used;

void* memset(void* buffer, int c, size_t n) {
    u8* buf = (u8*)buffer;
    for (size_t i = 0; i < n; i++) {
        buf[i] = c;
    }
    return buffer;
}

void fault_handler(void) {
    if (log_stream) {
        syscall(SYS_debug_log, log_stream, log_used);
        log_used = 0;
    }
}

void _putchar(char ch) {
    if (log_stream == 0) {
        log_stream = syscall(SYS_vmo_create, 0, 4*1024);
        log_buffer = mmap(0, log_stream, 0, 4*1024, PROT_READ | PROT_WRITE, 0);
    } else if (log_used == 4096) {
        syscall(SYS_debug_log, log_stream, log_used);
        log_used = 0;
    }

    log_buffer[log_used++] = ch;
}

static void* pin(size_t size, uintptr_t* paddr) {
    return (void*) syscall(SYS_mpin, 0, 0, size, paddr);
}

int _start(KHandle pci_device) {
    /* uint64_t info[4];
    int fb_bar = syscall(SYS_fb_grab, info);
    uint32_t* fb = (uint32_t*) syscall(SYS_mmap, fb_bar, 0, info[3]); */

    size_t size;
    KHandle bar0 = syscall(SYS_pci_get_bar, pci_device, 0, &size);
    mmio = mmap(0, bar0, 0, size, PROT_READ | PROT_WRITE, 0);

    uintptr_t dcbaap_paddr;
    uintptr_t* dcbaap = pin(4096, &dcbaap_paddr);

    uintptr_t crcr_paddr;
    uintptr_t* crcr = pin(4096, &crcr_paddr);

    uintptr_t ers0_paddr;
    uint32_t* ers0 = pin(4096, &ers0_paddr);

    uintptr_t erst_paddr;
    uint32_t* erst = pin(4096, &erst_paddr);

    // ERS0 has 4K worth of TRB entries
    erst[0] = ers0_paddr & 0xFFFFFFFF;
    erst[1] = ers0_paddr >> 32ull;
    erst[2] = 16;
    erst[3] = 0;

    volatile uint32_t* op_base = &mmio[(mmio[0] & 0xFF) >> 2];
    volatile uint32_t* rt_regs = &mmio[mmio[6] >> 2];
    uint32_t max_ports = mmio[1] >> 24u;

    // XHCI initialization
    //   1. Wait for hardware reset (USBSTS @ 4h)
    op_base[0] |= 2;
    syscall(SYS_sleep, 100000);
    while (op_base[1] & USBSTS_CNR) {}
    //   2. Program max device slots
    op_base[14] = 16; // (CONFIG @ 38h)
    //   3. Program DCBAAP
    op_base[13] = (dcbaap_paddr >> 32ull);
    op_base[12] = dcbaap_paddr & 0xFFFFFFFF; // (DCBAAP @ 30h)
    //   4. Defgine the Command ring dequeue pointer
    op_base[7] = (crcr_paddr >> 32ull);
    op_base[6] = crcr_paddr & 0xFFFFFFFF; // (CRCR @ 18h)
    //   5. Runtime Register space
    volatile uint32_t* interrupt = &rt_regs[8];
    {
        // set table size
        interrupt[2] = 1;
        // set ERDP
        interrupt[7] = (ers0_paddr >> 32ull);
        interrupt[6] = ers0_paddr & 0xFFFFFFFF;
        // set ERSTBA
        interrupt[4] = erst_paddr & 0xFFFFFFFF;
        interrupt[5] = (erst_paddr >> 32ull);
    }
    //   5. Enable event ring
    //   5. Enable interrupts (TODO)
    //   6. Turn host controller on
    op_base[0] |= 1; // (USBCMD @ 00h)

    printf("[usb] Detected %u root hub ports\n", max_ports);
    FOR_N(i, 0, max_ports) {
        volatile uint32_t* sts = &op_base[0x100 + (4 * i)];
        if (*sts & 1) {
            printf("[usb] Resetting root hub Port%zu...\n", i);
            *sts |= 1 << 4;
        }
    }

    bool ccs = true;
    for (;;) {
        uintptr_t erdp_phys = (interrupt[6] & ~15) | ((uintptr_t) interrupt[7] << 32ull);
        uintptr_t trb_i = (erdp_phys - ers0_paddr) / sizeof(uint32_t);

        bool advanced = false;
        for (int i = 0; i < 4; i++) {
            volatile uint32_t* trb = &ers0[trb_i];
            if ((trb[3] & 1) != ccs) {
                // not a new entry, we don't move ahead
                break;
            }

            uint32_t type = (trb[3] >> 10) & 0b111111;
            if (type == 0x22) { // Port status change
                uint32_t port = (trb[0] >> 24) - 1;
                volatile uint32_t* sts = &op_base[0x100 + (4 * port)];

                printf("[usb] Port%u connected, sts=%#x\n", port, *sts);
            }
            advanced = true;

            trb_i += 4;
            if (trb_i == 4096/4) {
                // wrap around
                trb_i = 0;
                ccs = !ccs;
            }
        }

        if (advanced) {
            // update ERDP
            erdp_phys = ers0_paddr + trb_i*sizeof(uint32_t);
            interrupt[7] = (erdp_phys >> 32ull);
            interrupt[6] = erdp_phys & 0xFFFFFFFF;
            fault_handler();
        }

        syscall(SYS_sleep, 1000000);
    }

    return 0;
}
