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

enum {
    MAX_PORTS = 16,
};

enum {
    USB_SPEED_UNKNOWN,
    USB_SPEED_LOW,
    USB_SPEED_FULL,       // USB 1.1
    USB_SPEED_HIGH,       // USB 2.0
    USB_SPEED_WIRELESS,   // USB 2.5... unsupported by us
    USB_SPEED_SUPER,      // USB 3.0
    USB_SPEED_SUPER_PLUS, // USB 3.1 (TODO)
};

typedef struct {
    uint32_t data[8];
} EndpointContext;

typedef struct {
    // EP Context 0
    // EP Context 1  OUT (dir=0)
    // EP Context 1  IN  (dir=1)
    // ...
    // EP Context 15 OUT (dir=0)
    // EP Context 15 IN  (dir=1)
    EndpointContext arr[33];
} InputContext;

typedef struct {
    bool cycle_bit;

    uint32_t count;
    uint32_t* base;
    uintptr_t base_paddr;

    // Dequeue pointer
    uint32_t* dequeue;
    uintptr_t dequeue_paddr;
} HCI_Ring;

#define assert(cond) if (!(cond)) { assert_msg(#cond); }
static void assert_msg(const char* str) {
    printf("assert condition failed! %s\n", str);
    fault_handler();
}

enum { PAGE_SIZE_DWORDS = 4096 / 4 };
static void ring_alloc(HCI_Ring* ring, size_t cnt) {
    size_t size = cnt*16;

    ring->cycle_bit = true;
    ring->count = cnt;

    ring->base = mmap(0, 0, 0, size, PROT_READ | PROT_WRITE, 0);
    ring->base_paddr = syscall(SYS_get_paddr, ring->base);
    assert(ring->base_paddr);

    ring->dequeue = ring->base;
    ring->dequeue_paddr = ring->base_paddr;

    if (size > 4096) {
        size_t curr = PAGE_SIZE_DWORDS;
        uintptr_t prev_paddr = ring->base_paddr;
        uint32_t* base =  ring->base;

        // Insert Link TRBs
        size_t page_count = (size + 4095) / 4096;
        FOR_N(i, 1, page_count) {
            uintptr_t paddr = syscall(SYS_get_paddr, &base[i*PAGE_SIZE_DWORDS]);
            if (prev_paddr+4096 != paddr) {
                // Not contiguous? insert link at the end of the prev_paddr
                uint32_t* link_entry = &base[i*PAGE_SIZE_DWORDS - 4];
                link_entry[0] = paddr & 0xFFFFFFF0;
                link_entry[1] = paddr >> 32ull;
                link_entry[3] = 6u << 10u;
            }
            prev_paddr = paddr;
        }

        // Final Link TRB
        uint32_t* link_entry = &base[(size - 16) / 4];
        link_entry[0] = 0;
        link_entry[1] = 0;
        link_entry[3] = (6u << 10u) | 2u; // Toggle cycle
    }
}

static uint32_t* ring_cmd_at(HCI_Ring* ring, uintptr_t paddr) {
    return &ring->base[(paddr - ring->base_paddr) / 4];
}

static void ring_advance(HCI_Ring* ring) {
    // Move 16B forward
    ring->dequeue += 4;
    ring->dequeue_paddr += 16;

    // Check for Link TRBs
    if (ring->dequeue == &ring->base[ring->count*4]) {
        // Wrap to start
        ring->dequeue       = ring->base;
        ring->dequeue_paddr = ring->base_paddr;
        ring->cycle_bit     = !ring->cycle_bit;
    } else {
        uint32_t type = (ring->dequeue[3] >> 10u) & 0b111111;
        if (type == 6) {
            ring->dequeue_paddr = ring->dequeue[0] | ((uintptr_t) ring->dequeue[1] << 32ull);
        }
    }
}

static void ring_submit_cmd(HCI_Ring* ring, int type, uint32_t cmd[4]) {
    uint32_t* dst = ring->dequeue;

    // copy command (except control field)
    FOR_N(i, 0, 3) { dst[i] = cmd[i]; }

    // write out, last word in the command must be written last since
    // it holds the control field and the producer cycle bit.
    dst[3] = cmd[3] | ((type & 0x3F) << 10u) | ring->cycle_bit;

    ring_advance(ring);
}

