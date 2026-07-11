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

static const char text[] = "Hello\n";

void _start(KHandle file_handle, KHandle aux_handle) {
    KHandle vmo  = vmo_create(4*1024);
    char* buffer = mem_map(NULL_HANDLE, 0, vmo, 0, 4*1024, PROT_RW, 0);
    for (int i = 0; i < sizeof(text); i++) {
        buffer[i] = text[i];
    }

    sys_debug_log(vmo, sizeof(text) - 1);
    thread_exit(0);
}

