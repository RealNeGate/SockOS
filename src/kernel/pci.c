#include <kernel.h>
#include "pci.h"

#define PCI_BASE_ADDR 0x80000000
#define PCI_VALUE_PORT 0xCFC
#define PCI_ADDR_PORT  0xCF8
#define PCI_NONE 0xFFFF

#define PCI_VENDOR_INTEL    0x8086

#define PCI_BRIDGE_PCI2PCI          0x04

#define PCI_CLASS                                                          \
X(PCI_CLASS_UNCLASSIFIED,        0x0,  "Unclassified")                     \
X(PCI_CLASS_STORAGE_CTL,         0x1,  "Storage Controller")               \
X(PCI_CLASS_NETWORK_CTL,         0x2,  "Network Controller")               \
X(PCI_CLASS_DISPLAY_CTL,         0x3,  "Display Controller")               \
X(PCI_CLASS_MULTIMEDIA_CTL,      0x4,  "Multimedia Controller")            \
X(PCI_CLASS_MEMORY_CTL,          0x5,  "Memory Controller")                \
X(PCI_CLASS_BRIDGE,              0x6,  "Bridge")                           \
X(PCI_CLASS_COMMUNICATION_CTL,   0x7,  "Simple Communication Controller")  \
X(PCI_CLASS_SYS_PERIPHERAL,      0x8,  "Base System Peripheral")           \
X(PCI_CLASS_INPUT_CTL,           0x9,  "Input Device Controller")          \
X(PCI_CLASS_DOCKING_STATION,     0xA,  "Docking Station")                  \
X(PCI_CLASS_PROCESSOR,           0xB,  "Processor")                        \
X(PCI_CLASS_SERIAL_BUS_CTL,      0xC,  "Serial Bus Controller")            \
X(PCI_CLASS_WIRELESS_CTL,        0xD,  "Wireless Controller")              \
X(PCI_CLASS_INTELLIGENT_CTL,     0xE,  "Intelligent Controller")           \
X(PCI_CLASS_SATELLITE_COMM_CTL,  0xF,  "Satellite Communication Controller") \
X(PCI_CLASS_ENCRYPT_CTL,         0x10,  "Encryption Controller")            \
X(PCI_CLASS_SIGNAL_PROC_CTL,     0x11,  "Signal Processing Controller")     \
X(PCI_CLASS_PROCESSING_ACCEL,    0x12,  "Processing Accelerator")           \
X(PCI_CLASS_NON_ESSENTIAL_INST,  0x13,  "Non-Essential Instrumentation")    \
X(PCI_CLASS_COPROCESSOR,         0x40,  "Co-Processor")                     \
X(PCI_CLASS_UNASSIGNED,          0xFF,  "Unassigned")

static const char *pci_class_names[] = {
    #define X(tag, id, name) [id] = name,
    PCI_CLASS
    	#undef X
};
typedef enum {
    #define X(tag, id, name) tag = id,
    PCI_CLASS
    	#undef X
} PCI_Class;

#define PCI_SUBCLASS_BR                                                 \
X(PCI_SUBCLASS_BR_HOST,        0x0,  "Host Bridge")                     \
X(PCI_SUBCLASS_BR_ISA,         0x1,  "ISA Bridge")                      \
X(PCI_SUBCLASS_BR_EISA,        0x2,  "EISA Bridge")                     \
X(PCI_SUBCLASS_BR_MCA,         0x3,  "MCA Bridge")                      \
X(PCI_SUBCLASS_BR_PCI2PCI,     0x4,  "PCI-to-PCI Bridge")               \
X(PCI_SUBCLASS_BR_PCMCIA,      0x5,  "PCMCIA Bridge")                   \
X(PCI_SUBCLASS_BR_NUBUS,       0x6,  "NuBus Bridge")                    \
X(PCI_SUBCLASS_BR_CARDBUS,     0x7,  "CardBus Bridge")                  \
X(PCI_SUBCLASS_BR_RACEWAY,     0x8,  "RACEway Bridge")                  \
X(PCI_SUBCLASS_BR_PCI2PCI2,    0x9,  "PCI-to-PCI Bridge")               \
X(PCI_SUBCLASS_BR_INFINBAND2PCI, 0xA,  "Infiniband-to-PCI Host Bridge")