typedef struct {
    enum {
        PORT_DISCONNECTED,
        PORT_POLLING, // only USB2 does this

        // the USB speed has been negociated but no address is setup.
        PORT_DEFAULT,

        // we've submitted the address device command but haven't received an
        // answer yet.
        PORT_WAIT_FOR_ADDRESS,

        // there's an address but no config.
        PORT_ADDRESS,

        // there's at least one config!
        PORT_CONFIGURED,
    } state;
    bool usb3;
    int slot;

    uintptr_t init_cmd;

    // Device rings
    HCI_Ring xfer_ring;
} Port;

static Port ports[MAX_PORTS];
static volatile uint32_t* doorbell;
static volatile uint32_t* op_base;
static volatile uintptr_t* dcbaap;

static HCI_Ring crcr;

// Doorbell reg 0 is the host controller
static void ring_doorbell(int slot, int target) {
    doorbell[slot] = target;
}

// https://www.beyondlogic.org/usbnutshell/usb6.shtml
// Request type bitmap
enum {
    // recipient
    USB_RECIP_DEVICE    = 0,
    USB_RECIP_INTERFACE = 1,
    USB_RECIP_ENDPOINT  = 2,
    USB_RECIP_OTHER     = 3,
    // type
    USB_TYPE_STANDARD   = 0 << 5u,
    USB_TYPE_CLASS      = 1 << 5u,
    USB_TYPE_VENDOR     = 2 << 5u,
    USB_TYPE_RESERVED   = 3 << 5u,
    // direction
    USB_HOST2DEV        = 0 << 7, // out
    USB_DEV2HOST        = 1 << 7, // in
};

// Request
enum {
    USB_GET_STATUS        = 0,
    USB_CLEAR_FEATURE     = 1,
    USB_SET_FEATURE       = 3,
    USB_SET_ADDRESS       = 5,
    USB_GET_DESCRIPTOR    = 6,
    USB_SET_DESCRIPTOR    = 7,
    USB_GET_CONFIGURATION = 8,
    USB_SET_CONFIGURATION = 9,
};

typedef struct {
    // Setup stage
    uint8_t request_type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;

    // Data stage
    size_t data_len;
    void* data;

    // Status stage
} URB;

void* memcpy(void* dest, const void* src, size_t n) {
    u8* d = (u8*)dest;
    u8* s = (u8*)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dest;
}

static alignas(64) char usb_desc[64];
static void submit_urb(int port, int slot, URB* urb) {
    uint32_t cmd[4];
    int trt = urb->data_len ? (urb->request_type & USB_DEV2HOST ? 3 : 2) : 0;

    // Setup Stage Packet
    memcpy(cmd, urb, sizeof(uint32_t[2]));
    cmd[2] = 8u; // TRB Transfer length is fixed at 8
    cmd[3] = (trt << 16u) | (1u << 6u);
    ring_submit_cmd(&ports[port].xfer_ring, 2, cmd);

    // Data Stage Packet (optional)
    bool status_in = urb->request_type & USB_DEV2HOST;
    if (urb->data_len) {
        uintptr_t data_paddr = syscall(SYS_get_paddr, urb->data);

        cmd[0] = data_paddr & 0xFFFFFFFF;
        cmd[1] = data_paddr >> 32ull;
        cmd[2] = urb->data_len;
        cmd[3] = status_in << 16u;
        ring_submit_cmd(&ports[port].xfer_ring, 3, cmd);
    }

    // Status Stage Packet
    cmd[0] = 0;
    cmd[1] = 0;
    cmd[2] = 0;
    cmd[3] = status_in << 16u;
    ring_submit_cmd(&ports[port].xfer_ring, 4, cmd);

    ring_doorbell(1+slot, 1);
}

enum {
    MSG_PORT_CHANGE,
    MSG_SLOT_ENABLE,
    MSG_ADDRESSED,
};

