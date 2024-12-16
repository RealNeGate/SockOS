#include "x64.h"

typedef struct __attribute__((packed)) {
    char signature[8];
    u8   checksum;
    char oem_id[6];
    u8   revision;
    u32  rsdt_addr;
} ACPI_RSDP_Desc;

typedef struct __attribute__((packed)) {
    ACPI_RSDP_Desc rsdp_head;
    u32            length;
    u64            xsdt_addr;
    u8             extended_checksum;
    u8             reserved[3];
} ACPI_RSDP_Desc_V2;

typedef struct __attribute__((packed)) {
    char signature[4];
    u32  length;
    u8   revision;
    u8   checksum;
    char oem_id[6];
    char oem_table_id[8];
    u32  oem_revision;
    u32  creator_id;
    u32  creator_revision;
} ACPI_SDT_Header;

typedef struct __attribute__((packed)) {
    ACPI_SDT_Header header;
    u64             other_headers[];
} ACPI_XSDT_Header;

typedef struct __attribute__((packed)) {
    u8  address_space_id;
    u8  register_bit_width;
    u8  register_bit_offset;
    u8  reserved;
    u64 address;
} ACPI_HPET_Addr;

typedef struct __attribute__((packed)) {
    u8  hardware_rev_id;
    u8  comparator_count: 5;
    u8  counter_size: 1;
    u8  reserved: 1;
    u8  legacy_replacement: 1;
    u16 pci_vendor_id;
    ACPI_HPET_Addr addr_struct;
    u8  hpet_number;
    u16 minimum_tick;
    u8  page_protection;
} ACPI_HPET_Header;

typedef struct __attribute__((packed)) {
    u8  revision;
    u8  timer_count: 5;
    u8  is_64bit: 1;
    u8  reserved: 1;
    u8  legacy_support: 1;
    u16 vendor_id;
    u32 clock_period_fs;
} ACPI_HPET_GenCap;

#define HPET_COUNTER 30

typedef struct __attribute__((packed)) {
    u32 lapic_addr;
    u32 flags;
} ACPI_APIC_Header;

typedef struct __attribute__((packed)) {
    u8 type;
    u8 length;
} ACPI_APIC_Entry;

typedef struct __attribute__((packed)) {
    u8  type;
    u8  length;
    u8  processor_id;
    u8  apic_id;
    u32 flags;
} ACPI_APIC_LAPIC_Entry;

typedef struct __attribute__((packed)) {
    u8  type;
    u8  length;
    u8  ioapic_id;
    u8  reserved;
    u32 ioapic_address;
    u32 global_system_interrupt_base;
} ACPI_APIC_IOAPIC_Entry;

typedef enum {
    APIC_ENTRY_LAPIC  = 0,
    APIC_ENTRY_IOAPIC = 1,
    APIC_ENTRY_IOAPIC_INTERRUPT_SRC_OVERRIDE     = 2,
    APIC_ENTRY_IOAPIC_NONMASKABLE_INTERRUPT_SRC  = 3,
    APIC_ENTRY_LAPIC_NONMASKABLE_INTERRUPTS      = 4,
    APIC_ENTRY_LAPIC_ADDRESS_OVERRIDE            = 5,
    APIC_ENTRY_LOCAL_X2APIC                      = 9,
} APIC_Entry_Type;

const char* entry_to_string(int type) {
    switch (type) {
        case APIC_ENTRY_LAPIC:                            return "Local APIC";
        case APIC_ENTRY_IOAPIC:                           return "I/O APIC";
        case APIC_ENTRY_IOAPIC_INTERRUPT_SRC_OVERRIDE:    return "I/O APIC Interrupt Source Override";
        case APIC_ENTRY_IOAPIC_NONMASKABLE_INTERRUPT_SRC: return "I/O APIC Non-Maskable Interrupt Source";
        case APIC_ENTRY_LAPIC_NONMASKABLE_INTERRUPTS:     return "Local APIC Non-Maskable Interrupts";
        case APIC_ENTRY_LAPIC_ADDRESS_OVERRIDE:           return "Local APIC Address Override";
        case APIC_ENTRY_LOCAL_X2APIC:                     return "Local x2APIC";
        default:                                          return "(unknown)";
    }
}

