#include <common.h>

int itoa(u64 i, u8 base, u8 *buf) {
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

void put_number(u64 number, u8 base) {
    u8 buffer[65];
    itoa(number, base, buffer);
    buffer[64] = 0;

    // serial port writing
    for (int i = 0; buffer[i]; i++) {
        put_char(buffer[i]);
    }
}

void put_string(const char* str) {
    for (; *str; str++) put_char(*str);
}

static void put_buffer(const u8* buf, int size) {
    if (size >= 0) {
        for (int i = 0; i < size; i++) put_char(buf[i]);
    } else {
        for (int i = 0; buf[i]; i++) put_char(buf[i]);
    }
}

#define _PRINT_BUFFER_LEN 128
void kprintf(const char *fmt, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, fmt);

    u8 obuf[_PRINT_BUFFER_LEN];
    u32 min_len = 0;
    for (const char *c = fmt; *c != 0; c++) {
        if (*c != '%') {
            int i = 0;
            for (; *c != 0 && *c != '%'; i++) {
                if (i > _PRINT_BUFFER_LEN) {
                    put_buffer(obuf, i);
                    i = 0;
                }

                obuf[i] = *c;
                c++;
            }

            if (i > 0) {
                put_buffer(obuf, i);
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
                put_char(c);
            } break;
            case 's': {
                u8 *s = __builtin_va_arg(args, u8 *);
                put_buffer(s, precision);
            } break;
            case 'd': {
                i64 i = __builtin_va_arg(args, i64);
                put_number(i, 10);
            } break;
            case 'z': {
                goto consume_moar;
            } break;
            case 'l': {
                goto consume_moar;
            } break;
            case 'u': {
                u64 i = __builtin_va_arg(args, u64);
                put_number(i, 10);
            } break;
            case 'x': {
                u64 i = __builtin_va_arg(args, u64);

                u8 tbuf[64];
                int sz = itoa(i, 16, tbuf);

                int pad_sz = min_len - (sz - 1);
                while (pad_sz > 0) {
                    put_char('0');
                    pad_sz--;
                }

                put_buffer(tbuf, sz - 1);
                min_len = 0;
            } break;
            case 'p': {
                u64 i = __builtin_va_arg(args, u64);
                u8 tbuf[64];
                int sz = itoa(i, 16, tbuf);
                int pad_sz = 16 - (sz - 1);
                put_char('0');
                put_char('x');
                while (pad_sz > 0) {
                    put_char('0');
                    pad_sz--;
                }
                put_buffer(tbuf, sz - 1);
                min_len = 0;
            } break;
            case 'b': {
                u64 i = __builtin_va_arg(args, u64);

                u8 tbuf[64];
                int sz = itoa(i, 2, tbuf);

                int pad_sz = min_len - (sz - 1);
                while (pad_sz > 0) {
                    put_char('0');
                    pad_sz--;
                }

                put_buffer(tbuf, sz - 1);
                min_len = 0;
            } break;
        }
    }

    __builtin_va_end(args);
}
