#include <common.h>
#include <beans.h>
#include "../kernel/printf.c"

static const char text[] = "Hello\n";

#define EXPORT_FN __attribute__((visibility("default")))

static KHandle log_stream;
static char* log_buffer;
static int log_used;
static atomic_int log_lock;

#define SPIN_LOCK(x)   while (!atomic_compare_exchange_strong((x), &(int){ 0 }, 1)) {}
#define SPIN_UNLOCK(x) atomic_store((x), 0)

void _putchar(char ch) {
    SPIN_LOCK(&log_lock);
    if (log_stream == 0) {
        log_stream = vmo_create(4*1024);
        log_buffer = mem_map(NULL_HANDLE, 0, log_stream, 0, 4*1024, PROT_RW, 0);
    } else if (log_used == 4096) {
        sys_debug_log(log_stream, log_used);
        log_used = 0;
    }

    log_buffer[log_used++] = ch;
    SPIN_UNLOCK(&log_lock);
}

EXPORT_FN void* memset(void* buffer, int c, size_t n) {
    u8* buf = (u8*)buffer;
    for (size_t i = 0; i < n; i++) {
        buf[i] = c;
    }
    return buffer;
}

EXPORT_FN void* memcpy(void* dest, const void* src, size_t n) {
    u8* d = (u8*)dest;
    u8* s = (u8*)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dest;
}

EXPORT_FN void* memmove(void* dest, const void* src, size_t n) {
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

EXPORT_FN void _start(KHandle handle) {
    KHandle vmo  = vmo_create(4*1024);
    char* buffer = mem_map(NULL_HANDLE, 0, vmo, 0, 4*1024, PROT_RW, 0);
    for (int i = 0; i < sizeof(text); i++) {
        buffer[i] = text[i];
    }

    sys_debug_log(vmo, sizeof(text) - 1);
    thread_exit(0);
}