static const char *pci_subclass_bridge_names[] = {
    #define X(tag, id, name) [id] = name,
    PCI_SUBCLASS_BR
    	#undef X
};
typedef enum {
    #define X(tag, id, name) tag = id,
    PCI_SUBCLASS_BR
    	#undef X
} PCI_Subclass_Bridge;

#define PCI_SUBCLASS_NET                                                       \
X(PCI_SUBCLASS_NET_ETH,         0x0,  "Ethernet Controller")                   \
X(PCI_SUBCLASS_NET_TOKEN_RING,  0x1,  "Token Ring Controller")                 \
X(PCI_SUBCLASS_NET_FDDI,        0x2,  "FDDI Controller")                       \
X(PCI_SUBCLASS_NET_ATM,         0x3,  "ATM Controller")                        \
X(PCI_SUBCLASS_NET_ISDN,        0x4,  "ISDN Controller")                       \
X(PCI_SUBCLASS_NET_WORLDFIP,    0x5,  "WorldFip Controller")                   \
X(PCI_SUBCLASS_NET_PICMG,       0x6,  "PICMG 2.14 Multi Computing Controller") \
X(PCI_SUBCLASS_NET_INFINIBAND,  0x7,  "Infiniband Controller")                 \
X(PCI_SUBCLASS_NET_FABRIC,      0x8,  "Fabric Controller")

static const char *pci_subclass_net_names[] = {
    #define X(tag, id, name) [id] = name,
    PCI_SUBCLASS_NET
    	#undef X
};
typedef enum {
    #define X(tag, id, name) tag = id,
    PCI_SUBCLASS_NET
    	#undef X
} PCI_Subclass_Network;

#define PCI_SUBCLASS_DISP                        \
X(PCI_SUBCLASS_DISP_VGA, 0x0,  "VGA Controller") \
X(PCI_SUBCLASS_DISP_XGA, 0x1,  "XGA Controller") \
X(PCI_SUBCLASS_DISP_3D,  0x2,  "3D Controller")

static const char *pci_subclass_display_names[] = {
    #define X(tag, id, name) [id] = name,
    PCI_SUBCLASS_DISP
    	#undef X
};
typedef enum {
    #define X(tag, id, name) tag = id,
    PCI_SUBCLASS_DISP
    	#undef X
} PCI_Subclass_Display;

#define PCI_SUBCLASS_STR                        \
X(PCI_SUBCLASS_STR_SCSI,    0x0,  "SCSI Bus Controller") \
X(PCI_SUBCLASS_STR_IDE,     0x1,  "IDE Controller") \
X(PCI_SUBCLASS_STR_FLOPPY,  0x2,  "Floppy Disk Controller")

static const char *pci_subclass_storage_names[] = {
    #define X(tag, id, name) [id] = name,
    PCI_SUBCLASS_STR
    	#undef X
};
typedef enum {
    #define X(tag, id, name) tag = id,
    PCI_SUBCLASS_STR
    	#undef X
} PCI_Subclass_Storage;

static inline u32 pci_read_u32(u32 bus, u32 device, u32 func, u32 offs) {
    u32 address = PCI_BASE_ADDR | (bus << 16) | (device << 11) | (func << 8) | offs;
    io_out32(PCI_ADDR_PORT, address);
    return io_in32(PCI_VALUE_PORT);
}

