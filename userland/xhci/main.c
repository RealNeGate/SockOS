#include <beans.h>
#include <common.h>
#include <stdatomic.h>
#include <emmintrin.h>
#include "../kernel/printf.c"

#define IPC_RING_IMPL
#include "../ipc_ring.h"

#include "usb.h"

static volatile uint32_t* mmio;
static int context_slot_size;

static KHandle log_stream;
static char* log_buffer;
static int log_used;
static atomic_int log_lock;
static KHandle root_mailbox;

#define SPIN_LOCK(x)   while (!atomic_compare_exchange_strong((x), &(int){ 0 }, 1)) {}
#define SPIN_UNLOCK(x) atomic_store((x), 0)

void* memset(void* buffer, int c, size_t n) {
    u8* buf = (u8*)buffer;
    for (size_t i = 0; i < n; i++) {
        buf[i] = c;
    }
    return buffer;
}

void fault_handler(void) {
    SPIN_LOCK(&log_lock);
    if (log_stream && log_used) {
        syscall(SYS_debug_log, log_stream, log_used);
        log_used = 0;
    }
    SPIN_UNLOCK(&log_lock);
}

#define assert(cond) if (!(cond)) { assert_msg(#cond); }
static void assert_msg(const char* str) {
    printf("assert condition failed! %s\n", str);
    fault_handler();
    *((volatile int*) 0) = 0;
}

void _putchar(char ch) {
    SPIN_LOCK(&log_lock);
    if (log_stream == 0) {
        log_stream = syscall(SYS_vmo_create, 4*1024);
        log_buffer = mem_map(NULL_HANDLE, 0, log_stream, 0, 4*1024, PROT_RW, 0);
    } else if (log_used == 4096) {
        syscall(SYS_debug_log, log_stream, log_used);
        log_used = 0;
    }

    log_buffer[log_used++] = ch;
    SPIN_UNLOCK(&log_lock);
}

static void* alloc_pinned_page(uintptr_t* paddr) {
    void* base = mem_map_private(NULL_HANDLE, 4096, PROT_RW, 0);
    *paddr = mem_translate(NULL_HANDLE, base);
    return base;
}

// Source files
#include "ring.c"

enum {
    MAX_PORTS = 16,
    MAX_SLOTS = 256,
};

typedef struct {
    int usb_type;
    int slot;
} Port;

#define DEV_SLOT(dev) ((dev) - devices)

static Port ports[MAX_PORTS];
static USB_Device* devices;

static volatile uint32_t* doorbell;
static volatile uint32_t* op_base;
static volatile uintptr_t* dcbaap;

static HCI_Ring crcr;

static uint32_t mmio_read32(volatile uint32_t* src) {
    asm volatile("mfence" : : : "memory");
    uint32_t x = *src;
    asm volatile("mfence" : : : "memory");
    return x;
}

// the dummy read is to force the PCI writes to actually flush
static void ring_doorbell(USB_Device* dev, int target) {
    asm volatile("mfence" : : : "memory");

    if (dev == NULL) {
        // Doorbell reg 0 is the host controller
        doorbell[0] = target;
        uint32_t x = mmio_read32(&doorbell[0]);
    } else {
        doorbell[1 + (dev - devices)] = target;
        mmio_read32(&doorbell[1 + (dev - devices)]);
    }
}

typedef struct PendingURB PendingURB;
struct PendingURB {
    // Key
    uintptr_t paddr;

    // Value
    USB_Device* dev;
    int pipe;
};

typedef struct {
    enum USB_DeviceState state;
    int port;

    // Physical address of the EnableSlot command, used by
    // init to know when we're initialized.
    uintptr_t init_cmd;
} PendingConnection;

static int pending_con_count;
static PendingConnection pending_con[256];

static int pending_urb_count;
static PendingURB pending_urbs[512];
static atomic_int pending_urb_lock;

static atomic_int dev_to_refresh;

