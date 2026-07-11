/*
    enum {
        DT_FLAGS_1 = 0x6ffffffb,
        DT_DEBUG   = 21,
        DT_SYMTAB  = 6,
        DT_SYMENT  = 11,
        DT_STRTAB  = 5,
        DT_HASH    = 4,
        DT_STRSZ   = 10,
        DT_GNUHASH = 0x6ffffef5,
    };

    // Elf dynamic state
    size_t symtab = 0, syment = 0;
    char* strtab = NULL;
    char* dyn_ht = NULL;

        if (segment->p_type == PT_DYNAMIC) {
            Elf64_Dyn* dyns = (Elf64_Dyn*) &contents[segment->p_offset];
            size_t dyn_count = segment->p_memsz / sizeof(Elf64_Dyn);

            for (size_t j = 0; j < dyn_count; j++) {
                printf("DYN[%#zx] %p\n", dyns[j].d_tag, dyns[j].d_ptr);

                if (dyns[j].d_tag == DT_HASH)   { dyn_ht = &elf_mirror[dyns[j].d_ptr]; }
                if (dyns[j].d_tag == DT_SYMTAB) { symtab = dyns[j].d_ptr; }
                if (dyns[j].d_tag == DT_SYMENT) { syment = dyns[j].d_ptr; }
                if (dyns[j].d_tag == DT_STRTAB) { strtab = &elf_mirror[dyns[j].d_ptr]; }
            }
        }

    {
        hexdump(&elf_mirror[symtab], sizeof(Elf64_Sym));

        uint32_t ht_entry_count;
        memcpy(&ht_entry_count, dyn_ht, sizeof(uint32_t));
        printf("A %d\n", ht_entry_count);

        // first symbol is NULL symbol
        Elf64_Sym* syms = (Elf64_Sym*) &elf_mirror[symtab];
        for (size_t j = 1; j < ht_entry_count; j++) {
            printf("SYM[%zu] %s\n", j, &strtab[syms[j].st_name]);
        }
    }
    fault_handler();
*/

#include <common.h>
#include <beans.h>
#include <elf.h>

typedef int (*ThreadEntry)(KHandle arg);

static const char text[] = "Hello\n";

void* memcpy(void* dest, const void* src, size_t n) {
    u8* d = (u8*)dest;
    u8* s = (u8*)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dest;
}

static void* load_elf(KHandle file_handle) {
    return NULL;
}

void _start(KHandle file_handle, KHandle aux_handle) {
    size_t file_size = vmo_get_size(file_handle);
    char* contents   = mem_map(NULL_HANDLE, 0, file_handle, 0, file_size, PROT_RW, 0);

    Elf64_Ehdr* elf_header = (Elf64_Ehdr*) contents;
    size_t segment_size = elf_header->e_phentsize;
    size_t segment_header_bounds = elf_header->e_phoff + elf_header->e_phnum*segment_size;
    if (segment_header_bounds >= file_size) {
        thread_exit(1);
    }

    uintptr_t lo = 0, hi = 0;
    size_t page_size = 4096;
    size_t total_memsz = 0;
    const char* segments = contents + elf_header->e_phoff;
    for (size_t i = 0; i < elf_header->e_phnum; i++) {
        Elf64_Phdr* segment = (Elf64_Phdr*) (segments + i*segment_size);
        if (segment->p_type != PT_LOAD) {
            continue;
        }

        size_t mem_size    = (segment->p_memsz + page_size - 1) & -page_size;
        uintptr_t vaddr    = segment->p_vaddr & -page_size;
        uintptr_t vaddr_hi = vaddr + mem_size;

        uintptr_t offset   = segment->p_vaddr & (page_size - 1);
        total_memsz = (segment->p_memsz + offset + page_size - 1) & -page_size;

        if (lo > vaddr)    { lo = vaddr; }
        if (hi < vaddr_hi) { hi = vaddr_hi; }
    }

    // Create environment
    KHandle section_vmo = vmo_create(total_memsz);

    // Place ELF into env
    size_t curr_pos = 0;
    char* elf_vmap  = mem_map_private(NULL_HANDLE, hi - lo, PROT_RW, 0);
    for (size_t i = 0; i < elf_header->e_phnum; i++) {
        Elf64_Phdr* segment = (Elf64_Phdr*) (segments + i*segment_size);
        if (segment->p_type == PT_DYNAMIC) {
            /* Elf64_Dyn* dyns = (Elf64_Dyn*) &contents[segment->p_offset];
            size_t dyn_count = segment->p_memsz / sizeof(Elf64_Dyn);

            for (size_t j = 0; j < dyn_count; j++) {
                printf("DYN[%#zx] %p\n", dyns[j].d_tag, dyns[j].d_ptr);

                if (dyns[j].d_tag == DT_HASH)   { dyn_ht = &elf_mirror[dyns[j].d_ptr]; }
                if (dyns[j].d_tag == DT_SYMTAB) { symtab = dyns[j].d_ptr; }
                if (dyns[j].d_tag == DT_SYMENT) { syment = dyns[j].d_ptr; }
                if (dyns[j].d_tag == DT_STRTAB) { strtab = &elf_mirror[dyns[j].d_ptr]; }
            } */
        } else if (segment->p_type == PT_LOAD) {
            uintptr_t vaddr  = (segment->p_vaddr & -page_size) - lo;
            uintptr_t offset = segment->p_vaddr & (page_size - 1);
            uintptr_t memsz  = segment->p_memsz + offset;
            memsz = (memsz + page_size - 1) & -page_size;

            char* dst = mem_map(NULL_HANDLE, elf_vmap + vaddr, section_vmo, curr_pos, memsz, PROT_RW, 0);
            if (segment->p_filesz > 0) {
                memcpy(&dst[offset], &contents[segment->p_offset], segment->p_filesz);
            }

            curr_pos += memsz;
        }
    }

    {
        KHandle vmo  = vmo_create(4*1024);
        char* buffer = mem_map(NULL_HANDLE, 0, vmo, 0, 4*1024, PROT_RW, 0);
        for (int i = 0; i < sizeof(text); i++) {
            buffer[i] = text[i];
        }

        sys_debug_log(vmo, sizeof(text) - 1);
    }

    ThreadEntry fn = (ThreadEntry) (elf_vmap + (elf_header->e_entry - lo));
    int code = fn(aux_handle);

    thread_exit(0);
}

