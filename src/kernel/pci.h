#pragma once
#include <kernel.h>

typedef struct {
	u32 value;
	u32 size;
} Raw_BAR;

struct PCI_Device {
    KObject super; // tag = KOBJECT_DEV_PCI

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

BAR parse_bar(Raw_BAR bar);