static void usb_fsm(int msg, int port, int slot) {
    printf("USB_FSM(%d, %d, %d)\n", msg, port, slot);
    fault_handler();

    volatile uint32_t* sts_ptr = &op_base[0x100 + (4 * port)];
    switch (msg) {
        case MSG_PORT_CHANGE: {
            uint32_t sts = *sts_ptr;
            if (sts & 1) { // Connect
                // USB3 will immediately set the PED flag, USB2 requires a reset before doing so
                if ((sts & 2) == 0) {
                    printf("[usb] Port%u connecting via USB2, sts=%#x (reset enqueued)\n", port, sts);

                    ports[port].state = PORT_POLLING;
                    *sts_ptr |= 1 << 4;
                    return;
                }

                ports[port].slot = -1;
                ports[port].usb3 = ports[port].state == PORT_DISCONNECTED;
                ports[port].state = PORT_DEFAULT;
                printf("[usb] Port%u connecting via USB%d, sts=%#x, speed=%d\n", port, 2 + ports[port].usb3, sts, (sts >> 10) & 0xF);

                // EnableSlot Command
                ports[port].init_cmd = crcr.dequeue_paddr;
                uint32_t cmd[4] = { 0, 0, 0, (1 + slot) << 24u };
                ring_submit_cmd(&crcr, 9, cmd);
                ring_doorbell(0, 0);
            } else { // Disconnect
                printf("[usb] Port%u disconnecting, sts=%#x\n", port, sts);
                ports[port].state = PORT_DISCONNECTED;
            }
        } break;

        case MSG_SLOT_ENABLE: {
            printf("[usb] Port%u is associated with Slot%u\n", port, slot);

            uintptr_t in_ctx_paddr;
            InputContext* in_ctx = pin(4096, &in_ctx_paddr);

            uintptr_t out_ctx_paddr;
            uint32_t* out_ctx = pin(4096, &out_ctx_paddr);

            HCI_Ring* xfer_ring = &ports[port].xfer_ring;
            ring_alloc(xfer_ring, 256);

            uint32_t speed = (*sts_ptr >> 10u) & 0xF;
            uint32_t max_packet = 8;
            switch (speed) {
                case USB_SPEED_SUPER:
                max_packet = 512;
                break;

                case USB_SPEED_FULL: case USB_SPEED_HIGH:
                max_packet = 64;
                break;
            }

            // control context, enabling A0 and A1
            in_ctx->arr[0].data[1] = 3;

            // slot context
            in_ctx->arr[1].data[0] = (1 << 27u) | (speed << 20u);
            in_ctx->arr[1].data[1] = ((port + 1) << 16u);

            // endpoint 0
            //                        CErr        EP Type     Max Packet Size
            in_ctx->arr[2].data[1] = (3 << 1u) | (4 << 3u) | (max_packet << 16u);
            in_ctx->arr[2].data[2] = (xfer_ring->base_paddr & 0xFFFFFFF0) | 1u;
            in_ctx->arr[2].data[3] = (xfer_ring->base_paddr >> 32ull);

            dcbaap[slot] = out_ctx_paddr;
            printf("[usb] Initialized Port%u with In:%p, Out:%p Transfer:%p (%zu max packet)\n", port, in_ctx_paddr, out_ctx_paddr, xfer_ring->base_paddr, max_packet);

            // submit AddressDevice command
            ports[port].state = PORT_WAIT_FOR_ADDRESS;
            ports[port].init_cmd = crcr.dequeue_paddr;
            uint32_t cmd[4] = { in_ctx_paddr & 0xFFFFFFF0, in_ctx_paddr >> 32ull, (1 + slot) << 24u };
            ring_submit_cmd(&crcr, 11, cmd);
            ring_doorbell(0, 0);
        } break;
        case MSG_ADDRESSED: {
            printf("Port%u is now addressed!\n", port);
            ports[port].state = PORT_ADDRESS;

            URB urb = {
                .request_type = USB_DEV2HOST | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                .request      = USB_GET_DESCRIPTOR,
                .value        = 0,
                .length       = 8,

                .data_len     = 64,
                .data         = usb_desc,
            };
            submit_urb(port, slot, &urb);
        } break;
    }
}

