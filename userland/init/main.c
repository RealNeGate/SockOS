#include <beans.h>
#include <common.h>
#include <elf.h>
#include "../src/kernel/printf.c"

typedef struct {
    uint32_t data_len;
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

void* memset(void* buffer, int c, size_t n) {
    u8* buf = (u8*)buffer;
    for (size_t i = 0; i < n; i++) {
        buf[i] = c;
    }
    return buffer;
}

void fault_handler(void) {
    syscall(SYS_debug_log, log_stream, log_used);
    log_used = 0;
}

void _putchar(char ch) {
    if (log_stream == 0) {
        log_stream = syscall(SYS_vmo_create, 4*1024);
        log_buffer = mmap(0, log_stream, 0, 4*1024, PROT_READ | PROT_WRITE, 0);
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

static void parse_driver_list(const char* src) {
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
}

static int strcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) {
        a++, b++;
    }
    return *a - *b;
}

static int strncmp(const char* a, const char* b, size_t n) {
    while (*a && *b && *a == *b && n--) {
        a++, b++;
    }
    return *a - *b;
}

static FileEntry* find_file(FileEntry* initrd, size_t path_len, const char* path) {
    while (initrd->path[0]) {
        if (strncmp(initrd->path, path, path_len) == 0) {
            return initrd;
        }

        size_t padded_len = (initrd->data_len + 16) & -16ull;
        initrd = (FileEntry*) (((char*) initrd) + sizeof(FileEntry) + padded_len);
    }
    return NULL;
}

static bool exec(FileEntry* file) {
    if (file->data_len < sizeof(Elf64_Ehdr)) {
        return false;
    }

    // TODO(NeGate): maybe VMOs should be able to be split into subviews...
    KHandle file_vmo = syscall(SYS_vmo_create, file->data_len);
    char* dst = mmap(0, file_vmo, 0, file->data_len, PROT_READ | PROT_WRITE, 0);
    for (size_t i = 0; i < file->data_len; i++) {
        dst[i] = file->data[i];
    }

    Elf64_Ehdr* elf_header = (Elf64_Ehdr*) dst;
    size_t segment_size = elf_header->e_phentsize;
    size_t segment_header_bounds = elf_header->e_phoff + elf_header->e_phnum*segment_size;
    if (segment_header_bounds >= file->data_len) {
        printf("[init] error: segments do not fit into file\n");
        return false;
    }

    uintptr_t lo = 0, hi = 0;
    size_t page_size = 4096;
    const char* segments = dst + elf_header->e_phoff;
    for (size_t i = 0; i < elf_header->e_phnum; i++) {
        Elf64_Phdr* segment = (Elf64_Phdr*) (segments + i*segment_size);
        if (segment->p_type != PT_LOAD) {
            continue;
        }

        size_t mem_size    = (segment->p_memsz + page_size - 1) & -page_size;
        uintptr_t vaddr    = segment->p_vaddr & -page_size;
        uintptr_t vaddr_hi = vaddr + mem_size;

        if (lo > vaddr)    { lo = vaddr; }
        if (hi < vaddr_hi) { hi = vaddr_hi; }
    }

    // Create environment
    KHandle child_env = syscall(SYS_env_create);

    // Place ELF into env
    char* elf_vmap = mmap(child_env, file_vmo, 0, hi - lo, PROT_READ | PROT_WRITE | MEM_PLACEHOLDER, 0);
    for (size_t i = 0; i < elf_header->e_phnum; i++) {
        Elf64_Phdr* segment = (Elf64_Phdr*) (segments + i*segment_size);
        if (segment->p_type != PT_LOAD) {
            continue;
        }

        uintptr_t vaddr = (segment->p_vaddr & -page_size) - lo;
        mmap(child_env, file_vmo, (uintptr_t) elf_vmap + vaddr, segment->p_memsz, PROT_READ | PROT_WRITE, segment->p_offset & -page_size);
    }

    // Spin up the main thread
    KHandle thread = syscall(SYS_thread_create, child_env, elf_vmap + (elf_header->e_entry - lo), 0, 2*1024*1024, 0);

    syscall(SYS_mdump, child_env);
    // printf("[init] Loaded %p - %p!!! %p\n", elf_vmap, elf_vmap + (hi - lo) - 1, thread);

    return true;
}

int _start(KHandle bootstrap_vmo) {
    size_t initrd_size = vmo_get_size(bootstrap_vmo);
    FileEntry* initrd  = mmap(0, bootstrap_vmo, 0, initrd_size, PROT_READ | PROT_WRITE, 0);

    // Scan the drivers.txt, construct hashmap for driver mappings
    printf("InitRD:\n", initrd->path);
    for (FileEntry* file = initrd; file->path[0];) {
        printf("[init] found file '%s' (%zu bytes)\n", file->path, file->data_len);
        if (strcmp(file->path, "/drivers.txt") == 0) {
            parse_driver_list(file->data);
        }

        // Advance files
        size_t padded_len = (file->data_len + 16) & -16ull;
        file = (FileEntry*) (((char*) file) + sizeof(FileEntry) + padded_len);
    }

    // Find the first set of connected PCI devices
    int dev_count = syscall(SYS_pci_device_count);
    for (int i = 0; i < dev_count; i++) {
        uint32_t key;
        KHandle dev = syscall(SYS_pci_claim_device, i, &key);

        DriverEntry* driver = get_driver(key);
        if (driver != NULL) {
            printf("PCI Device matched with driver! %04x:%04x\n", key >> 16u, key & 0xFFFF);

            // FileEntry* file = find_file(initrd, driver->path_len, driver->path);
            // exec(file);
        }
    }

    FileEntry* desktop = find_file(initrd, sizeof("/desktop.so")-1, "/desktop.so");
    if (desktop != NULL) {
        printf("Found desktop! %zu bytes\n", desktop->data_len);
        exec(desktop);
    }

    syscall(SYS_debug_log, log_stream, log_used);
    log_used = 0;

    for (;;) {
        syscall(SYS_test, 0);
        syscall(SYS_sleep, 100000);
    }
}

