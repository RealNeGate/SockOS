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

int strcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) {
        a++, b++;
    }
    return *a - *b;
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

static void fault_handler(void) {
    if (log_stream && log_used) {
        sys_debug_log(log_stream, log_used);
        log_used = 0;
    }
}

void _putchar(char ch) {
    if (log_stream == 0) {
        log_stream = vmo_create(4*1024);
        log_buffer = mem_map(NULL_HANDLE, 0, log_stream, 0, 4*1024, PROT_RW, 0);
    } else if (log_used == 4096) {
        sys_debug_log(log_stream, log_used);
        log_used = 0;
    }

    log_buffer[log_used++] = ch;
}

#undef assert // lz4.c
#define assert(cond) if (!(cond)) { assert_msg(#cond); }
static void assert_msg(const char* str) {
    printf("assert condition failed! %s\n", str);
    fault_handler();
    *((volatile int*) 0) = 0;
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

static void hexdump(const char* buf, size_t len) {
    printf("DUMP %p %zu\n", buf, len);

    size_t rows = (len + 15) / 16;
    for (int j = 0; j < rows; j++) {
        for (int i = 0; i < 16; i++) {
            int ch = buf[j*16 + i];
            printf("%02x ", ch);
        }

        for (int i = 0; i < 16; i++) {
            int ch = buf[j*16 + i];
            if (ch >= 32) {
                _putchar(ch);
            } else if (ch == 0) {
                _putchar('.');
            } else {
                printf("\x1b[96m.\x1b[0m");
            }
        }
        printf("\n");
    }
}

typedef struct {
    KHandle vmo;
    size_t len;
    char* contents;
} OpenFile;

// create VMO from unpacked file
static bool open_file(FileEntry* file, OpenFile* out) {
    out->vmo = vmo_create(file->unpacked_len);
    out->len = file->unpacked_len;
    out->contents = mem_map(NULL_HANDLE, 0, out->vmo, 0, out->len, PROT_RW, 0);
    return LZ4_decompress_safe(file->data, out->contents, file->data_len, out->len) == out->len;
}

static bool spawn_process(OpenFile* file, KHandle arg0, KHandle arg1) {
    if (file->len < sizeof(Elf64_Ehdr)) {
        return false;
    }

    Elf64_Ehdr* elf_header = (Elf64_Ehdr*) file->contents;
    size_t segment_size = elf_header->e_phentsize;
    size_t segment_header_bounds = elf_header->e_phoff + elf_header->e_phnum*segment_size;
    if (segment_header_bounds >= file->len) {
        printf("[init] error: segments do not fit into file\n");
        return false;
    }

    KHandle child_env = syscall0(SYS_env_create);
    KHandle elf_vmo = file->vmo;

    // Place ELF into env
    size_t page_size = 4096;
    const char* segments = file->contents + elf_header->e_phoff;
    for (size_t i = 0; i < elf_header->e_phnum; i++) {
        Elf64_Phdr* segment = (Elf64_Phdr*) (segments + i*segment_size);
        if (segment->p_type == PT_LOAD) {
            // check segment permissions
            assert(segment->p_filesz <= segment->p_memsz && "no enough space in memory for file data");
            assert((segment->p_align & (segment->p_align - 1)) == 0 && "alignment is not a power-of-two");

            uintptr_t vaddr  = segment->p_vaddr & -page_size;
            size_t offset    = segment->p_offset & -page_size;
            size_t file_size = (segment->p_filesz + page_size - 1) & -page_size;
            size_t mem_size  = (segment->p_memsz  + page_size - 1) & -page_size;

            if (segment->p_filesz > 0) {
                mem_map(child_env, (void*) vaddr, elf_vmo, offset, file_size, PROT_RW, 0);
            }
            if (file_size < mem_size) {
                mem_map(child_env, (void*) (vaddr + file_size), NULL_HANDLE, 0, mem_size - file_size, PROT_RW, 0);
            }
        }
    }

    // TODO(NeGate): unmap and revoke our own rights over the
    // child env to avoid later leaks of resources.
    // ...

    // Spin up the main thread
    KHandle thread = thread_create(child_env, (void*) elf_header->e_entry, arg0, arg1, 2*1024*1024, THREAD_FLAGS_GRANT0 | THREAD_FLAGS_GRANT1);
    return true;
}

int _start(KHandle bootstrap_vmo, UTCB* utcb) {
    static char dummy_tls[64];

    utcb->tls_addr = dummy_tls;
    utcb->self     = utcb;

    size_t initrd_size = vmo_get_size(bootstrap_vmo);
    FileEntry* initrd  = mem_map(NULL_HANDLE, 0, bootstrap_vmo, 0, initrd_size, PROT_RW, 0);

    // Scan the drivers.txt, construct hashmap for driver mappings
    printf("InitRD:\n");
    fault_handler();

    bool has_ld_so = false;
    OpenFile ld_so = { 0 };
    for (FileEntry* file = initrd; file->path[0];) {
        printf("[init] found file '%s' (%zu bytes)\n", file->path, file->data_len);
        fault_handler();

        if (strcmp(file->path, "/drivers.txt") == 0) {
            parse_driver_list(file);
        } else if (strcmp(file->path, "/ld.so") == 0) {
            has_ld_so = open_file(file, &ld_so);
        }

        // Advance files
        size_t padded_len = (file->data_len + 16) & -16ull;
        file = (FileEntry*) (((char*) file) + sizeof(FileEntry) + padded_len);
    }

    if (!has_ld_so) {
        printf("[init] Where's ld.so?\n");
        fault_handler();
        thread_exit(1);
    }

    // Find the first set of connected PCI devices
    for (int i = 0;; i++) {
        uint32_t key;
        KHandle dev = syscall2(SYS_pci_peek_device, i, (uint64_t) &key);
        if (dev == RESULT_NO_HANDLE) { break; }

        DriverEntry* driver = get_driver(key);
        if (driver != NULL) {
            FileEntry* file = find_file(initrd, driver->path_len, driver->path);

            OpenFile driver_exec;
            if (file != NULL && open_file(file, &driver_exec)) {
                printf("[init] PCI Device matched! %04x:%04x    %.*s\n", key >> 16u, key & 0xFFFF, (int) driver->path_len, driver->path);
                spawn_process(&ld_so, driver_exec.vmo, dev);
            }
        }
    }
    fault_handler();

    KHandle mailbox = get_root_mailbox();
    utcb = get_utcb();

    static KHandle names[256];

    // Process messages
    KHandle from;
    MSG_Tag tag = mailbox_wait(mailbox, &from);
    for (;;) {
        fault_handler();

        uintptr_t ret[2];
        switch (tag.cmd) {
            // Register into names
            case 0: {
                size_t index = utcb->mr[0];
                names[index] = utcb->hr[0];
                tag = msg_tag(0, 0, 0, 0);

                printf("Register! %d %p\n", index, utcb->hr[0]);
                fault_handler();
                break;
            }

            // Retrieve from names
            case 1: {
                size_t index = utcb->mr[0];
                utcb->hr[0] = names[index];
                tag = msg_tag(0, 1, 0, 0);

                printf("Get! %d %p\n", index, utcb->hr[0]);
                fault_handler();
                break;
            }
        }

        // reply and wait for the next message
        tag = mailbox_reply(mailbox, from, tag, &from, ret[0], ret[1]);
    }
}