static void mark_pending_urb(uintptr_t paddr, USB_Device* dev, int pipe) {
    SPIN_LOCK(&pending_urb_lock);
    pending_urbs[pending_urb_count++] = (PendingURB){
        paddr, dev, pipe
    };
    SPIN_UNLOCK(&pending_urb_lock);
}

static void signal_pending_urb(uintptr_t paddr, int cc, int trb_len) {
    SPIN_LOCK(&pending_urb_lock);
    PendingURB pending = { 0};
    for (int i = 0; i < pending_urb_count; i++) {
        if (pending_urbs[i].paddr == paddr) {
            // Remove from list
            pending = pending_urbs[i];
            pending_urbs[i] = pending_urbs[--pending_urb_count];
            break;
        }
    }
    SPIN_UNLOCK(&pending_urb_lock);

    if (pending.dev) {
        event_signal(pending.dev->ipc_ring_evt[pending.pipe]);
    }
}

void* memcpy(void* dest, const void* src, size_t n) {
    u8* d = (u8*)dest;
    u8* s = (u8*)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dest;
}

enum {
    TRB_NORMAL = 1,
    TRB_SETUP  = 2,
    TRB_DATA   = 3,
    TRB_STATUS = 4,
};

static uintptr_t submit_urb(USB_RequestBlock* urb) {
    USB_Device* dev = urb->dev;
    uint32_t max_packet_size = dev->in_ctx->arr[urb->pipe + 2].data[1] >> 16u;

    uint32_t cmd[4];
    int trt = urb->data_len ? (urb->setup.request_type & USB_DEV2HOST ? 3 : 2) : 0;
    HCI_Ring* ring = &dev->xfer_ring[urb->pipe];

    uint32_t* start = ring->dequeue;

    // Setup Stage Packet
    memcpy(cmd, &urb->setup, sizeof(struct URB_Setup));
    cmd[2] = 8u; // TRB Transfer length is fixed at 8
    cmd[3] = (trt << 16u) | (1u << 6u);
    ring_submit_cmd2(ring, TRB_SETUP, cmd);

    // Data Stage Packet (optional)
    bool status_in = urb->setup.request_type & USB_DEV2HOST;
    uintptr_t data_paddr = 0;
    if (urb->data_len) {
        uintptr_t paddr = mem_translate(NULL_HANDLE, urb->data);
        assert(paddr/4096ull == (paddr+urb->data_len)/4096ull);

        cmd[0] = paddr & 0xFFFFFFFF;
        cmd[1] = paddr >> 32ull;
        cmd[2] = urb->data_len;
        cmd[3] = status_in << 16u;
        ring_submit_cmd2(ring, TRB_DATA, cmd);

        // printf("DATA: %s\n", status_in ? "IN" : "OUT");
        status_in = !status_in;
    } else {
        status_in = true;
    }
    // printf("STATUS: %s\n", status_in ? "IN" : "OUT");

    // Status Stage Packet (IOC=true)
    cmd[0] = 0;
    cmd[1] = 0;
    cmd[2] = 0;
    cmd[3] = (1u << 5u) | (status_in << 16u);
    uintptr_t paddr = ring->dequeue_paddr;
    mark_pending_urb(paddr, dev, urb->pipe);
    ring_submit_cmd2(ring, TRB_STATUS, cmd);

    // toggle all cycle bits together
    while (start != ring->dequeue) {
        start[3] ^= 1, start += 4;
        asm volatile("mfence" : : : "memory");
    }

    return paddr;
}

enum {
    // init messages
    MSG_PORT_CHANGE,
    MSG_SLOT_ENABLE,
    MSG_ADDRESSED,
};

static void usb_submit_urb_sync(USB_RequestBlock* urb) {
    USB_Device* dev = urb->dev;
    uintptr_t paddr = submit_urb(urb);
    ring_doorbell(dev, urb->pipe + 1);

    // Wait for URB
    event_wait(dev->ipc_ring_evt[0]);
}