BAR parse_bar(u32 bar) {
    BAR b = {};
    b.is_mem = !(bar & 0x1);

    if (b.is_mem) {
        b.type = (bar >> 1) & 0x3;
        b.addr = (bar >> 4) << 4;
        kassert(b.type == 0, "TODO: Only supports BAR 32-bit");
        b.prefetch = (bar >> 3) & 0x1;
    } else {
        b.addr = (bar >> 2) << 2;
    }
    return b;
}
char *pin_names[] = { "NONE", "A", "B", "C" "D" };

static void pci_print_device(PCI_Device *dev) {
    kprintf("[pci] Found %x:%x\n", dev->vendor_id, dev->device_id);

    char *subclass_tag;
    if (dev->subclass == 0x80) {
        subclass_tag = "Other";
    } else {
        switch (dev->class) {
    		case PCI_CLASS_BRIDGE: {
    			subclass_tag = (char *)pci_subclass_bridge_names[dev->subclass];
    		} break;
    		case PCI_CLASS_NETWORK_CTL: {
    			subclass_tag = (char *)pci_subclass_net_names[dev->subclass];
    		} break;
    		case PCI_CLASS_DISPLAY_CTL: {
    			subclass_tag = (char *)pci_subclass_display_names[dev->subclass];
    		} break;
    		case PCI_CLASS_STORAGE_CTL: {
    			subclass_tag = (char *)pci_subclass_storage_names[dev->subclass];
    		} break;
    		default: {
    			subclass_tag = "Unknown";
    		} break;
        }
    }

    kprintf("\tclass: (%d) %s, subclass: (%d) %s\n",
        dev->class,
        pci_class_names[dev->class],
        dev->subclass,
        subclass_tag
    );
    kprintf("\tprog if: %d, revision: %d\n",
        dev->prog_IF,
        dev->revision
    );
    for (int i = 0; i < dev->bar_count; i++) {
        if (dev->bars[i].value == 0) {
            continue;
        }

        BAR bar = parse_bar(dev->bars[i].value);
        kprintf("\t - BAR%d %s addr: %p\n", i, bar.is_mem ? "mem" : "io", bar.addr);
    }

    if (dev->irq_line != 0xFF) {
        kprintf("\tirq: %d, pin: %s\n", dev->irq_line, pin_names[dev->irq_pin]);
    }
}

typedef struct {
    union {
        struct {
            u16 vendor_id;
            u16 device_id;
        };
        u32 reg0;
    };
    union {
        struct {
            u16 command;
            u16 status;
        };
        u32 reg1;
    };
    union {
        struct {
            u8 rev_id;
            u8 prog_IF;
            u8 subclass;
            u8 class;
        };
        u32 reg2;
    };
    union {
        struct {
            u8 bist;
            u8 header_type;
            u8 latency_timer;
            u8 cache_line_size;
        };
        u32 reg3;
    };
} PCI_Header;

typedef struct {
    u32 bar[6];
    u32 cardbus_cis_ptr;

    union {
        struct {
            u16 subsystem_id;
            u16 subsystem_vendor_id;
        };
        u32 regB;
    };

    u32 expansion_rom_base_addr;

    union {
        struct {
            u16 reserved_1;
            u8  reserved_2;
            u8  cap_ptr;
        };
        u32 regD;
    };

    u32 reserved_3;

    union {
        struct {
            u8 interrupt_line;
            u8 interrupt_pin;
            u8 max_latency;
            u8 min_grant;
        };
        u32 regF;
    };
} PCI_Header_0;

