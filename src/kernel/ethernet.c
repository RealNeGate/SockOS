#include <kernel.h>
#include "pci.h"

static void print_mac(u8 *mac) {
    kprintf("[eth] MAC: ");
    for (int i = 0; i < 6; i++) {
        kprintf("%x%s", mac[i], i == 5 ? "\n" : ":");
    }
}

static bool init_eth(PCI_Device *eth_dev) {
    BAR mem_bar = parse_bar(eth_dev->bars[0].value);
    u64 mem_base = mem_bar.addr;

    uintptr_t end_addr = PAGE_ALIGN(mem_base + 0x5400);
    u64 size = end_addr - mem_base;

    void *muh_ptr = paddr2kaddr(mem_base);
    memmap_view(boot_info->kernel_pml4, mem_base, (uintptr_t)muh_ptr, size, VMEM_PAGE_WRITE);

    u32 mac_addr_lo = *(u32 *)(muh_ptr + 0x5400);
    u16 mac_addr_hi = *(u16 *)(muh_ptr + 0x5400 + 4);

    u8 mac[6];
    mac[0] = mac_addr_lo & 0xFF;
    mac[1] = (mac_addr_lo >> 8) & 0xFF;
    mac[2] = (mac_addr_lo >> 16) & 0xFF;
    mac[3] = (mac_addr_lo >> 24) & 0xFF;
    mac[4] = mac_addr_hi & 0xFF;
    mac[5] = (mac_addr_hi >> 8) & 0xFF;
    print_mac(mac);

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

