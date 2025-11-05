#include <beans.h>
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

void _putchar(char ch) {
    if (log_stream == 0) {
        log_stream = syscall(SYS_vmo_create, 4*1024);
        log_buffer = mmap(log_stream, 0, 4*1024);
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

static void exec(FileEntry* file) {
    // TODO(NeGate): maybe VMOs should be able to be split into subviews...
    KHandle file_vmo = syscall(SYS_vmo_create, file->data_len);
    char* dst = mmap(log_stream, 0, file->data_len);
    for (size_t i = 0; i < file->data_len; i++) {
        dst[i] = file->data[i];
    }

    // Create process
    KHandle child_env = syscall(SYS_env_create);

    // Load dynamic loader ELF

    // Spin up the main thread
    KHandle thread = syscall(SYS_thread_create, child_env, NULL, file_vmo, 2*1024*1024, 1);
}

int _start(KHandle bootstrap_vmo) {
    size_t initrd_size = vmo_get_size(bootstrap_vmo);
    FileEntry* initrd = mmap(bootstrap_vmo, 0, initrd_size);

    // Scan the drivers.txt, construct hashmap for driver mappings
    printf("InitRD:\n", initrd->path);
    for (FileEntry* file = initrd; initrd->path[0];) {
        printf("[init] found file '%s' (%zu bytes)\n", file->path, file->data_len);
        if (strcmp(file->path, "/drivers.txt") == 0) {
            parse_driver_list(file->data);

            // Advance files
            size_t padded_len = (file->data_len + 16) & -16ull;
            file = (FileEntry*) (((char*) file) + sizeof(FileEntry) + padded_len);
        }
    }

    // Find the first set of connected PCI devices
    int dev_count = syscall(SYS_pci_device_count);
    for (int i = 0; i < dev_count; i++) {
        uint32_t key;
        KHandle dev = syscall(SYS_pci_claim_device, i, &key);

        DriverEntry* driver = get_driver(key);
        if (driver != NULL) {
            printf("PCI Device matched with driver! %04x:%04x\n", key >> 16u, key & 0xFFFF);

            FileEntry* file = find_file(initrd, driver->path_len, driver->path);
            exec(file);
        }
    }

    syscall(SYS_debug_log, log_stream, log_used);
    log_used = 0;

    for (;;) {
        syscall(SYS_test, 0);
        syscall(SYS_sleep, 100000);
    }
}