typedef struct {
    u32 bar[2];

    union {
        struct {
            u8 secondary_latency_timer;
            u8 subordinate_bus_number;
            u8 secondary_bus_number;
            u8 primary_bus_number;
        };
        u32 reg6;
    };
    union {
        struct {
            u16 secondary_status;
            u8 io_limit;
            u8 io_base;
        };
        u32 reg7;
    };
    union {
        struct {
            u16 memory_limit;
            u16 memory_base;
        };
        u32 reg8;
    };
    union {
        struct {
            u16 prefetch_memory_limit;
            u16 prefetch_memory_base;
        };
        u32 reg9;
    };
    u32 prefetch_memory_base_upper;
    u32 prefetch_memory_limit_upper;
    union {
        struct {
            u16 io_limit_upper;
            u16 io_base_upper;
        };
        u32 regC;
    };

    union {
        struct {
            u16 reserved_1;
            u8  reserved_2;
            u8  cap_ptr;
        };
        u32 regD;
    };

    u32 expansion_rom_base_addr;

    union {
        struct {
            u16 bridge_ctrl;
            u8  interrupt_pin;
            u8  interrupt_line;
        };
        u32 regF;
    };
} PCI_Header_1;

static bool pci_check_device(PCI_Device *dev, u32 bus, u32 device, u8 func) {
    PCI_Header hdr = {};
    hdr.reg0 = pci_read_u32(bus, device, func, 0x0);

    if (hdr.vendor_id == PCI_NONE) return false;

    hdr.reg1 = pci_read_u32(bus, device, func, 0x4);
    hdr.reg2 = pci_read_u32(bus, device, func, 0x8);
    hdr.reg3 = pci_read_u32(bus, device, func, 0xc);

    dev->device_id = hdr.device_id;
    dev->vendor_id = hdr.vendor_id;
    dev->class = hdr.class;
    dev->subclass = hdr.subclass;
    dev->prog_IF = hdr.prog_IF;
    dev->revision = hdr.rev_id;

    switch (hdr.header_type) {
    	case 0: {
    		PCI_Header_0 hdr0 = {};

    		dev->bar_count = 6;
    		for (int i = 0; i < dev->bar_count; i++) {
    			u32 addr = 0x10 + (i * sizeof(u32));
    			dev->bars[i].value = pci_read_u32(bus, device, func, addr);
    		}
    		hdr0.regF = pci_read_u32(bus, device, func, 0x3C);

    		dev->irq_line = hdr0.interrupt_line;
    		dev->irq_pin = hdr0.interrupt_pin;
    	} break;
    	case 0x1: {
    		PCI_Header_1 hdr1 = {};

    		dev->bar_count = 2;
    		for (int i = 0; i < dev->bar_count; i++) {
    			u32 addr = 0x4 + (i * sizeof(u32));
    			dev->bars[i].value = pci_read_u32(bus, device, func, addr);
    		}
    		dev->irq_line = hdr1.interrupt_line;
    		dev->irq_pin  = hdr1.interrupt_pin;
    	} break;
    	default: {
    		kprintf("Unhandled PCI header: %d\n", hdr.header_type);
    		return false;
    	} break;
    }

    return true;
}

extern Device_Driver _DRIVER_START[];
extern Device_Driver _DRIVER_END[];

#define MAX_DEVICES 10
void pci_scan_all(void) {
    PCI_Device devs[MAX_DEVICES];
    int dev_count = 0;

    kprintf("[pci] Scanning for devices!\n");
    for (u32 bus = 0; bus < 256; bus++) {
        for (u32 device = 0; device < 32; device++) {
            PCI_Device *dev = &devs[dev_count];

            for (u8 func = 0; func < 8; func++) {
                if (pci_check_device(dev, bus, device, func)) {
                    pci_print_device(dev);

                    for (Device_Driver* driver = _DRIVER_START; driver != _DRIVER_END; driver++) {
                        if (driver->vendor_id == dev->vendor_id && driver->device_id == dev->device_id) {
                            kprintf("[pci] Driver found for: %s\n", driver->name);
                            if (!driver->init(dev)) {
                                kprintf("[pci] Failed to load driver!\n");
                            }
                            goto next_device;
                        }
                    }
                    next_device:

                    dev_count += 1;
                    if (dev_count >= MAX_DEVICES) {
                        kprintf("[pci] Hit max devices!\n");
                        goto done_scanning;
                    }
                }
            }
        }
    }
    done_scanning:
    kprintf("[pci] Done scanning\n");
}
