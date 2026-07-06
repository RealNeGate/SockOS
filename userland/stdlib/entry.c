#include <common.h>
#include <beans.h>

static const char text[] = "Hello\n";

#define EXPORT_FN __attribute__((visibility("default")))

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

void _start(KHandle handle) {
    KHandle vmo  = syscall(SYS_vmo_create, 0, 4*1024);
    char* buffer = mmap(0, vmo, 0, 4*1024, PROT_READ | PROT_WRITE, 0);
    for (int i = 0; i < sizeof(text); i++) {
        buffer[i] = text[i];
    }

    syscall(SYS_debug_log, vmo, sizeof(text) - 1);
    thread_exit(0);
}

