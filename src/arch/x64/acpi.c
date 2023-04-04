typedef struct __attribute__((packed)) {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_addr;
} ACPI_RSDP_Desc;

typedef struct __attribute__((packed)) {
    ACPI_RSDP_Desc rsdp_head;
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} ACPI_RSDP_Desc_V2;

typedef struct __attribute__((packed)) {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} ACPI_SDT_Header;

typedef struct __attribute__((packed)) {
    ACPI_SDT_Header header;
    uint64_t other_headers[];
} ACPI_XSDT_Header;

typedef struct __attribute__((packed)) {
	uint32_t lapic_addr;
	uint32_t flags;
} ACPI_APIC_Header;

typedef struct __attribute__((packed)) {
	uint8_t type;
	uint8_t length;
} ACPI_APIC_Entry;

typedef enum {
	APIC_ENTRY_LAPIC  = 0,
	APIC_ENTRY_IOAPIC = 1,
	APIC_ENTRY_IOAPIC_INTERRUPT_SRC_OVERRIDE     = 2,
	APIC_ENTRY_IOAPIC_NONMASKABLE_INTERRUPT_SRC  = 3,
	APIC_ENTRY_LAPIC_NONMASKABLE_INTERRUPTS      = 4,
	APIC_ENTRY_LAPIC_ADDRESS_OVERRIDE            = 5,
	APIC_ENTRY_LOCAL_X2APIC                      = 9,
} APIC_Entry_Type;

void parse_acpi(void *rsdp) {
    ACPI_RSDP_Desc_V2 *header = (ACPI_RSDP_Desc_V2 *)rsdp;
	uint8_t rsdp_magic[] = {'R', 'S', 'D', ' ', 'P', 'T', 'R', ' ' };
	if (!memeq(header->rsdp_head.signature, rsdp_magic, sizeof(rsdp_magic))) {
		panic("Invalid ACPI header! Got: %.*s || %.*s\n", 
			sizeof(rsdp_magic), rsdp_magic,
			sizeof(header->rsdp_head.signature),
			header->rsdp_head.signature
		);
	}

	ACPI_XSDT_Header *xhead = (ACPI_XSDT_Header *)header->xsdt_addr;
	uint8_t xsdt_magic[] = {'X', 'S', 'D', 'T' };
	if (!memeq(xhead->header.signature, xsdt_magic, sizeof(xsdt_magic))) {
		panic("Invalid XSDT header! Got: %.*s || %.*s\n", 
			sizeof(xsdt_magic), xsdt_magic,
			sizeof(xhead->header.signature),
			xhead->header.signature
		);
	}

	int core_count = 0;
	uint64_t apic_addr = 0;

	uint8_t apic_magic[] = {'A', 'P', 'I', 'C' };
	uint64_t remaining_length = xhead->header.length - sizeof(xhead->header);
	int entries = remaining_length / 8;
	for (int i = 0; i < entries; i++) {
		ACPI_SDT_Header *head = (ACPI_SDT_Header *)xhead->other_headers[i];

		if (!memeq(head->signature, apic_magic, sizeof(apic_magic))) {
			continue;
		}

		char *buf_ptr = (char *)head + sizeof(ACPI_SDT_Header);
		ACPI_APIC_Header *ahead = (ACPI_APIC_Header *)buf_ptr;
		apic_addr = ahead->lapic_addr;

		buf_ptr += sizeof(ACPI_APIC_Header);
		while (buf_ptr < (char *)(head + sizeof(ACPI_APIC_Header))) {
			ACPI_APIC_Entry *entry = (ACPI_APIC_Entry *)buf_ptr;
			if (entry->type == APIC_ENTRY_LAPIC) {
				core_count += 1;
			}
			buf_ptr += entry->length;
		}
	}
	if (core_count == 0) {
		panic("Invalid core count from ACPI!\n");
	}

	kprintf("Got %d cores, local APIC at 0x%x!\n", core_count, apic_addr);
}
