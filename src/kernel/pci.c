#define PCI_BASE_ADDR 0x80000000
#define PCI_VALUE_PORT 0xCFC
#define PCI_ADDR_PORT  0xCF8
#define PCI_NONE 0xFFFF

#define PCI_VENDOR_BLOB      0x00
#define PCI_COMMAND          0x04
#define PCI_CLASS_BLOB       0x08

#define PCI_BAR_START 0x10
#define PCI_INTERRUPT_LINE   0x3C

#define PCI_VENDOR_INTEL    0x8086

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

// FIXME: The asm stubs for these are inexplicably broken
static void out32(u32 port, u32 value) {
	asm volatile ("out dx, eax" :: "dN" (port), "a" (value));
}

static u32 in32(u16 port) {
	u32 rv;
	asm volatile ("in eax, dx" : "=a" (rv) : "dN" (port));
	return rv;
}

static inline u32 pci_read_u32(u32 bus, u32 device, u32 func, u32 offs) {
    u32 address = PCI_BASE_ADDR | (bus << 16) | (device << 11) | (func << 8) | offs;
    out32(PCI_ADDR_PORT, address);
    return in32(PCI_VALUE_PORT);
}

typedef struct {
	u16 vendor_id;
	u16 device_id;

	u8 class;
	u8 subclass;
	u8 prog_IF;
	u8 revision;

	u32 bars[6];
	u8 irq;
} PCI_Device;

static void pci_print_device(PCI_Device *dev) {
	kprintf("[pci] Found %x:%x\n", dev->vendor_id, dev->device_id);
	kprintf("[pci] class: %d, subclass: %d, prog if: %d, revision: %d\n", 
		dev->class,
		dev->subclass,
		dev->prog_IF,
		dev->revision
	);
	kprintf("[pci] bars: [");
	for (int i = 0; i < 6; i++) {
		kprintf("%x,", dev->bars[i]);
	}
	kprintf("]\nirq: %x\n", dev->irq);
}

static bool pci_check_device(PCI_Device *dev, u32 bus, u32 device) {
	u8 func = 0;
	u32 vendor_blob = pci_read_u32(bus, device, func, PCI_VENDOR_BLOB);

	u16 vendor_id = vendor_blob;
	if (vendor_id == PCI_NONE) return false;
	u16 device_id = vendor_blob >> 16;

	u32 class_blob = pci_read_u32(bus, device, func, PCI_CLASS_BLOB);
	u8 class_code  = class_blob >> 24;
	u8 subclass    = (class_blob << 8)  >> 24;
	u8 prog_IF     = (class_blob << 16) >> 24;
	u8 revision    = (class_blob << 24) >> 24;

	u32 bars[6] = {};
	for (int i = 0; i < 6; i++) {
		u32 addr = PCI_BAR_START + (i * sizeof(u32));
		bars[i] = pci_read_u32(bus, device, func, addr);
	}
	u8 irq = pci_read_u32(bus, device, func, PCI_INTERRUPT_LINE);

	dev->vendor_id = vendor_id;
	dev->device_id = device_id;
	dev->class = class_code;
	dev->subclass = subclass;
	dev->prog_IF = prog_IF;
	dev->revision = revision;
	for (int i = 0; i < 6; i++) {
		dev->bars[i] = bars[i];
	}
	dev->irq = irq;

	return true;
}

#define MAX_DEVICES 10
static void pci_scan_all(void) {
	PCI_Device devs[MAX_DEVICES];
	int dev_count = 0;

	kprintf("[pci] Scanning for devices!\n");
	for (u32 bus = 0; bus < 256; bus++) {
		for (u32 device = 0; device < 32; device++) {
			PCI_Device *dev = &devs[dev_count];
			if (pci_check_device(dev, bus, device)) {
				dev_count += 1;
				if (dev_count >= MAX_DEVICES) {
					kprintf("[pci] Hit max devices!\n");
					goto done_scanning;
				}
			}
		}
	}
	done_scanning:
	kprintf("[pci] Done scanning\n");

	for (int i = 0; i < dev_count; i++) {
		pci_print_device(&devs[i]);
	}
}
