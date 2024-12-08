typedef struct {
	volatile u64 addr;
	volatile u16 length;
	volatile u16 checksum;
	volatile u8  status;
	volatile u8  errors;
	volatile u16 special;
} __attribute__((packed)) e1000_rx_desc;

typedef struct {
	volatile u64 addr;
	volatile u16 length;
	volatile u8  cso;
	volatile u8  cmd;
	volatile u8  status;
	volatile u8  css;
	volatile u16 special;
} __attribute__((packed)) e1000_tx_desc;

static void print_mac(u8 *mac) {
	kprintf("[eth] MAC: ");
	for (int i = 0; i < 6; i++) {
		kprintf("%x%s", mac[i], i == 5 ? "\n" : ":");
	}
}

static e1000_rx_desc *rx_descs[32];
static e1000_tx_desc *tx_descs[8];

#define RX_CTRL     0x100
#define RXD_BASE_LO 0x2800
#define RXD_BASE_HI 0x2804
#define RXD_LEN     0x2808
#define RXD_HEAD    0x2810
#define RXD_TAIL    0x2818

#define TX_CTRL     0x400
#define TX_IPG      0x410
#define TXD_BASE_LO 0x3800
#define TXD_BASE_HI 0x3804
#define TXD_LEN     0x3808
#define TXD_HEAD    0x3810
#define TXD_TAIL    0x3818

#define RCTL_EN            (1 << 1)  // Receiver Enable
#define RCTL_SBP           (1 << 2)  // Store Bad Packets
#define RCTL_UPE           (1 << 3)  // Unicast Promiscuous Enabled
#define RCTL_MPE           (1 << 4)  // Multicast Promiscuous Enabled
#define RCTL_LPE           (1 << 5)  // Long Packet Reception Enable
#define RCTL_LBM_NONE      (0 << 6)  // No Loopback
#define RCTL_LBM_PHY       (3 << 6)  // PHY or external SerDesc loopback
#define RCTL_RDMTS_HALF    (0 << 8)  // Free Buffer Threshold is 1/2 of RDLEN
#define RCTL_RDMTS_QUARTER (1 << 8)  // Free Buffer Threshold is 1/4 of RDLEN
#define RCTL_RDMTS_EIGHTH  (2 << 8)  // Free Buffer Threshold is 1/8 of RDLEN
#define RCTL_MO_36         (0 << 12) // Multicast Offset - bits 47:36
#define RCTL_MO_35         (1 << 12) // Multicast Offset - bits 46:35
#define RCTL_MO_34         (2 << 12) // Multicast Offset - bits 45:34
#define RCTL_MO_32         (3 << 12) // Multicast Offset - bits 43:32
#define RCTL_BAM           (1 << 15) // Broadcast Accept Mode
#define RCTL_VFE           (1 << 18) // VLAN Filter Enable
#define RCTL_CFIEN         (1 << 19) // Canonical Form Indicator Enable
#define RCTL_CFI           (1 << 20) // Canonical Form Indicator Bit Value
#define RCTL_DPF           (1 << 22) // Discard Pause Frames
#define RCTL_PMCF          (1 << 23) // Pass MAC Control Frames
#define RCTL_SECRC         (1 << 26) // Strip Ethernet CRC

#define TCTL_EN            (1 << 1)  // Transmit Enable
#define TCTL_PSP           (1 << 3)  // Pad Short Packets
#define TCTL_CT_SHIFT      4         // Collision Threshold
#define TCTL_COLD_SHIFT    12        // Collision Distance
#define TCTL_SWXOFF        (1 << 22) // Software XOFF Transmission
#define TCTL_RTLC          (1 << 24) // Re-transmit on Late Collision

#define RCTL_BSIZE_8192 ((2 << 16) | (1 << 25))

#define INT_MASK 0xD0 // Interrupt Mask Set/Read

void write_cmd(u64 addr, u32 val) {
	(*(volatile u32 *)(addr)) = val;
}
u32 read_cmd(u64 addr) {
	return (*(volatile u32 *)(addr));
}