void x86_parse_acpi(void) {
    ACPI_RSDP_Desc_V2 *header = paddr2kaddr(boot_info->rsdp_addr);
    u8 rsdp_magic[] = {'R', 'S', 'D', ' ', 'P', 'T', 'R', ' ' };
    if (!memeq(header->rsdp_head.signature, rsdp_magic, sizeof(rsdp_magic))) {
        panic("Invalid ACPI header! Got: %.*s || %.*s\n",
            sizeof(rsdp_magic), rsdp_magic,
            sizeof(header->rsdp_head.signature),
            header->rsdp_head.signature
        );
    }

    ACPI_XSDT_Header *xhead = paddr2kaddr(header->xsdt_addr);
    u8 xsdt_magic[] = {'X', 'S', 'D', 'T' };
    if (!memeq(xhead->header.signature, xsdt_magic, sizeof(xsdt_magic))) {
        panic("Invalid XSDT header! Got: %.*s || %.*s\n",
            sizeof(xsdt_magic), xsdt_magic,
            sizeof(xhead->header.signature),
            xhead->header.signature
        );
    }

    i32 core_count = 0;
    u64 tsc_freq = 0;

    u8 apic_magic[] = {'A', 'P', 'I', 'C' };
    u8 hpet_magic[] = {'H', 'P', 'E', 'T' };
    u64 remaining_length = xhead->header.length - sizeof(xhead->header);
    int entries = remaining_length / 8;
    for (int i = 0; i < entries; i++) {
        ACPI_SDT_Header *head = paddr2kaddr(xhead->other_headers[i]);
        char *buf_ptr = (char *)head + sizeof(ACPI_SDT_Header);

        // Map the APIC list and build the list of cores
        if (memeq(head->signature, apic_magic, sizeof(apic_magic))) {
            ACPI_APIC_Header *ahead = (ACPI_APIC_Header *)buf_ptr;
            boot_info->lapic_base = ahead->lapic_addr;

            buf_ptr += sizeof(*ahead);
            while (buf_ptr < (char *)(head + sizeof(ACPI_APIC_Header))) {
                ACPI_APIC_Entry *entry = (ACPI_APIC_Entry *)buf_ptr;
                switch (entry->type) {
                    case APIC_ENTRY_LAPIC: {
                        ACPI_APIC_LAPIC_Entry *entry = (ACPI_APIC_LAPIC_Entry *)buf_ptr;
                        boot_info->cores[core_count].core_id  = entry->processor_id;
                        boot_info->cores[core_count].lapic_id = entry->apic_id;
                        core_count += 1;
                    } break;
                    case APIC_ENTRY_IOAPIC: {
                        ACPI_APIC_IOAPIC_Entry *entry = (ACPI_APIC_IOAPIC_Entry *)buf_ptr;
                        boot_info->ioapic_base = entry->ioapic_address;
                    } break;
                }
                buf_ptr += entry->length;
            }

            // Map out the HPET and get the TSC frequency
        } else if (memeq(head->signature, hpet_magic, sizeof(hpet_magic))) {
            ACPI_HPET_Header *hhead = (ACPI_HPET_Header *)buf_ptr;
            volatile u64 *hpet_reg = paddr2kaddr(hhead->addr_struct.address);

            // Map into relevant "identity" site
            memmap_view(boot_info->kernel_pml4, hhead->addr_struct.address, (uintptr_t) hpet_reg, PAGE_SIZE, VMEM_PAGE_WRITE);

            u64 gen_cap_reg = hpet_reg[0];
            ACPI_HPET_GenCap gen_cap = *((ACPI_HPET_GenCap *)&gen_cap_reg);
            u64 hpet_period = gen_cap.clock_period_fs;

            u64 us_delay = 20000;
            asm ("cli");

            // reset / enable the HPET
            hpet_reg[HPET_COUNTER] = 0;
            hpet_reg[2] = 1;

            // Approximate the TSC freq by spin-sleeping for a period
            u64 start = __rdtsc();
            u64 hpet_start = hpet_reg[HPET_COUNTER];
            u64 done_time = hpet_start + (us_delay * (1000000000 / hpet_period));
            while (hpet_reg[HPET_COUNTER] < done_time) {
                asm ("pause");
            }
            u64 end = __rdtsc();

            // disable the HPET
            hpet_reg[2] = 0;
            asm ("sti");

            u64 ticks_per_time = end - start;
            tsc_freq = (ticks_per_time / us_delay);
        }
    }

    if (core_count == 0) {
        panic("Invalid core count from ACPI!\n");
    }

    boot_info->core_count = core_count;
    boot_info->tsc_freq = tsc_freq;

    kassert(boot_info->lapic_base, "We don't have APIC?");
    kprintf("Found the APIC:     %p\n", boot_info->lapic_base);
    kprintf("Found the I/O APIC: %p\n", boot_info->ioapic_base);

    memmap_view(boot_info->kernel_pml4, boot_info->lapic_base, boot_info->lapic_base, PAGE_SIZE, VMEM_PAGE_WRITE);
    memmap_view(boot_info->kernel_pml4, boot_info->ioapic_base, boot_info->ioapic_base, PAGE_SIZE, VMEM_PAGE_WRITE);
}

// Enable APIC
void x86_enable_apic(void) {
    u64 x = x86_readmsr(IA32_APIC_BASE);
    x |= (1u << 11u); // enable APIC
    x86_writemsr(IA32_APIC_BASE, x);

    if (x & IA32_APIC_BASE_MSR_BSP) {
        kprintf("We're the main core\n");
    }
}
