#include <common.h>
#include <kernel.h>

#include "printf.h"

int itoa(u64 i, uint8_t *buf, u8 base) {
    static const char bchars[] = "0123456789ABCDEF";

    int      pos   = 0;
    int      o_pos = 0;
    int      top   = 0;
    u8 tbuf[64];

    if (i == 0 || base > 16) {
        buf[0] = '0';
        buf[1] = '\0';
        return 2;
    }

    while (i != 0) {
        tbuf[pos] = bchars[i % base];
        pos++;
        i /= base;
    }
    top = pos--;

    for (o_pos = 0; o_pos < top; pos--, o_pos++) {
        buf[o_pos] = tbuf[pos];
    }

    buf[o_pos] = 0;
    return o_pos + 1;
}

#define LOG_TYPES         \
X(LOG_INFO, 0x0,  "INFO") \
X(LOG_BOOT, 0x1,  "BOOT") \
X(LOG_TIME, 0x2,  "TIME") \
X(LOG_MEM,  0x3,  "MEM ") \
X(LOG_PCI,  0x4,  "PCI ") \
X(LOG_NET,  0x5,  "NET ")

static const char *log_typenames[] = {
    #define X(tag, id, name) [id] = name,
    LOG_TYPES
        #undef X
};
typedef enum {
    #define X(tag, id, name) tag = id,
    LOG_TYPES
        #undef X
} LogType;

enum {
    LOG_BUFFER_SIZE = 4*1024
};

typedef struct LogBuffer {
    size_t used;
    char data[LOG_BUFFER_SIZE - sizeof(size_t)];
} LogBuffer;

static Lock print_lock;
static LogBuffer log_buffer[2];

// Buffered kernel printing
void kprintf(const char* fmt, ...) {
    LogBuffer* b = &log_buffer[0];
    spin_lock(&print_lock);
    for (;;) {

        __builtin_va_list args;
        __builtin_va_start(args, fmt);
        int len = vsnprintf(&b->data[b->used], LOG_BUFFER_SIZE - b->used, fmt, args);
        __builtin_va_end(args);

        if (b->used + len > LOG_BUFFER_SIZE) {
            // buffer is full, let's flush then retry
            for (int i = 0; i < b->used; i++) {
                _putchar(b->data[i]);
            }
            b->used = 0;
            continue;
        }

        b->used += len;
        spin_unlock(&print_lock);
        return;
    }
}

