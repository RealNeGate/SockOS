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
    uint64_t xdst_addr;
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
} XSDT;

void parse_acpi(void *rdsp) {
    ACPI_RSDP_Desc_V2 *header = (ACPI_RSDP_Desc_V2 *)rdsp;
    kprintf("header: %x\n", (uint64_t)header);
    kprintf("signature: %d\n", header->rsdp_head.signature[0]);
}
