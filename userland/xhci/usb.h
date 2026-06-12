#pragma once

enum {
    USB_DT_DEVICE    = 0x01,
    USB_DT_CONFIG    = 0x02,
    USB_DT_STRING    = 0x03,
    USB_DT_INTERFACE = 0x04,
    USB_DT_ENDPOINT  = 0x05,

    USB_DT_HID       = 0x21,
    USB_DT_REPORT    = 0x22,
    USB_DT_PHYSICAL  = 0x23,
    USB_DT_HUB       = 0x29,
};

typedef struct {
    uint8_t length;
    uint8_t desc_type;
    uint16_t bcd_usb;

    uint8_t dev_class;
    uint8_t dev_sub_class;
    uint8_t protocol;
    uint8_t max_packet_size;

    uint16_t id_vendor;
    uint16_t id_product;
    uint16_t bcd_dev;

    uint8_t manufacturer;
    uint8_t product;
    uint8_t serial_number;
    uint8_t num_configs;
} USB_DevDesc;

typedef struct {
    uint8_t length;
    uint8_t type;
    uint8_t interface_num;
    uint8_t alt_setting;
    uint8_t endpoint_count;
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    uint8_t interface_str;
} USB_InterfaceDesc;

enum {
    USB_SPEED_UNKNOWN,
    USB_SPEED_LOW,
    USB_SPEED_FULL,       // USB 1.1
    USB_SPEED_HIGH,       // USB 2.0
    USB_SPEED_WIRELESS,   // USB 2.5... unsupported by us
    USB_SPEED_SUPER,      // USB 3.0
    USB_SPEED_SUPER_PLUS, // USB 3.1 (TODO)
};

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
    bool cycle_bit;

    uint32_t count;
    uint32_t* base;
    uintptr_t base_paddr;

    // Dequeue pointer
    uint32_t* dequeue;
    uintptr_t dequeue_paddr;
} HCI_Ring;

typedef struct {
    enum USB_DeviceState {
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

    // there's ever only 255 devices or hubs and
    // the root controller is attached to slot0
    // int slot;

    // USB_GET_DESCRIPTOR(DT_DEVICE, 0)
    USB_DevDesc desc;

    // Device rings
    HCI_Ring xfer_ring;

    // for notifying usb_submit_urb_sync
    atomic_bool complete_sync;
} USB_Device;

typedef struct {
    USB_Device* dev;

    // Setup stage
    struct URB_Setup {
        uint8_t request_type;
        uint8_t request;
        uint16_t value;
        uint16_t index;
        uint16_t length;
    } setup;

    // Data stage
    size_t data_len;
    void* data;

    // Status stage
    // void* user_data;
    // void (*cont)(void* user_data);
} USB_RequestBlock;
_Static_assert(sizeof(struct URB_Setup) == 8, "bad!!!");

