
typedef struct {
    void* entry_addr;
    u64 virt_base;
    u64 phys_base;
    size_t size;
} ELF_Module;

static bool elf_load(EFI_SYSTEM_TABLE* st, void* elf_base, ELF_Module *module) {
    u8* elf = elf_base;
    Elf64_Ehdr* elf_header = (Elf64_Ehdr*)elf;
    u8* segments = elf + elf_header->e_phoff;
    size_t segment_size = elf_header->e_phentsize;
    size_t segment_count = elf_header->e_phnum;
    // Determine the image size
    size_t min_vaddr = (size_t)0xffffffffffffffffull;
    size_t max_vaddr = 0;
    for (size_t i = 0; i < segment_count; i++) {
        Elf64_Phdr* segment = (Elf64_Phdr*) (segments + i*segment_size);
        if (segment->p_type != PT_LOAD) {
            continue;
        }
        // Check segment
        if ((segment->p_flags & (PF_X | PF_W)) == (PF_X | PF_W)) {
            panic("Write-execute pages are banned in this loader\n");
        }
        if (segment->p_filesz > segment->p_memsz) {
            panic("No enough space in this section for the file data\n");
        }
        if ((segment->p_align & (segment->p_align - 1)) != 0) {
            panic("segment must be aligned to a power-of-two\n");
        }
        u64 segment_vstart = segment->p_vaddr;
        u64 segment_vend = segment->p_vaddr + segment->p_memsz;
        if(segment_vstart < min_vaddr) {
            min_vaddr = segment_vstart;
        }
        if(segment_vend > max_vaddr) {
            max_vaddr = segment_vend;
        }
    }
    size_t image_size = max_vaddr - min_vaddr;
    // Allocate the memory for the image
    u8* phys_base = efi_alloc(st, image_size);
    if (phys_base == NULL) {
        panic("Failed to allocate space for kernel phys_base!\n");
    }
    printf("Phys addr: %X..%X\n", phys_base, phys_base + image_size);
    // Copy the segments into memory at the respective addresses in physical memory
    u64 virt_base = min_vaddr;
    printf("Virt addr: %X\n", virt_base);
    size_t pages_used = 0;
    for (size_t i = 0; i < segment_count; i++) {
        Elf64_Phdr* segment = (Elf64_Phdr*) (segments + i*segment_size);
        if (segment->p_type != PT_LOAD) {
            continue;
        }
        u64 base_offset = segment->p_vaddr - virt_base;
        printf("  Segment offset: %X\n", base_offset);
        u8* dst_data = phys_base + base_offset;
        u8* src_data = elf + segment->p_offset;
        memcpy(dst_data, src_data, segment->p_filesz);
        printf("Memory dump at %X:\n", dst_data);
        memdump(dst_data, 16);
        // TODO(NeGate): set new memory protection rules
        #if 0
        DWORD new_protect = 0;
        if (segment->p_flags == PF_R) new_protect = PAGE_READONLY;
        else if (segment->p_flags == (PF_R|PF_W)) new_protect = PAGE_READWRITE;
        else if (segment->p_flags == (PF_R|PF_X)) new_protect = PAGE_EXECUTE_READ;

        if (new_protect == 0) {
            panic("error: could not resolve memory protection rules on segment\n\n");
            return 1;
        }
        #endif
    }
    // Find the RELA section and resolve the relocations
    u8* sections = elf + elf_header->e_shoff;
    size_t section_count = elf_header->e_shnum;
    printf("Resolving relocations\n");
    for (size_t i = 0; i < section_count; i++) {
        Elf64_Shdr* section = (Elf64_Shdr*) (sections + i * elf_header->e_shentsize);
        if (section->sh_type != SHT_RELA) {
            continue;
        }
        Elf64_Rela* relocs = (Elf64_Rela*) (elf + section->sh_offset);
        size_t nrelocs = section->sh_size / sizeof(Elf64_Rela);
        for (size_t j = 0; j < nrelocs; j += 1) {
            Elf64_Rela* rela = &relocs[j];
            u8 type = ELF64_R_TYPE(rela->r_info);
            if (type == R_X86_64_RELATIVE) {
                size_t reloc_offset = rela->r_offset - virt_base;
                size_t reloc_addend = rela->r_addend - virt_base;
                u64 patch_address = (u64)(phys_base + reloc_offset);
                u64 resolved_address = (u64)(virt_base + reloc_addend);
                printf("  Reloc: [%X] = %X\n", patch_address, resolved_address);
                *(u64*)patch_address = resolved_address;
            } else {
                panic("Unable to handle unknown relocation type!\n\n");
            }
        }
    }
    module->entry_addr = (void*) elf_header->e_entry;
    module->virt_base = virt_base;
    module->phys_base = (u64) phys_base;
    module->size = image_size;
    return true;
}
