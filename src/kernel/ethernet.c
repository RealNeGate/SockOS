#include <kernel.h>
#include "pci.h"

#define E1000_REG_RCTRL   0x100
#define E1000_RXD_BASE_LO 0x2800
#define E1000_RXD_BASE_HI 0x2804
#define E1000_RXD_LEN     0x2808
#define E1000_RXD_HEAD    0x2810
#define E1000_RXD_TAIL    0x2818

#define E1000_REG_RXADDR  0x5400

#define E1000_REG_TCTRL   0x400
#define E1000_TXD_BASE_LO 0x3800
#define E1000_TXD_BASE_HI 0x3804
#define E1000_TXD_LEN     0x3808
#define E1000_TXD_HEAD    0x3810
#define E1000_TXD_TAIL    0x3818

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

#define ICR_TXDW   (1 << 0)
#define ICR_TXQE   (1 << 1)  /* Transmit queue is empty */
#define ICR_LSC    (1 << 2)  /* Link status changed */
#define ICR_RXSEQ  (1 << 3)  /* Receive sequence count error */
#define ICR_RXDMT0 (1 << 4)  /* Receive descriptor minimum threshold */

#define ICR_RXO    (1 << 6)  /* Receive overrun */
#define ICR_RXT0   (1 << 7)  /* Receive timer interrupt? */
#define ICR_ACK    (1 << 17)
#define ICR_SRPD   (1 << 16)

#define RCTL_BSIZE_4096 ((3 << 16) | (1 << 25))
#define RCTL_BSIZE_8192 ((2 << 16) | (1 << 25))

#define CTRL_RST (1 << 26)

#define E1000_REG_CTRL   0x00 // Device Control
#define E1000_REG_STATUS 0x08 // Device Status
#define E1000_MASK_SET   0xD0 // Interrupt Mask Set/Read
#define E1000_MASK_CLEAR 0xD8 // Interrupt Mask Clear
#define E1000_INT_READ   0xC0 // Interrupt Cause Read

#define E1000_FLOW_LO       0x028 // Flow Control Low Address
#define E1000_FLOW_HI       0x02C // Flow Control High Address
#define E1000_FLOW_TYPE     0x030 // Flow Control Type
#define E1000_FLOW_TX_TIMER 0x170 // Flow Control TX Timer

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
    volatile u8  checksum_offset;
    volatile u8  cmd;
    volatile u8  status;
    volatile u8  checksum_start;
    volatile u16 special;
} __attribute__((packed)) e1000_tx_desc;

static char *mac_to_str(u8 *mac, char *str) {
    int j = 0;
    for (int i = 0; i < 6; i++) {
        u8 tbuf[64];
        int sz = itoa(mac[i], tbuf, 16);

        if (sz == 2) {
            str[j++] = '0';
        }

        for (int k = 0; k < sz - 1; k++) {
            str[j++] = tbuf[k];
        }

        if (i != 5) {
            str[j++] = ':';
        }
    }
    str[j] = 0;

    return str;
}

static e1000_rx_desc *rx_descs[32];
static e1000_tx_desc *tx_descs[8];


void write_cmd(u64 addr, u32 val) {
    (*(volatile u32 *)(addr)) = val;
}
u32 read_cmd(u64 addr) {
    return (*(volatile u32 *)(addr));
}

void eth_interrupt(void *ctx) {
    kprintf("PACKET TIME\n");
}

void disable_interrupts(uintptr_t reg_base) {
    write_cmd(reg_base + E1000_MASK_CLEAR, 0xFFFFFFFF);
    write_cmd(reg_base + E1000_INT_READ, 0xFFFFFFFF);
    read_cmd(reg_base + E1000_REG_STATUS);
}

