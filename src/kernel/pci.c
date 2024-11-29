
#define PCI_VENDOR_ID_OFFS   +0x00
#define PCI_DEVICE_ID_OFFS   +0x02
#define PCI_COMMAND_OFFS     +0x04
#define PCI_STATUS_OFFS      +0x06
#define PCI_CLASS_CODE_OFFS  +0x08
#define PCI_HEADER_TYPE_OFFS +0x0e
#define PCI_PIN_OFFS         +0x3c
#define PCI_LINE_OFFS        +0x3d

// PCI2PCI bridge header
#define PCI_BUS_NUMBER_OFFS  +0x18

#define PCI_VENDOR_INTEL    0x8086

#define PCI_CONFIG_ADDR 0x0cf8
#define PCI_CONFIG_DATA 0x0cfc

#define PCI_CLASS_UNCLASSIFIED      0x00
#define PCI_CLASS_STORAGE_CTL       0x01
#define PCI_CLASS_NETWORK_CTL       0x02
#define PCI_CLASS_DISPLAY_CTL       0x03
#define PCI_CLASS_MULTIMEDIA_CTL    0x04
#define PCI_CLASS_MEM_CTL           0x05
#define PCI_CLASS_BRIDGE            0x06
#define PCI_CLASS_COMMUNICATION_CTL 0x07
#define PCI_CLASS_SYS_PERIPHERAL    0x08
#define PCI_CLASS_INPUT_CTL         0x09
#define PCI_CLASS_DOCKING_STATION   0x0a
#define PCI_CLASS_PROCESSOR         0x0b
#define PCI_CLASS_SERIAL_BUS_CTL    0x0c
#define PCI_CLASS_WIRELESS_CTL      0x0d
#define PCI_CLASS_INTELLIGENT_CTL   0x0e

#define PCI_BRIDGE_PCI2PCI          0x04

// TODO(flysand): These should be in their own file...
static u8 io_read8(u16 port) {
    u8 value;
    asm volatile("in al, dx" : "=a" (value) : "Nd" (port));
    return value;
}

static u32 io_read32(u16 port) {
    u32 value;
    asm volatile("in eax, dx" : "=a" (value) : "Nd" (port));
    return value;
}

static void io_write8(u16 port, u8 data) {
    asm volatile("out dx, al" : : "a" (data), "Nd" (port));
}

static inline u16 pci_config_read_u16(u8 bus, u8 slot, u8 func, u8 offs) {
    u32 address =
    (1u<<31)
        | ((u32) bus  << 16)
        | ((u32) slot << 11)
        | ((u32) func << 8)
        | ((u32) (offs & 0xfc));
    io_write8(PCI_CONFIG_ADDR, address);
    u32 word = io_read32(PCI_CONFIG_DATA);
    u32 bits = (offs&2)*8;
    return (u16) (word >> bits);
}

static inline u32 pci_config_read_u32(u8 bus, u8 slot, u8 func, u8 offs) {
    u32 address =
    (1u<<31)
        | ((u32) bus  << 16)
        | ((u32) slot << 11)
        | ((u32) func << 8)
        | ((u32) (offs & 0xfc));
    io_write8(PCI_CONFIG_ADDR, address);
    return io_read32(PCI_CONFIG_DATA);
}


static inline u16 pci_vendor_id(u8 bus, u8 slot, u8 func) {
    u16 vendor = pci_config_read_u16(bus, slot, func, PCI_VENDOR_ID_OFFS);
    return vendor;
}

static inline u16 pci_device_id(u8 bus, u8 slot, u8 func) {
    u16 device = pci_config_read_u16(bus, slot, func, PCI_DEVICE_ID_OFFS);
    return device;
}

static inline u8 pci_class(u8 bus, u8 slot, u8 func) {
    u16 class = pci_config_read_u16(bus, slot, func, PCI_CLASS_CODE_OFFS);
    return (u8) class;
}

static inline u8 pci_subclass(u8 bus, u8 slot, u8 func) {
    u8 subclass = pci_config_read_u16(bus, slot, func, PCI_CLASS_CODE_OFFS);
    return (u8) (subclass>>8);
}

static inline u8 pci_header_type(u8 bus, u8 slot, u8 func) {
    u8 header_type = pci_config_read_u16(bus, slot, func, PCI_HEADER_TYPE_OFFS);
    return (u8) header_type;
}

static inline u8 pci_secondary_bus(u8 bus, u8 slot, u8 func) {
    u8 bus_numbers = pci_config_read_u16(bus, slot, func, PCI_BUS_NUMBER_OFFS);
    return (u8) (bus_numbers>>8);
}

static void pci_scan_bus(u8 bus);

static void pci_scan_function(u8 bus, u8 slot, u8 func) {
    u8 class = pci_class(bus, slot, func);
    u8 subclass = pci_subclass(bus, slot, func);
    kprintf("[pci] device %.02d:%02d.%d, %02x%02x\n", bus, slot, func, class, subclass);
    if(class == PCI_CLASS_BRIDGE) {
        if(subclass == PCI_BRIDGE_PCI2PCI) {
            u8 secondary_bus = pci_secondary_bus(bus, slot, func);
            pci_scan_bus(secondary_bus);
        }
    }
}

static void pci_scan_slot(u8 bus, u8 slot) {
    u16 vendor_id = pci_vendor_id(bus, slot, 0);
    if(vendor_id == 0xffff) {
        return;
    }
    kprintf("[pci] %.2d:%.2d, vendor id = %04x\n", bus, slot, vendor_id);
    pci_scan_function(bus, slot, 0);
    u8 header_type = pci_header_type(bus, slot, 0);
    if((header_type & 0x80) == 0) {
        return;
    }
    for(u8 func = 0; func < 8; ++func) {
        u16 func_vendor_id = pci_vendor_id(bus, slot, func);
        if(func_vendor_id == 0xffff) {
            continue;
        }
        pci_scan_function(bus, slot, func);
    }
}

static void pci_scan_bus(u8 bus) {
    for(u32 slot = 0; slot < 32; ++slot) {
        pci_scan_slot(bus, slot);
    }
}

static void pci_scan_all(void) {
    kprintf("[pci] TEST2 %08x\n", pci_config_read_u32(0, 1, 1, 0));

    u8 header_type = pci_header_type(0, 0, 0);
    kprintf("[pci] Scanning PCI devices (header %02x)\n", header_type);
    if((header_type & 0x80) == 0) {
        pci_scan_bus(0);
    } else for(u8 func = 0; func < 8; ++func) {
        u16 vendor_id = pci_vendor_id(0, 0, func);
        kprintf("[pci] Scan function %d, vendor id %02x\n", func, vendor_id);
        if(vendor_id != 0xffff) {
            break;
        }
        pci_scan_bus(func);
    }
    kprintf("[pci] Scan over\n");
}