int _start(KHandle pci_device) {
    /* uint64_t info[4];
    int fb_bar = syscall(SYS_fb_grab, info);
    uint32_t* fb = (uint32_t*) syscall(SYS_mmap, fb_bar, 0, info[3]); */

    size_t size;
    KHandle bar0 = syscall(SYS_pci_get_bar, pci_device, 0, &size);
    mmio = mmap(0, bar0, 0, size, PROT_READ | PROT_WRITE, 0);

    uintptr_t dcbaap_paddr;
    dcbaap = pin(4096, &dcbaap_paddr);

    ring_alloc(&crcr, 256);

    uintptr_t ers0_paddr;
    uint32_t* ers0 = pin(4096, &ers0_paddr);

    uintptr_t erst_paddr;
    uint32_t* erst = pin(4096, &erst_paddr);

    // ERS0 has 4K worth of TRB entries
    erst[0] = ers0_paddr & 0xFFFFFFFF;
    erst[1] = ers0_paddr >> 32ull;
    erst[2] = 1024 / 16;
    erst[3] = 0;

    op_base  = &mmio[(mmio[0] & 0xFF) >> 2];
    volatile uint32_t* rt_regs = &mmio[mmio[6] >> 2];
    doorbell = &mmio[mmio[5] >> 2];

    int slot_size = (mmio[4] >> 2) & 1 ? 64 : 32;
    printf("[usb] Slot size: %zu\n", slot_size);

    uint32_t max_ports = mmio[1] >> 24u;

    // XHCI initialization
    //   1. Wait for hardware reset (USBSTS @ 4h)
    op_base[0] |= 2;
    syscall(SYS_sleep, 100000);
    while (op_base[1] & USBSTS_CNR) {}
    //   2. Program max device slots
    op_base[14] = MAX_PORTS; // (CONFIG @ 38h)
    //   3. Program DCBAAP
    op_base[13] = (dcbaap_paddr >> 32ull);
    op_base[12] = dcbaap_paddr & 0xFFFFFFFF; // (DCBAAP @ 30h)
    //   4. Defgine the Command ring dequeue pointer
    op_base[6] = (crcr.base_paddr & 0xFFFFFFFF) | (1<<3) | crcr.cycle_bit; // (CRCR @ 18h)
    op_base[7] = (crcr.base_paddr >> 32ull);
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

    printf("[usb] Ready to receive events!\n");
    fault_handler();

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
            if (type == 0x21) { // Command completion
                uintptr_t cmd_ptr = trb[0] | ((uintptr_t) trb[1] << 32ull);
                volatile uint32_t* cmd = ring_cmd_at(&crcr, cmd_ptr);

                uint32_t completed_type = (cmd[3] >> 10) & 0b111111;
                printf("%d : Completed Event%p %#x (type=%d)\n", trb[2] >> 24u, cmd_ptr, trb[3], completed_type);

                // We have a slot ID now
                size_t i = 0;
                for (; i < max_ports; i++) {
                    if (ports[i].init_cmd == cmd_ptr) {
                        ports[i].init_cmd = 0;
                        break;
                    }
                }

                if (i < max_ports) {
                    if (ports[i].state == PORT_DEFAULT && completed_type == 9) {
                        ports[i].slot = (trb[3] >> 24u) - 1;
                        usb_fsm(MSG_SLOT_ENABLE, i, ports[i].slot);
                    } else if (ports[i].state == PORT_WAIT_FOR_ADDRESS && completed_type == 11) {
                        usb_fsm(MSG_ADDRESSED, i, ports[i].slot);
                    }
                }
            } else if (type == 0x22) { // Port status change
                int port = (trb[0] >> 24) - 1;
                usb_fsm(MSG_PORT_CHANGE, port, ports[i].slot);
            } else {
                printf("Unsupported event %d\n", type);
            }
            advanced = true;
            fault_handler();

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
        }

        printf("Ping\n");
        fault_handler();

        syscall(SYS_sleep, 1000000);
    }

    return 0;
}
