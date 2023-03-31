
int itoa(uint32_t i, uint8_t base, uint16_t* buf) {
    static const char bchars[] = "0123456789ABCDEF";

    int pos   = 0;
    int o_pos = 0;
    int top   = 0;
    uint16_t tbuf[32];

    if (i == 0 || base > 16) {
        buf[0] = '0';
        buf[1] = '\0';
        return 0;
    }

    while (i != 0) {
        tbuf[pos] = bchars[i % base];
        pos++;
        i /= base;
    }
    top = pos--;

    for (o_pos = 0; o_pos < top; pos--, o_pos++) {
        buf[o_pos] = tbuf[pos];
    }
    buf[o_pos] = 0;
    return o_pos;
}

static void efi_print_hex(EFI_SYSTEM_TABLE* st, uint32_t number) {
    uint16_t buffer[32];
    itoa(number, 16, buffer);
    buffer[31] = 0;

    st->ConOut->OutputString(st->ConOut, (int16_t*) buffer);
    st->ConOut->OutputString(st->ConOut, (int16_t*) L"\n\r");
}

static void efi_println(EFI_SYSTEM_TABLE* st, uint16_t* str) {
    st->ConOut->OutputString(st->ConOut, (int16_t*) str);
    st->ConOut->OutputString(st->ConOut, (int16_t*) L"\n\r");
}

static void efi_println_ansi(EFI_SYSTEM_TABLE* st, char* str) {
    for (char *tmp = str; *tmp != '\0'; tmp++) {
        int16_t c[2] = {*tmp, 0};
        st->ConOut->OutputString(st->ConOut, c);
    }
    st->ConOut->OutputString(st->ConOut, (int16_t*) L"\n\r");
}

static void* efi_alloc(EFI_SYSTEM_TABLE* st, size_t alloc_size) {
    EFI_PHYSICAL_ADDRESS addr;
    uint64_t npages = (alloc_size + 0xfff) / 0x1000;
    EFI_STATUS status = st->BootServices->AllocatePages(AllocateAnyPages, EfiLoaderData, npages, &addr);
    if (status != 0) {
        return NULL;
    }
    return (void*) addr;
}

static void* efi_alloc_pages(EFI_SYSTEM_TABLE* st, size_t npages) {
    EFI_PHYSICAL_ADDRESS addr;
    EFI_STATUS status = st->BootServices->AllocatePages(AllocateAnyPages, EfiLoaderData, npages, &addr);
    if (status != 0) {
        return NULL;
    }
    return (void*) addr;
}

static uint64_t efi_cvt_mem_desc_type(uint32_t efi_type) {
    switch(efi_type) {
        case EfiLoaderCode:
        case EfiLoaderData:
            return MEM_REGION_BOOT;
        case EfiBootServicesCode:
        case EfiBootServicesData:
            // return MEM_REGION_UEFI_BOOT;
            return MEM_REGION_USABLE;
        case EfiRuntimeServicesCode:
        case EfiRuntimeServicesData:
            return MEM_REGION_UEFI_RUNTIME;
        case EfiConventionalMemory:
            return MEM_REGION_USABLE;
        case EfiACPIReclaimMemory:
            return MEM_REGION_ACPI;
        case EfiACPIMemoryNVS:
            return MEM_REGION_ACPI_NVS;
        case EfiMemoryMappedIO:
            return MEM_REGION_IO;
        case EfiMemoryMappedIOPortSpace:
            return MEM_REGION_IO_PORTS;
        case EfiPalCode:
        case EfiUnusableMemory:
        case EfiPersistentMemory: // ?
        case EfiReservedMemoryType:
            return MEM_REGION_RESERVED;
        default: {
            if(0x80000000 <= efi_type && efi_type <= 0xFFFFFFFF) {
                return MEM_REGION_USABLE;
            }
            else {
                return MEM_REGION_RESERVED;
            }
        }
    }
}

static char* mem_region_name(uint64_t type) {
    switch(type) {
        case MEM_REGION_USABLE:       return "MEM_REGION_USABLE";
        case MEM_REGION_RESERVED:     return "MEM_REGION_RESERVED";
        case MEM_REGION_BOOT:         return "MEM_REGION_BOOT";
        case MEM_REGION_KERNEL:       return "MEM_REGION_KERNEL";
        case MEM_REGION_UEFI_BOOT:    return "MEM_REGION_UEFI_BOOT";
        case MEM_REGION_UEFI_RUNTIME: return "MEM_REGION_UEFI_RUNTIME";
        case MEM_REGION_ACPI:         return "MEM_REGION_ACPI";
        case MEM_REGION_ACPI_NVS:     return "MEM_REGION_ACPI_NVS";
        case MEM_REGION_IO:           return "MEM_REGION_IO";
        case MEM_REGION_IO_PORTS:     return "MEM_REGION_IO_PORTS";
        case MEM_REGION_FRAMEBUFFER:  return "MEM_REGION_FRAMEBUFFER";
    }
    return "MEM_REGION_BAD_TYPE";
}

static MemMap efi_get_mem_map(EFI_SYSTEM_TABLE* st, size_t* efi_map_key, size_t nregions_limit) {
    EFI_STATUS status;
    uint8_t* efi_descs = NULL;
    size_t desc_size = 0;
    uint32_t desc_version = 0;
    size_t size = 0;
    size_t map_key = 0;
    // Get the size of memory map and descriptor size
    status = st->BootServices->GetMemoryMap(&size, (void*) efi_descs, &map_key, &desc_size, &desc_version);
    if(status == 0) {
        panic("UEFI shouldn't've returned success here");
    }
    // Note(flysand): EFI creates one memory descriptor for each allocation.
    // we add 2 to nregions because below we make two more allocations which.
    // we'll need to account for.
    size_t nregions = size / desc_size + 2;
    efi_descs = efi_alloc(st, size);
    if(efi_descs == NULL) {
        panic("Failed to allocate memory for EFI Memory Descriptors");
    }
    MemRegion* regions = efi_alloc(st, nregions_limit * sizeof(MemRegion));
    if(regions == NULL) {
        panic("Failed to allocate memory for MemMap");
    }
    // Get the memory descriptors
    status = st->BootServices->GetMemoryMap(&size, (void*) efi_descs, &map_key, &desc_size, &desc_version);
    if (status != 0) {
        panic("Failed to get memory map!\nStatus: %x", status);
    }
    nregions = size / desc_size;
    // Load memory map into `regions`
    // printf("MemMap as returned by UEFI:\n");
    for (int i = 0; i < nregions && i < nregions_limit; i++) {
        EFI_MEMORY_DESCRIPTOR* desc = (EFI_MEMORY_DESCRIPTOR*)(efi_descs + (i * desc_size));
        uint64_t type = efi_cvt_mem_desc_type(desc->Type);
        uint64_t paddr = (uint64_t) desc->PhysicalStart;
        uint64_t npages = desc->NumberOfPages;
        regions[i].type = type;        
        regions[i].base  = paddr;
        regions[i].pages = npages;
        char* name = mem_region_name(regions[i].type);
        // printf("%d: %X .. %X [%s]\n", i, regions[i].base, regions[i].base + regions[i].pages*4096, name);
    }
    *efi_map_key = map_key;
    MemMap result;
    result.nregions = nregions;
    result.regions = regions;
    result.cap = nregions_limit;
    return result;
}