static void usb_device_thread(void* arg);
static void usb_fsm(int msg, int port, int slot) {
    USB_Device* dev = slot >= 0 ? &devices[slot] : NULL;

    // printf("USB_FSM(%d, %d, %d, %p)\n", msg, port, slot, dev);
    // fault_handler();

    volatile uint32_t* sts_ptr = &op_base[0x100 + (4 * port)];
    switch (msg) {
        case MSG_PORT_CHANGE: {
            uint32_t sts = *sts_ptr;
            if (sts & 1) { // Connect
                // USB3 will immediately set the PED flag, USB2 requires a reset before doing so
                if ((sts & 2) == 0) {
                    printf("[usb]  Port%u connecting via USB2, sts=%#x (reset enqueued)\n", port, sts);
                    assert(ports[port].usb_type == 0);
                    ports[port].usb_type = 2;
                    *sts_ptr |= 1 << 4;
                    return;
                }

                // If we reach here without needing to set the reset, we're a USB3 port
                if (ports[port].usb_type == 0) {
                    ports[port].usb_type = 3;
                }

                printf("[usb]  Port%u connecting via USB%d, sts=%#x, speed=%d\n", port, ports[port].usb_type, sts, (sts >> 10) & 0xF);
                pending_con[pending_con_count++] = (PendingConnection){
                    PORT_DEFAULT, port, crcr.dequeue_paddr
                };

                // EnableSlot Command
                uint32_t cmd[4] = { 0, 0, 0, (1 + slot) << 24u };
                ring_submit_cmd(&crcr, 9, cmd);
                ring_doorbell(NULL, 0);
            } else { // Disconnect
                printf("[usb]  Port%u disconnecting, sts=%#x\n", port, sts);

                // TODO(NeGate): disconnect all child devices correctly
                assert(dev);
                dev->state = PORT_DISCONNECTED;
            }
        } break;

        case MSG_SLOT_ENABLE: {
            printf("[usb]  Port%u is associated with Slot%u\n", port, slot);

            uintptr_t in_ctx_paddr;
            InputContext* in_ctx = alloc_pinned_page(&in_ctx_paddr);

            uintptr_t out_ctx_paddr;
            uint32_t* out_ctx = alloc_pinned_page(&out_ctx_paddr);

            HCI_Ring* xfer_ring = &dev->xfer_ring[0];
            ring_alloc(xfer_ring, 256);

            uint32_t speed = (*sts_ptr >> 10u) & 0xF;
            uint32_t max_packet = 8; // FULL could be 8,16,32,64
            switch (speed) {
                case USB_SPEED_SUPER:
                max_packet = 512;
                break;

                case USB_SPEED_HIGH:
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

            #if 0
            printf("PORTSC: %08x\n", *sts_ptr);
            for (int i = 0; i < 3; i++) {
                printf("[%d] %08x %08x %08x %08x\n",
                    i,
                    in_ctx->arr[i].data[0],
                    in_ctx->arr[i].data[1],
                    in_ctx->arr[i].data[2],
                    in_ctx->arr[i].data[3]
                );
            }
            #endif

            dcbaap[1 + slot] = out_ctx_paddr;
            asm volatile("mfence" : : : "memory");

            printf("[usb]  Initialized Port%u with In:%p, Out:%p Transfer:%p (%zu max packet)\n", port, in_ctx_paddr, out_ctx_paddr, xfer_ring->base_paddr, max_packet);

            dev->speed = speed;
            dev->in_ctx_paddr = in_ctx_paddr;
            dev->out_ctx_paddr = out_ctx_paddr;
            dev->in_ctx  = in_ctx;
            dev->out_ctx = out_ctx;

            // submit AddressDevice command
            pending_con[pending_con_count++] = (PendingConnection){
                PORT_WAIT_FOR_ADDRESS, port, crcr.dequeue_paddr
            };

            uint32_t cmd[4] = { in_ctx_paddr & 0xFFFFFFF0, in_ctx_paddr >> 32ull, 0, (1 + slot) << 24u };
            ring_submit_cmd(&crcr, 11, cmd);
            ring_doorbell(NULL, 0);
        } break;
        case MSG_ADDRESSED: {
            printf("Port%u is now addressed!\n", port);
            dev->state = PORT_ADDRESS;
            dev->mailbox = syscall(SYS_mailbox_create, 1);
            dev->ipc_ring_evt[0] = event_create();

            thread_create(NULL_HANDLE, usb_device_thread, (uintptr_t) dev, 8192, 0);
        } break;
    }
}

// see 4.8.2.4
static void usb_set_endpoint(USB_Device* dev, USB_EndpointDesc* ep, int interval) {
    bool dir_in = ep->addr >> 7;
    int ep_type = dir_in ? 7 : 3;
    int index   = ep->addr & 0xF;
    int i       = (index - 1)*2 + (dir_in ? 4 : 3);

    assert(i - 2 < MAX_ENDPOINTS);
    HCI_Ring* ring = &dev->xfer_ring[i - 2];
    ring_alloc(ring, 256);

    dev->ipc_ring[i - 2] = ipc_ring_alloc2(ep->max_packet_size, 4096, &dev->ipc_ring_vmo[i - 2]);
    dev->ipc_ring_evt[i - 2] = event_create();

    InputContext* in_ctx = dev->in_ctx;
    in_ctx->arr[0].data[1] |= 1 << (i - 1);

    // update context entries to contain the highest DCI
    int curr_context_entries = in_ctx->arr[1].data[0] >> 27u;
    if (i - 1 >= curr_context_entries) {
        uint32_t mask = 0b11111 << 27u;
        in_ctx->arr[1].data[0] = (in_ctx->arr[1].data[0] & ~mask) | (i << 27u);
    }

    //                        Interval
    in_ctx->arr[i].data[0] = (interval << 16);
    //                        CErr        EP Type           Max Packet Size                Max Burst
    in_ctx->arr[i].data[1] = (3 << 1u) | (ep_type << 3u) | (ep->max_packet_size << 16u) | (0 << 8u);
    in_ctx->arr[i].data[2] = (ring->base_paddr & 0xFFFFFFF0) | 1u;
    in_ctx->arr[i].data[3] = (ring->base_paddr >> 32ull);
}

static int read_string(USB_Device* dev, int index, char* dst, size_t cap) {
    // First read the header
    USB_RequestBlock urb = {
        .dev   = dev,
        .setup = {
            .request_type = USB_DEV2HOST | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
            .request      = USB_GET_DESCRIPTOR,
            .value        = (USB_DT_STRING << 8) | index,
            .length       = 2,
        },
        .data_len     = 2,
        .data         = dst,
    };
    usb_submit_urb_sync(&urb);

    int len = dst[0];
    urb.setup.length  = len;
    urb.data_len      = len;
    usb_submit_urb_sync(&urb);

    int cnt = (len - 2) / 2;
    if (cnt >= cap) {
        cnt = cap - 1;
    }

    for (int i = 0; i < cnt; i++) {
        dst[i] = dst[i*2 + 2];
    }
    dst[cnt] = 0;
    return cnt;
}

enum {
    USB_DRIVER_NONE,
    USB_DRIVER_HID,
};

static void usb_endpoint_thread(void* arg);
static void interrupt_in_try(USB_Device* dev, int pipe, char* data, size_t data_len);

static void usb_device_thread(void* d) {
    USB_Device* dev = d;
    mailbox_send(root_mailbox, 0, DEV_SLOT(dev), 0, NULL, &(KHandle){ dev->mailbox });

    USB_RequestBlock urb = {
        .dev   = dev,
        .setup = {
            .request_type = USB_DEV2HOST | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
            .request      = USB_GET_DESCRIPTOR,
            .value        = 0x0100, // DT_DEVICE
            .length       = 8,
        },
        .data_len     = 8,
        .data         = &dev->desc,
    };
    usb_submit_urb_sync(&urb);

    size_t desc_len = dev->desc.length;
    char* buf = (char*) &dev->desc;
    urb.setup.length = urb.data_len = desc_len;
    usb_submit_urb_sync(&urb);

    char str[256];
    char scratch[256];
    urb.data = scratch;

    char* interface = NULL;
    size_t interface_size = 0;
    int usb_type = USB_DRIVER_NONE;

    #if 0
    printf(" FOUND DEVICE!!! %02x:%02x, %d\n", dev->desc.dev_class, dev->desc.dev_sub_class, dev->desc.num_configs);
    for (int j = 0; j < desc_len; j++) {
        printf(" %02x", buf[j] & 0xFF);
    }
    printf("\n");
    fault_handler();
    #endif

    if (dev->desc.dev_class == 0 && dev->desc.dev_sub_class == 0) {
        int selected_config = 0;
        int selected_interface = 0;
        for (int i = 0; i < dev->desc.num_configs; i++) {
            urb.setup.request_type = USB_DEV2HOST | USB_TYPE_STANDARD | USB_RECIP_DEVICE;
            urb.setup.request = USB_GET_DESCRIPTOR;
            urb.setup.value   = (USB_DT_CONFIG << 8) | i;
            urb.setup.length  = 9;
            urb.data_len      = 9;
            usb_submit_urb_sync(&urb);

            read_string(dev, scratch[6], dev->name, 32);

            urb.setup.length = urb.data_len = scratch[2] | (scratch[3] << 8);
            usb_submit_urb_sync(&urb);

            // dump interfaces & endpoints
            size_t pos = 9;
            while (pos < urb.data_len) {
                // scan for interfaces, drivers latch onto those technically
                uint8_t len  = scratch[pos];
                uint8_t type = scratch[pos+1];

                // interface
                if (type == 4) {
                    USB_InterfaceDesc* idesc = (USB_InterfaceDesc*) &scratch[pos];
                    if (idesc->interface_class == 3 && idesc->interface_subclass == 1) {
                        usb_type = USB_DRIVER_HID;
                        selected_interface = idesc->interface_num;

                        // scan until either the end or the next interface
                        size_t pos2 = pos + len;
                        while (pos2 < urb.data_len && scratch[pos2+1] != 4) {
                            pos2 += scratch[pos2];
                        }
                        interface_size = pos2 - pos;
                        interface = &scratch[pos];
                        goto found;
                    }
                }
                pos += len;
            }
        }

        found:;
        urb.setup.request_type = USB_DEV2HOST | USB_TYPE_STANDARD | USB_RECIP_DEVICE;
        urb.setup.length  = 0;
        urb.data_len      = 0;

        // select config
        /* if (selected_config) {
            urb.setup.request = USB_SET_CONFIGURATION;
            urb.setup.value   = selected_config;
            usb_submit_urb_sync(&urb);
        }

        // select interface
        if (selected_config) {
            urb.setup.request = USB_SET_INTERFACE;
            urb.setup.value   = selected_interface;
        }
        usb_submit_urb_sync(&urb);*/
        assert(selected_config == 0 || selected_interface == 0);
    }

    // Initialize the interface's endpoints
    if (interface_size > 0) {
        int slot = DEV_SLOT(dev);
        printf("[usb]  dev%d: connected '%s' %04x:%04x\n", slot, dev->name, dev->desc.id_vendor, dev->desc.id_product);
        fault_handler();

        InputContext* in_ctx = dev->in_ctx;
        in_ctx->arr[0].data[1] = 0;
        in_ctx->arr[0].data[1] = 1;

        // TODO(NeGate): negotiate bandwidth
        for (int i = interface[0], j = 0; i < interface_size; i += interface[i], j++) {
            USB_EndpointDesc* ep = (USB_EndpointDesc*) &interface[i];
            bool dir_in = ep->addr >> 7;

            const char* type_str;
            switch (ep->attrs & 3) {
                case 0: type_str = "Ctrl"; break;
                case 1: type_str = "Iso";  break;
                case 2: type_str = "Bulk"; break;
                case 3: type_str = "Int";  break;
            }

            int ep_interval = 0;
            if ((ep->attrs & 3) == 3) {
                // frames are 1ms on low/full, 125us for high
                int interval_micros = ep->interval * 125;
                if (dev->speed == USB_SPEED_LOW || dev->speed == USB_SPEED_FULL) {
                    interval_micros *= 8;
                }

                // endpoint interval represent a period:
                //   period = 125us * 2^interval
                //
                // so interval=1 means 125us but 4 means 2ms.
                // we pick a valid interval that's at least as
                // big as the interval_micros.
                while (interval_micros > 125*(1<<ep_interval)) {
                    ep_interval++;
                }
            }

            printf("[usb]  dev%d: endpoint%d: num=%d, dir=%-3s, type=%s, rate=%d, max_packet=%d\n", slot, j, ep->addr & 0xF, dir_in ? "IN" : "OUT", type_str, ep->interval, ep->max_packet_size);
            usb_set_endpoint(dev, ep, ep_interval);
        }
        fault_handler();

        // Ask to refresh device
        int id = 1 + DEV_SLOT(dev);
        while (!atomic_compare_exchange_strong(&dev_to_refresh, &(int){ 0 }, id)) {
        }

        // Wait for it to complete
        while (dev_to_refresh == id) {
        }

        if (usb_type == USB_DRIVER_HID) {
            urb.setup.request_type = 0x21; // SetProtocol
            urb.setup.request      = 0x0B; // Boot
            urb.setup.value        = 0;
            usb_submit_urb_sync(&urb);
        }

        // Launch listener threads to track interrupt IN channels
        for (int i = interface[0], j = 0; i < interface_size; i += interface[i], j++) {
            USB_EndpointDesc* ep = (USB_EndpointDesc*) &interface[i];
            bool dir_in = ep->addr >> 7;

            // "Bootstrap" the interrupt process
            if ((ep->attrs & 3) == 3 && dir_in) {
                bool dir_in = ep->addr >> 7;
                int ep_type = dir_in ? 7 : 3;
                int index   = ep->addr & 0xF;
                int i       = (index - 1)*2 + (dir_in ? 4 : 3);

                uintptr_t val = (i - 2) | (DEV_SLOT(dev) << 32u);
                thread_create(NULL_HANDLE, usb_endpoint_thread, val, 8192, 0);
            }
        }
    }

    KHandle mailbox = dev->mailbox;

    // Process messages
    KHandle handle;
    uint64_t args[2], msg[4];
    uint64_t info = mailbox_wait(mailbox, sizeof(msg) << 16u, args, msg, &handle);
    for (;;) {
        int ret = 0;
        switch (info & 0xFFFF) {
            case USB_CMD_CTRL_XFER: {
                break;
            }

            case USB_CMD_BULK_XFER: {
                fault_handler();

                handle = dev->ipc_ring_vmo[args[0]];
                break;
            }
        }
        // reply and wait for the next message
        info = mailbox_reply(mailbox, (sizeof(msg) << 16u) | (ret & 0xFFFF), args, msg, &handle);
    }
}

static void usb_endpoint_thread(void* arg) {
    uintptr_t a     = (uintptr_t) arg;
    USB_Device* dev = &devices[a >> 32u];
    int pipe        = a & 0xFFFFFFFF;

    uint32_t max_packet_size = dev->in_ctx->arr[pipe + 2].data[1] >> 16u;
    IPC_Endpoint ep = ipc_endpoint(dev->ipc_ring[pipe], false);

    // CACHE to avoid mem_translate calls
    uintptr_t last_vaddr = 0;
    uintptr_t last_paddr = 0;
    do {
        // Send packet
        char* packet = ipc_write_reserve(&ep);
        {
            uint32_t cmd[4];
            HCI_Ring* ring = &dev->xfer_ring[pipe];
            assert(pipe > 0);

            uintptr_t addr = ((uintptr_t) packet) & ~4095ull;
            if (addr != last_vaddr) {
                last_vaddr = addr;
                last_paddr = mem_translate(NULL_HANDLE, (void*) last_vaddr);
            }
            uintptr_t data_paddr = last_paddr + (((uintptr_t) packet) & 4095ull);

            // Normal Packet
            cmd[0] = data_paddr & 0xFFFFFFFF;
            cmd[1] = data_paddr >> 32ull;
            cmd[2] = max_packet_size;
            cmd[3] = (1u << 5u);

            uintptr_t paddr = ring->dequeue_paddr;
            mark_pending_urb(paddr, dev, pipe);
            ring_submit_cmd(ring, TRB_NORMAL, cmd);
            ring_doorbell(dev, pipe + 1);
        }

        // Wait for packet
        fault_handler();
        event_wait(dev->ipc_ring_evt[pipe]);

        printf("REPORT %d.%d: ", DEV_SLOT(dev), pipe);
        for (int j = 0; j < 8; j++) {
            printf(" %02x", packet[j] & 0xFF);
        }
        printf("\n");
        fault_handler();

        // Tell user about it
        ipc_write_commit(&ep, max_packet_size);
    } while (true);
}

#define USBSTS_CNR (1<<11)
int _start(KHandle pci_handle) {
    root_mailbox = get_root_mailbox();

    PCI_Desc pci;
    int res = syscall(SYS_pci_claim_device, pci_handle, &pci);
    if (res < 0) {
        thread_exit(1);
    }

    mmio = mem_map(NULL_HANDLE, 0, pci.bars[0], 0, pci.sizes[0], PROT_RW, 0);

    uintptr_t dcbaap_paddr;
    dcbaap = alloc_pinned_page(&dcbaap_paddr);

    ring_alloc(&crcr, 256);

    uintptr_t ers0_paddr;
    uint32_t* ers0 = alloc_pinned_page(&ers0_paddr);

    uintptr_t erst_paddr;
    uint32_t* erst = alloc_pinned_page(&erst_paddr);

    // ERS0 has 4K worth of TRB entries
    erst[0] = ers0_paddr & 0xFFFFFFFF;
    erst[1] = ers0_paddr >> 32ull;
    erst[2] = 256;
    erst[3] = 0;

    volatile uint32_t* rt_regs = &mmio[mmio[6] >> 2];
    op_base  = &mmio[(mmio[0] & 0xFF) >> 2];
    doorbell = &mmio[mmio[5] >> 2];

    context_slot_size = (mmio[4] >> 2) & 1 ? 64 : 32;
    printf("[usb]  Slot size: %zu\n", context_slot_size);

    uint32_t max_ports = mmio[1] >> 24u;
    uint32_t max_slots = mmio[1] & 0xFF;
    devices = mem_map_private(NULL_HANDLE, max_slots*sizeof(USB_Device), PROT_RW, 0);

    // XHCI initialization
    //   1. Wait for hardware reset (USBSTS @ 4h)
    op_base[0] |= 2;
    thread_sleep(100000);
    while (op_base[1] & USBSTS_CNR) {}
    //   2. Program max device slots
    op_base[14] = max_slots; // (CONFIG @ 38h)
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
    asm volatile("mfence" : : : "memory");

    printf("[usb]  Detected %u root hub ports (%d max slots)\n", max_ports, max_slots);
    FOR_N(i, 0, max_ports) {
        volatile uint32_t* sts = &op_base[0x100 + (4 * i)];
        if (*sts & 1) {
            printf("[usb]  Resetting root hub Port%zu...\n", i);
            *sts |= 1 << 4;
        }
    }

    printf("[usb]  Ready to receive events!\n");
    fault_handler();

    bool ccs = true;
    uintptr_t trb_i = 0;
    for (;;) {
        assert(trb_i < 1024);

        int poke = dev_to_refresh;
        bool advanced = false;
        while ((ers0[trb_i+3] & 1) == ccs) {
            asm volatile("mfence" : : : "memory");

            volatile uint32_t* trb = &ers0[trb_i];
            uint32_t type = (trb[3] >> 10) & 0b111111;

            uintptr_t trb_phys = ers0_paddr + trb_i*sizeof(uint32_t);
            // ring_dump_cmd(trb_phys, (uint32_t*) trb);

            if (type == 0x21) { // Command completion
                uintptr_t cmd_ptr = trb[0] | ((uintptr_t) trb[1] << 32ull);
                volatile uint32_t* cmd = ring_cmd_at(&crcr, cmd_ptr);

                uint32_t completed_type = (cmd[3] >> 10) & 0b111111;
                uint32_t completition_code = trb[2] >> 24u;

                if (completed_type != 1) {
                    printf("%d : Completed Event%p %#x (type=%d, cc=%#x)\n", trb[2] >> 24u, cmd_ptr, trb[3], completed_type, completition_code);
                    fault_handler();
                }

                if (completed_type == 12) {
                    // configure endpoint
                    //   allow others to use it now
                    if (poke == (cmd[3] >> 24u)) {
                        dev_to_refresh = 0;
                    }
                } else {
                    // We have a slot ID now, forward it to whoever was waiting
                    for (int i = 0; i < pending_con_count; i++) {
                        if (pending_con[i].init_cmd == cmd_ptr) {
                            // remove-swap
                            int state = pending_con[i].state;
                            int port  = pending_con[i].port;
                            if (--pending_con_count) {
                                pending_con[i] = pending_con[pending_con_count];
                            }

                            if (state == PORT_DEFAULT && completed_type == 9) {
                                int slot = (trb[3] >> 24u) - 1;
                                devices[slot].state = state;
                                ports[port].slot = slot;
                                usb_fsm(MSG_SLOT_ENABLE, port, slot);
                            } else if (state == PORT_WAIT_FOR_ADDRESS && completed_type == 11) {
                                usb_fsm(MSG_ADDRESSED, port, ports[port].slot);
                            }
                            break;
                        }
                    }
                }
            } else if (type == 0x22) { // Port status change
                int port = (trb[0] >> 24) - 1;
                usb_fsm(MSG_PORT_CHANGE, port, ports[port].slot);
            } else if (type == 0x20) { // Transfer event
                uintptr_t cmd_ptr = trb[0] | ((uintptr_t) trb[1] << 32ull);
                uint32_t completition_code = trb[2] >> 24u;
                signal_pending_urb(cmd_ptr, completition_code, trb[2] & 0xFFFFFF);
            } else {
                printf("Unsupported event %d\n", type);
            }
            advanced = true;
            fault_handler();

            trb_i += 4;
            if (trb_i*4 == 0x1000) {
                // wrap around
                trb_i = 0;
                ccs = !ccs;
            }
        }

        if (poke) {
            USB_Device* dev = &devices[poke - 1];

            uint32_t cmd[4] = { dev->in_ctx_paddr & 0xFFFFFFF0, dev->in_ctx_paddr >> 32ull, 0, (1 + DEV_SLOT(dev)) << 24u };
            ring_submit_cmd(&crcr, 12, cmd);
            ring_doorbell(NULL, 0);
        }

        if (advanced) {
            // update ERDP
            uintptr_t erdp_phys = ers0_paddr + trb_i*sizeof(uint32_t);

            interrupt[7] = (erdp_phys >> 32ull);
            interrupt[6] = erdp_phys & 0xFFFFFFFF;
        }

        fault_handler();
        thread_sleep(10000);
    }

    return 0;
}
