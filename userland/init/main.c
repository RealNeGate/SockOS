#include <beans.h>
#include <common.h>
#include <elf.h>
#include "../kernel/printf.c"

void* memset(void* buffer, int c, size_t n) {
    u8* buf = (u8*)buffer;
    for (size_t i = 0; i < n; i++) {
        buf[i] = c;
    }
    return buffer;
}

void* memcpy(void* dest, const void* src, size_t n) {
    u8* d = (u8*)dest;
    u8* s = (u8*)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dest;
}

void* memmove(void* dest, const void* src, size_t n) {
    u8* d = (u8*)dest;
    u8* s = (u8*)src;
    if (s+n >= d) {
        for (size_t i = n; i--;) {
            d[i] = s[i];
        }
    } else {
        for (size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    }
    return dest;
}

// Just build it as part of the bigger unit
#define LZ4_memset(dst, src, n) memset(dst, src, n)
#define LZ4_memcpy(dst, src, n) memcpy(dst, src, n)
#define LZ4_memmove(dst, src, n) memmove(dst, src, n)
#define LZ4_FREESTANDING 1
#include <lz4.c>

typedef struct {
    uint32_t data_len;
    uint32_t unpacked_len;
    char path[24];
    char data[];
} FileEntry;

typedef struct {
    uint32_t key;
    size_t path_len;
    const char* path;
} DriverEntry;

static DriverEntry drivers[256];

static KHandle log_stream;
static char* log_buffer;
static int log_used;

int strcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) {
        a++, b++;
    }
    return *a - *b;
}

void fault_handler(void) {
    if (log_stream && log_used) {
        syscall(SYS_debug_log, log_stream, log_used);
        log_used = 0;
    }
}

void _putchar(char ch) {
    if (log_stream == 0) {
        log_stream = syscall(SYS_vmo_create, 4*1024);
        log_buffer = mem_map(NULL_HANDLE, 0, log_stream, 0, 4*1024, PROT_RW, 0);
    } else if (log_used == 4096) {
        syscall(SYS_debug_log, log_stream, log_used);
        log_used = 0;
    }

    log_buffer[log_used++] = ch;
}

static int parse_hexchar(int ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    } else if (ch >= 'A' && ch <= 'F') {
        return (ch - 'A') + 0xA;
    } else if (ch >= 'a' && ch <= 'f') {
        return (ch - 'a') + 0xA;
    } else {
        return 0;
    }
}

static uint32_t lowbias32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

static bool put_driver(DriverEntry e) {
    uint32_t first = lowbias32(e.key) & 255, i = first;
    do {
        if (drivers[i].key == 0) {
            drivers[i] = e;
            return true;
        } else if (drivers[i].key == e.key) {
            return false;
        }
        i = (i + 1) & 255;
    } while (first != i);
    return false;
}

static DriverEntry* get_driver(uint32_t key) {
    uint32_t first = lowbias32(key) & 255, i = first;
    do {
        if (drivers[i].key == 0) {
            return NULL;
        } else if (drivers[i].key == key) {
            return &drivers[i];
        }
        i = (i + 1) & 255;
    } while (first != i);
    return NULL;
}

static bool parse_driver_list(FileEntry* file) {
    char* src = mem_map_private(NULL_HANDLE, file->unpacked_len, PROT_RW, 0);
    int res = LZ4_decompress_safe(file->data, src, file->data_len, file->unpacked_len);
    if (file->unpacked_len != res) {
        return false;
    }

    int state = 0;
    while (*src) {
        // skip whitespace
        while (*src == ' ' || *src == '\n') { src++; }

        // skip comments
        if (*src == '#') {
            while (*src) {
                char in = *src++;
                if (in == '\n') { break; }
            }
            continue;
        } else if (*src == 0) {
            break;
        }

        // Vendor:Device Path
        uint32_t key = 0;
        while (*src && *src != ' ') {
            char in = *src++;
            // ignore, used for spacing
            if (in == ':') { continue; }
            key = (key << 4) | parse_hexchar(in);
        }

        // skip whitespace
        while (*src == ' ') { src++; }

        // skip til EOL
        const char* name = src;
        while (*src && *src != '\n') { src++; }

        printf("[init] register driver: %#x %.*s\n", key, (int) (src - name), name);
        put_driver((DriverEntry){ key, src - name, name });
    }
    return true;
}