static bool init_eth(PCI_Device *eth_dev) {
	BAR mem_bar = parse_bar(eth_dev->bars[0].value);
	u64 mem_base = mem_bar.addr;

	uintptr_t end_addr = PAGE_ALIGN(mem_base + 0x5400);
	u64 size = end_addr - mem_base;

	void *reg_ptr = paddr2kaddr(mem_base);
	uintptr_t reg_base = (uintptr_t)reg_ptr;
	memmap__view(boot_info->kernel_pml4, mem_base, reg_base, size, PAGE_WRITE);

	u32 mac_addr_lo = *(u32 *)(reg_ptr + 0x5400);
	u16 mac_addr_hi = *(u16 *)(reg_ptr + 0x5400 + 4);

	// Get Mac Address
	u8 mac[6];
	mac[0] = mac_addr_lo & 0xFF;
	mac[1] = (mac_addr_lo >> 8) & 0xFF;
	mac[2] = (mac_addr_lo >> 16) & 0xFF;
	mac[3] = (mac_addr_lo >> 24) & 0xFF;
	mac[4] = mac_addr_hi & 0xFF;
	mac[5] = (mac_addr_hi >> 8) & 0xFF;
	print_mac(mac);

	// Setup RX Ring
	void *rx_virt_ptr = kernelfl_alloc(sizeof(rx_descs) + 16);
	uintptr_t rx_phys_addr = kaddr2paddr(rx_virt_ptr);

	e1000_rx_desc *r_descs = (e1000_rx_desc *)rx_virt_ptr;
	for (int i = 0; i < ELEM_COUNT(rx_descs); i++) {
		rx_descs[i] = (e1000_rx_desc *)((u8 *)(r_descs + i*16));

		uintptr_t ring_addr = kaddr2paddr(kernelfl_alloc(8192 + 16));
		rx_descs[i]->addr = (u64)ring_addr;
		rx_descs[i]->status = 0;
	}

	write_cmd(reg_base + RXD_BASE_LO, rx_phys_addr >> 32);
	write_cmd(reg_base + RXD_BASE_HI, rx_phys_addr & 0xFFFFFFFF);

	write_cmd(reg_base + RXD_LEN, ELEM_COUNT(rx_descs) * 16);
	write_cmd(reg_base + RXD_HEAD, 0);
	write_cmd(reg_base + RXD_TAIL, ELEM_COUNT(rx_descs) - 1);

	write_cmd(reg_base + RX_CTRL, RCTL_EN | RCTL_SBP | RCTL_UPE |
					   RCTL_MPE | RCTL_LBM_NONE | RCTL_RDMTS_HALF |
					   RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_8192);
	kprintf("[eth] RX ring configured\n");

	// Setup TX Ring
	void *tx_virt_ptr = kernelfl_alloc(sizeof(tx_descs) + 16);
	uintptr_t tx_phys_addr = kaddr2paddr(tx_virt_ptr);

	e1000_tx_desc *t_descs = (e1000_tx_desc *)tx_virt_ptr;
	for (int i = 0; i < ELEM_COUNT(tx_descs); i++) {
		tx_descs[i] = (e1000_tx_desc *)((u8 *)(t_descs + i*16));
		tx_descs[i]->addr = 0;
		tx_descs[i]->cmd = 0;
		tx_descs[i]->status = 1;
	}
	write_cmd(reg_base + TXD_BASE_LO, tx_phys_addr >> 32);
	write_cmd(reg_base + TXD_BASE_HI, tx_phys_addr & 0xFFFFFFFF);

	write_cmd(reg_base + TXD_LEN, ELEM_COUNT(tx_descs) * 16);
	write_cmd(reg_base + TXD_HEAD, 0);
	write_cmd(reg_base + TXD_TAIL, 0);

	write_cmd(reg_base + TX_CTRL, TCTL_EN | TCTL_PSP | (15 << TCTL_CT_SHIFT) | (64 << TCTL_COLD_SHIFT) | TCTL_RTLC);
	kprintf("[eth] TX ring configured\n");

	// Enable Interrupts
	// write_cmd(reg_base + INT_MASK, ????);

	return false;
}

static bool exit_eth(PCI_Device *eth_dev) {
	return false;
}
