#pragma once

typedef struct {
	u32 value;
	u32 size;
} Raw_BAR;

typedef struct {
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
} PCI_Device;

typedef struct {
	u64 addr;
	bool prefetch;
	u8 type;

	bool is_mem;
} BAR;

BAR parse_bar(u32 bar);