static bool string_match(const char* a, const char* b, size_t n) {
    while (*a && *b && n--) {
        if (*a != *b) { return false; }
        a++, b++;
    }
    return true;
}

static FileEntry* find_file(FileEntry* initrd, size_t path_len, const char* path) {
    while (initrd->path[0]) {
        if (string_match(initrd->path, path, path_len)) {
            return initrd;
        }

        size_t padded_len = (initrd->data_len + 16) & -16ull;
        initrd = (FileEntry*) (((char*) initrd) + sizeof(FileEntry) + padded_len);
    }
    return NULL;
}

static bool exec(FileEntry* file, KHandle arg) {
    if (file->unpacked_len < sizeof(Elf64_Ehdr)) {
        return false;
    }

    char* contents = mem_map_private(NULL_HANDLE, file->unpacked_len, PROT_RW, 0);
    int res = LZ4_decompress_safe(file->data, contents, file->data_len, file->unpacked_len);
    if (file->unpacked_len != res) {
        return false;
    }

    Elf64_Ehdr* elf_header = (Elf64_Ehdr*) contents;
    size_t segment_size = elf_header->e_phentsize;
    size_t segment_header_bounds = elf_header->e_phoff + elf_header->e_phnum*segment_size;
    if (segment_header_bounds >= file->unpacked_len) {
        printf("[init] error: segments do not fit into file\n");
        return false;
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

    printf("Total Mem %zu\n", total_memsz);
    fault_handler();

    // Create environment
    KHandle child_env = syscall(SYS_env_create);
    KHandle section_vmo = syscall(SYS_vmo_create, total_memsz);

    enum {
        DT_FLAGS_1 = 0x6ffffffb,
        DT_DEBUG   = 21,
        DT_SYMTAB  = 6,
        DT_SYMENT  = 11,
        DT_STRTAB  = 5,
        DT_STRSZ   = 10,
        DT_GNUHASH = 0x6ffffef5,
    };

    // Elf dynamic state
    size_t symtab = 0, syment = 0;
    char* strtab = NULL;

    // Place ELF into env
    size_t curr_pos = 0;
    char* elf_vmap   = mem_map_private(child_env, hi - lo, PROT_RW, 0);
    char* elf_mirror = mem_map_private(NULL_HANDLE, hi - lo, PROT_NONE, 0);
    for (size_t i = 0; i < elf_header->e_phnum; i++) {
        Elf64_Phdr* segment = (Elf64_Phdr*) (segments + i*segment_size);
        if (segment->p_type == PT_DYNAMIC) {
            Elf64_Dyn* dyns = (Elf64_Dyn*) &contents[segment->p_offset];
            size_t dyn_count = segment->p_memsz / sizeof(Elf64_Dyn);

            for (size_t j = 0; j < dyn_count; j++) {
                printf("DYN[%#zx] %p\n", dyns[j].d_tag, dyns[j].d_ptr);

                if (dyns[j].d_tag == DT_SYMTAB) { symtab = dyns[j].d_ptr; }
                if (dyns[j].d_tag == DT_SYMENT) { syment = dyns[j].d_ptr; }
                if (dyns[j].d_tag == DT_STRTAB) { strtab = &elf_mirror[dyns[j].d_ptr]; }
            }
        } else if (segment->p_type == PT_LOAD) {
            uintptr_t vaddr  = (segment->p_vaddr & -page_size) - lo;
            uintptr_t offset = segment->p_vaddr & (page_size - 1);
            uintptr_t memsz  = segment->p_memsz + offset;
            memsz = (memsz + page_size - 1) & -page_size;

            // Mirror into current env
            char* dst = mem_map(NULL_HANDLE, elf_mirror + vaddr, section_vmo, curr_pos, memsz, PROT_RW, 0);
            if (segment->p_filesz > 0) {
                memcpy(&dst[offset], &contents[segment->p_offset], segment->p_filesz);
            }

            // Map into child env
            mem_map(child_env, elf_vmap + vaddr, section_vmo, curr_pos, memsz, PROT_RW, 0);
            curr_pos += memsz;
        }
    }

    {
        for (int i = 0; i < 10; i++) {
            printf("A %c %d\n", elf_mirror[symtab + i], elf_mirror[symtab + i]);
        }

        Elf64_Sym* syms = (Elf64_Sym*) &elf_mirror[symtab];
        printf("SYM %p\n", syms);
        for (size_t j = 0; j < 1; j++) {
            printf("SYM[%#zx] %p\n", j, syms[j].st_name);
        }
    }
    fault_handler();

    // TODO(NeGate): unmap and revoke our own rights over the
    // child env to avoid later leaks of resources.
    // ...

    // Spin up the main thread
    void* fn = elf_vmap + (elf_header->e_entry - lo);
    KHandle thread = thread_create(child_env, fn, arg, 2*1024*1024, THREAD_FLAGS_GRANT);
    return true;
}

int _start(KHandle bootstrap_vmo) {
    size_t initrd_size = vmo_get_size(bootstrap_vmo);
    FileEntry* initrd  = mem_map(NULL_HANDLE, 0, bootstrap_vmo, 0, initrd_size, PROT_RW, 0);

    // Scan the drivers.txt, construct hashmap for driver mappings
    printf("InitRD:\n");
    fault_handler();

    for (FileEntry* file = initrd; file->path[0];) {
        printf("[init] found file '%s' (%zu bytes)\n", file->path, file->data_len);
        fault_handler();

        if (strcmp(file->path, "/drivers.txt") == 0) {
            parse_driver_list(file);
        }

        // Advance files
        size_t padded_len = (file->data_len + 16) & -16ull;
        file = (FileEntry*) (((char*) file) + sizeof(FileEntry) + padded_len);
    }

    // Find the first set of connected PCI devices
    for (int i = 0;; i++) {
        uint32_t key;
        KHandle dev = syscall(SYS_pci_peek_device, i, &key);
        if (dev == RESULT_NO_HANDLE) { break; }

        DriverEntry* driver = get_driver(key);
        if (driver != NULL) {
            FileEntry* file = find_file(initrd, driver->path_len, driver->path);
            if (file != NULL) {
                printf("[init] PCI Device matched! %04x:%04x    %.*s\n", key >> 16u, key & 0xFFFF, (int) driver->path_len, driver->path);
                exec(file, dev);
            }
        }
    }

    // Launch the desktop server, this will handle I/O relevant to
    // visualizing and interfacing with a UI.
    FileEntry* stdlib = find_file(initrd, sizeof("/stdlib.so")-1, "/stdlib.so");
    if (stdlib != NULL) {
        printf("Found desktop! %zu bytes\n", stdlib->data_len);
        exec(stdlib, 0);
    }

    fault_handler();

    KHandle mailbox = syscall(SYS_root_mailbox);
    static KHandle names[256];

    // Process messages
    KHandle handle;
    uint64_t args[2], msg[4];
    uint64_t info = mailbox_wait(mailbox, sizeof(msg) << 32u, args, msg, &handle);
    for (;;) {
        fault_handler();

        int ret = 0;
        switch (info & 0xFFFF) {
            // Register into names
            case 0: {
                names[args[0]] = handle;
                printf("Register! %d %p\n", args[0], handle);
                fault_handler();
                break;
            }

            // Retrieve from names
            case 1: {
                handle = names[args[0]];
                printf("Get! %d %p\n", args[0], handle);
                fault_handler();
                break;
            }
        }
        // reply and wait for the next message
        info = mailbox_reply(mailbox, (sizeof(msg) << 32u) | (ret & 0xFFFF), args, msg, &handle);
    }
}

