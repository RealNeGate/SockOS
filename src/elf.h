#pragma once

#define EI_MAG0       0
#define EI_MAG1       1
#define EI_MAG2       2
#define EI_MAG3       3
#define EI_CLASS      4  /* Class of machine. */
#define EI_DATA       5  /* Data format. */
#define EI_VERSION    6  /* ELF format version. */
#define EI_OSABI      7  /* Operating system / ABI identification */
#define EI_ABIVERSION 8  /* ABI version */
#define OLD_EI_BRAND  8  /* Start of architecture identification. */
#define EI_PAD        9  /* Start of padding (per SVR4 ABI). */
#define EI_NIDENT     16 /* Size of e_ident array. */

/* Values for e_type. */
#define ET_NONE   0      /* Unknown type. */
#define ET_REL    1      /* Relocatable. */
#define ET_EXEC   2      /* Executable. */
#define ET_DYN    3      /* Shared object. */
#define ET_CORE   4      /* Core file. */
#define ET_LOOS   0xfe00 /* First operating system specific. */
#define ET_HIOS   0xfeff /* Last operating system-specific. */
#define ET_LOPROC 0xff00 /* First processor-specific. */
#define ET_HIPROC 0xffff /* Last processor-specific. */

/* Values for e_machine. */
#define EM_NONE    0   /* Unknown machine. */
#define EM_X86_64  62  /* Advanced Micro Devices x86-64 */
#define EM_AARCH64 183 /* AArch64 (64-bit ARM) */

/* sh_type */
#define SHT_NULL     0 /* inactive */
#define SHT_PROGBITS 1 /* program defined information */
#define SHT_SYMTAB   2 /* symbol table section */
#define SHT_STRTAB   3 /* string table section */
#define SHT_RELA     4 /* relocations w/addends */
#define SHT_NOBITS   8 /* no space section */
#define SHT_REL      9 /* relocations wo/addends */

/* Flags for sh_flags. */
#define SHF_WRITE            0x1        /* Section contains writable data. */
#define SHF_ALLOC            0x2        /* Section occupies memory. */
#define SHF_EXECINSTR        0x4        /* Section contains instructions. */
#define SHF_MERGE            0x10       /* Section may be merged. */
#define SHF_STRINGS          0x20       /* Section contains strings. */
#define SHF_INFO_LINK        0x40       /* sh_info holds section index. */
#define SHF_LINK_ORDER       0x80       /* Special ordering requirements. */
#define SHF_OS_NONCONFORMING 0x100      /* OS-specific processing required. */
#define SHF_GROUP            0x200      /* Member of section group. */
#define SHF_TLS              0x400      /* Section contains TLS data. */
#define SHF_MASKOS           0x0ff00000 /* OS-specific semantics. */
#define SHF_MASKPROC         0xf0000000 /* Processor-specific semantics. */

/* Values for p_flags. */
#define	PF_X		0x1        /* Executable. */
#define	PF_W		0x2        /* Writable. */
#define	PF_R		0x4        /* Readable. */
#define	PF_MASKOS   0x0ff00000 /* Operating system-specific. */
#define	PF_MASKPROC 0xf0000000 /* Processor-specific. */

/* Values for p_type. */
#define PT_NULL      0	/* Unused entry. */
#define PT_LOAD      1	/* Loadable segment. */
#define PT_DYNAMIC   2	/* Dynamic linking information segment. */
#define PT_INTERP    3	/* Pathname of interpreter. */
#define PT_NOTE      4	/* Auxiliary information. */
#define PT_SHLIB     5	/* Reserved (not used). */
#define PT_PHDR      6	/* Location of program header itself. */
#define PT_TLS       7	/* Thread local storage segment */

#define PT_LOOS 0x60000000      /* OS-specific */
#define PT_GNU_RELRO (PT_LOOS + 0x474e552)

/* Values for relocation */
#define R_X86_64_NONE     0
#define R_X86_64_64       1
#define R_X86_64_PC32     2
#define R_X86_64_GOT32    3
#define R_X86_64_PLT32    4
#define R_X86_64_RELATIVE 8

typedef u64 Elf64_Addr;
typedef u16 Elf64_Half;
typedef u64 Elf64_Off;
typedef i32  Elf64_Sword;
typedef i64  Elf64_Sxword;
typedef u32 Elf64_Word;
typedef u64 Elf64_Lword;
typedef u64 Elf64_Xword;

/* Macros for accessing the fields of st_info. */
#define ELF64_ST_BIND(info) ((info) >> 4)
#define ELF64_ST_TYPE(info) ((info)&0xf)

// http://web.mit.edu/freebsd/head/sys/sys/elf64.h
// https://cirosantilli.com/elf-hello-world#minimal-elf-file
// https://en.wikipedia.org/wiki/Executable_and_Linkable_Format
#define EI_NIDENT 16

#define ELF64_R_SYM(i)     ((i) >> 32)
#define ELF64_R_TYPE(i)    ((i)&0xffffffffL)
#define ELF64_R_INFO(s, t) (((s) << 32) + ((t)&0xffffffffL))

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf64_Half    e_type;
    Elf64_Half    e_machine;
    Elf64_Word    e_version;
    Elf64_Addr    e_entry;
    Elf64_Off     e_phoff;
    Elf64_Off     e_shoff;
    Elf64_Word    e_flags;
    Elf64_Half    e_ehsize;
    Elf64_Half    e_phentsize;
    Elf64_Half    e_phnum;
    Elf64_Half    e_shentsize;
    Elf64_Half    e_shnum;
    Elf64_Half    e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    Elf64_Word  sh_name;
    Elf64_Word  sh_type;
    Elf64_Xword sh_flags;
    Elf64_Addr  sh_addr;
    Elf64_Off   sh_offset;
    Elf64_Xword sh_size;
    Elf64_Word  sh_link;
    Elf64_Word  sh_info;
    Elf64_Xword sh_addralign;
    Elf64_Xword sh_entsize;
} Elf64_Shdr;

// Program header for ELF64.
typedef struct {
    Elf64_Word  p_type;   // Type of segment
    Elf64_Word  p_flags;  // Segment flags
    Elf64_Off   p_offset; // File offset where segment is located, in bytes
    Elf64_Addr  p_vaddr;  // Virtual address of beginning of segment
    Elf64_Addr  p_paddr;  // Physical addr of beginning of segment (OS-specific)
    Elf64_Xword p_filesz; // Num. of bytes in file image of segment (may be zero)
    Elf64_Xword p_memsz;  // Num. of bytes in mem image of segment (may be zero)
    Elf64_Xword p_align;  // Segment alignment constraint
} Elf64_Phdr;

typedef struct {
    Elf64_Word    st_name;
    unsigned char st_info;
    unsigned char st_other;
    Elf64_Half    st_shndx;
    Elf64_Addr    st_value;
    Elf64_Xword   st_size;
} Elf64_Sym;

typedef struct {
    Elf64_Addr   r_offset;
    Elf64_Xword  r_info;
    Elf64_Sxword r_addend;
} Elf64_Rela;

typedef struct {
    Elf64_Addr  r_offset;
    Elf64_Xword r_info;
} Elf64_Rel;
