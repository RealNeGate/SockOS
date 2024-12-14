#include <common.h>

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

typedef struct {
    uint8_t *data;
    uint64_t ts;
    uint16_t pos;
    uint16_t type;
} __attribute__((packed)) LogEntry;

typedef struct {
    _Atomic uint64_t top;

    LogEntry *entries;
    size_t entry_count;
} Ring;

static Lock print_lock;
static Ring print_ring;

#define MAX_LOG_LEN 128
void print_ring_init(uint8_t *buffer, size_t size) {
    size_t ideal_entry_count = (size / MAX_LOG_LEN);
    size_t overhead = ideal_entry_count * sizeof(LogEntry);
    size_t real_size = size - overhead;
    size_t entry_count = (real_size / MAX_LOG_LEN);

    print_ring.entries = (LogEntry *)buffer;
    print_ring.entry_count = entry_count;
    uint8_t *data_start = buffer + (sizeof(LogEntry) * entry_count);
    for (int i = 0; i < entry_count; i++) {
        print_ring.entries[i].data = data_start + (i * MAX_LOG_LEN);
        print_ring.entries[i].ts = 0;
        print_ring.entries[i].pos = 0;
    }
}

static void write_char(LogEntry *e, char c) {
    if (e->pos > MAX_LOG_LEN) {
        return;
    }

    e->data[e->pos++] = c;
}

static void write_buffer(LogEntry *e, char *buf, int size) {
    if (size >= 0) {
        for (int i = 0; i < size; i++) write_char(e, buf[i]);
    } else {
        for (int i = 0; buf[i]; i++) write_char(e, buf[i]);
    }
}

void kprintf(const char *fmt, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, fmt);

    uint64_t top_slot = atomic_fetch_add(&print_ring.top, 1);
    LogEntry *entry = &print_ring.entries[top_slot % print_ring.entry_count];
    entry->ts = get_time_ticks();

    int base = 10;
    u32 min_len = 0;
    for (const char *c = fmt; *c != 0; c++) {
        if (*c != '%') {
            for (; *c != 0 && *c != '%';) {
                write_char(entry, *c);
                c++;
            }

            c--;
            continue;
        }

        i64 precision = -1;
        consume_moar:
        c++;
        switch (*c) {
            case '.': {
                c++;
                if (*c != '*') {
                    continue;
                }

                precision = __builtin_va_arg(args, i64);
                goto consume_moar;
            } break;
            case '0': {
                c++;
                min_len = *c - '0';
                goto consume_moar;
            } break;
            case 'c': {
                i64 c = __builtin_va_arg(args, i64);
                write_char(entry, c);
            } break;
            case 's': {
                char *s = __builtin_va_arg(args, char *);
                write_buffer(entry, s, precision);
            } break;
            case 'z': {
                goto consume_moar;
            } break;
            case 'l': {
                goto consume_moar;
            } break;

            case 'd':
            case 'u': {
                base = 10;
                goto print_num;
            } break;

            case 'b': {
                base = 2;
                goto print_num;
            } break;

            case 'x': {
                base = 16;
print_num:
                u64 i = __builtin_va_arg(args, u64);

                u8 tbuf[64];
                int sz = itoa(i, tbuf, base);

                int pad_sz = min_len - (sz - 1);
                while (pad_sz > 0) {
                    write_char(entry, '0');
                    pad_sz--;
                }

                write_buffer(entry, (char *)tbuf, sz - 1);
                min_len = 0;
            } break;

            case 'p': {
                u64 i = __builtin_va_arg(args, u64);

                u8 tbuf[64];
                int sz = itoa(i, tbuf, 16);
                int pad_sz = 16 - (sz - 1);
                write_char(entry, '0');
                write_char(entry, 'x');
                while (pad_sz > 0) {
                    write_char(entry, '0');
                    pad_sz--;
                }

                write_buffer(entry, (char *)tbuf, sz - 1);
                min_len = 0;
            } break;
        }
    }
    __builtin_va_end(args);

    // Dump entry to serial
    spin_lock(&print_lock);

    u8 tbuf[64];
    int sz = itoa(entry->ts, tbuf, 10);
    for (int i = 0; i < sz - 1; i++) {
        put_char(tbuf[i]);
    }
    put_char(' ');

/*
    put_char('[');
    const char *type_name = log_typenames[entry->type];
    for (int i = 0; i < 4; i++) {
        put_char(type_name[i]);
    }
    put_char(']');
    put_char(' ');
*/
    
    for (int i = 0; i < entry->pos; i++) {
        put_char(entry->data[i]);
    }

    entry->pos = 0;
    spin_unlock(&print_lock);
}
