#pragma once
#include "kernel.h"

typedef struct __attribute__((packed)) {
    u64 address;
    u16 pci_segment_group;
    u8  start_bus;
    u8  end_bus;
    u32 reserved;
} PCI_SegmentGroup;

typedef struct {
    u32 value;
    u32 size;
} Raw_BAR;

struct PCI_Device {
    KObject super; // tag = KOBJECT_DEV_PCI

    // which application actually claimed the
    // PCI device, this is who's responsible for
    // the interrupts as well.
    _Atomic(Env*) parent;

    u8 bus;
    u8 device;
    u8 func;

    u16 vendor_id;
    u16 device_id;

    u8 class;
    u8 subclass;
    u8 prog_IF;
    u8 revision;

    Raw_BAR bar[7];
    int bar_count;

    u8 irq_line;
    u8 irq_pin;

    u32 status;
};

typedef struct {
    u64 addr;
    u64 size;
    bool prefetch;
    u8 type;

    bool is_mem;
} BAR;

typedef struct {
    char *name;

    int vendor_id;
    int device_id;

    bool (*init)(PCI_Device *dev);
    bool (*exit)(PCI_Device *dev);
} Device_Driver;

extern PCI_SegmentGroup* pci_segment_group;

BAR parse_bar(Raw_BAR bar);
void pci_print_device(PCI_Device *dev);

u32 pci_read_u32(u32 bus, u32 device, u32 func, u32 offs);
void pci_write_u32(u32 bus, u32 device, u32 func, u32 offs, u32 value);
