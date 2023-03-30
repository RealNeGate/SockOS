

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