static bool init_eth(PCI_Device *eth_dev) {
    BAR mem_bar = parse_bar(eth_dev->bar[0]);
    u64 mem_base = mem_bar.addr;

    uintptr_t end_addr = PAGE_ALIGN(mem_base + E1000_REG_RXADDR);
    u64 size = end_addr - mem_base;

    void *reg_ptr = paddr2kaddr(mem_base);
    uintptr_t reg_base = (uintptr_t)reg_ptr;
    memmap_view(boot_info->kernel_pml4, mem_base, reg_base, size, VMEM_PAGE_WRITE);

    // Get Mac Address
    u32 mac_addr_lo = *(volatile u32 *)(reg_ptr + E1000_REG_RXADDR);
    u16 mac_addr_hi = *(volatile u16 *)(reg_ptr + E1000_REG_RXADDR + 4);

    u8 mac[6];
    mac[0] = mac_addr_lo & 0xFF;
    mac[1] = (mac_addr_lo >> 8) & 0xFF;
    mac[2] = (mac_addr_lo >> 16) & 0xFF;
    mac[3] = (mac_addr_lo >> 24) & 0xFF;
    mac[4] = mac_addr_hi & 0xFF;
    mac[5] = (mac_addr_hi >> 8) & 0xFF;

    char mac_str[(3 * 6) + 1];
    kprintf("[eth] MAC: %s\n", mac_to_str(mac, mac_str));

    // Allocate RX and TX Rings
    void *rx_virt_ptr = kheap_alloc(sizeof(rx_descs));
    uintptr_t rx_phys_addr = kaddr2paddr(rx_virt_ptr);

    void *tx_virt_ptr = kheap_alloc(sizeof(tx_descs));
    uintptr_t tx_phys_addr = kaddr2paddr(tx_virt_ptr);

    e1000_rx_desc *r_descs = (e1000_rx_desc *)rx_virt_ptr;
    for (int i = 0; i < ELEM_COUNT(rx_descs); i++) {
        rx_descs[i] = (e1000_rx_desc *)((u8 *)(r_descs + i*16));

        kprintf("allocating for rx ring: %d\n", i);
        uintptr_t ring_addr = kaddr2paddr(kheap_zalloc(4096));
        rx_descs[i]->addr = (u64)ring_addr;
        rx_descs[i]->status = 0;
    }

    e1000_tx_desc *t_descs = (e1000_tx_desc *)tx_virt_ptr;
    for (int i = 0; i < ELEM_COUNT(tx_descs); i++) {
        tx_descs[i] = (e1000_tx_desc *)((u8 *)(t_descs + i*16));

        kprintf("allocating for tx ring: %d\n", i);
        uintptr_t ring_addr = kaddr2paddr(kheap_zalloc(4096));
        tx_descs[i]->addr = (u64)ring_addr;
        tx_descs[i]->status = 0;
        tx_descs[i]->cmd = 1;
    }

    disable_interrupts(reg_base);

    // Disable RX and TX
    write_cmd(reg_base + E1000_REG_RCTRL, 0);
    write_cmd(reg_base + E1000_REG_TCTRL, TCTL_PSP);
    read_cmd(reg_base + E1000_REG_STATUS);

    // Reset the NIC
    u32 ctrl = read_cmd(reg_base + E1000_REG_CTRL);
    ctrl |= CTRL_RST;
    write_cmd(reg_base + E1000_REG_CTRL, ctrl);

    // Re-disable Interrupts
    disable_interrupts(reg_base);

    // Set up flow control (these are defaults right out of the datasheet)
    write_cmd(reg_base + E1000_FLOW_LO,       0x2C8001);
    write_cmd(reg_base + E1000_FLOW_HI,       0x100);
    write_cmd(reg_base + E1000_FLOW_TYPE,     0x8808);
    write_cmd(reg_base + E1000_FLOW_TX_TIMER, 0xFFFF);


    // Setup RX Ring
    write_cmd(reg_base + E1000_RXD_BASE_LO, rx_phys_addr >> 32);
    write_cmd(reg_base + E1000_RXD_BASE_HI, rx_phys_addr & 0xFFFFFFFF);

    write_cmd(reg_base + E1000_RXD_LEN, ELEM_COUNT(rx_descs) * 16);
    write_cmd(reg_base + E1000_RXD_HEAD, 0);
    write_cmd(reg_base + E1000_RXD_TAIL, ELEM_COUNT(rx_descs) - 1);

    u32 rctl = RCTL_EN;
    rctl |= RCTL_SBP;
    rctl |= RCTL_MPE;
    rctl |= RCTL_BAM;
    rctl |= RCTL_BSIZE_8192;
    rctl |= RCTL_SECRC;
    write_cmd(reg_base + E1000_REG_RCTRL, rctl);
    kprintf("[eth] RX ring configured\n");

    // Setup TX Ring
    write_cmd(reg_base + E1000_TXD_BASE_LO, tx_phys_addr >> 32);
    write_cmd(reg_base + E1000_TXD_BASE_HI, tx_phys_addr & 0xFFFFFFFF);

    write_cmd(reg_base + E1000_TXD_LEN, ELEM_COUNT(tx_descs) * 16);
    write_cmd(reg_base + E1000_TXD_HEAD, 0);
    write_cmd(reg_base + E1000_TXD_TAIL, 0);

    u32 tctl = read_cmd(reg_base + E1000_REG_TCTRL);
    tctl &= ~(0xFF << 4);
    tctl |= (15 << 4);

    tctl |= TCTL_EN;
    tctl |= TCTL_PSP;
    tctl |= TCTL_RTLC;
    write_cmd(reg_base + E1000_REG_TCTRL, tctl);
    kprintf("[eth] TX ring configured\n");

    set_interrupt_line(eth_dev->irq_line, eth_interrupt, NULL);

    // Enable Interrupts
    write_cmd(reg_base + E1000_MASK_SET,
        (ICR_LSC | ICR_RXO | ICR_RXT0 | ICR_TXQE | ICR_TXDW | ICR_ACK | ICR_RXDMT0 | ICR_SRPD)
    );

    return false;
}

static bool exit_eth(PCI_Device *eth_dev) {
    return false;
}

__attribute__((section(".driver"))) Device_Driver driver = {
    .name = "82540EM Gigabit Ethernet Controller",
    .vendor_id = 0x8086,
    .device_id = 0x100E,
    .init = init_eth,
    .exit = exit_eth,
};
