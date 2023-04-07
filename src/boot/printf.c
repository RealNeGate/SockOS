
static inline void writec(char c) {
    com_writec(c);
    term_printc(c);
}

static inline void writes(char* str) {
    while(*str) {
        writec(*str++);
    }
}

static inline void writex32(u32 num, size_t width) {
    const char hex[] = "0123456789abcdef";
    char buf[16] = {0};
    int idx = sizeof buf - 1;
    for(int i = 0; i != 8; ++i) {
        if(i == width) {
            break;
        }
        u8 digit = num % 16;
        num /= 16;
        buf[--idx] = hex[digit];
    }
    char* str = &buf[idx];
    writec('0');
    writec('x');
    writes(str);
}

static inline void writex64(u64 num, size_t width) {
    const char hex[] = "0123456789abcdef";
    char buf[32] = {0};
    int idx = sizeof buf - 1;
    for(int i = 0; i != 16; ++i) {
        if(i == width) {
            break;
        }
        u8 digit = num % 16;
        num /= 16;
        buf[--idx] = hex[digit];
    }
    char* str = &buf[idx];
    writec('0');
    writec('x');
    writes(str);
}

static inline void writed64(i64 dec) {
    if(dec < 0) {
        writec('-');
        dec = -dec;
    }
    char buf[32];
    int idx = sizeof buf - 1;
    do {
        int digit = dec % 10;
        dec /= 10;
        buf[--idx] = digit + '0';
    } while(dec != 0);
    char* str = &buf[idx];
    writes(str);
}

static inline void writeu64(u64 dec) {
    char buf[32];
    int idx = sizeof buf - 1;
    do {
        int digit = dec % 10;
        dec /= 10;
        buf[--idx] = digit + '0';
    } while(dec != 0);
    char* str = &buf[idx];
    writes(str);
}

static inline void vprintf(char* fmt, va_list args) {
    while(*fmt) {
        if(*fmt != '%') {
            writec(*fmt);
        }
        else {
            fmt += 1;
            size_t width = 0xff;
            if('0' <= *fmt && *fmt <= '9') {
                width = *fmt - '0';
                fmt += 1;
            }
            switch(*fmt) {
                case 'x': {
                    if(width == 0xff) {
                        width = 8;
                    }
                    u32 arg = va_arg(args, u32);
                    writex32(arg, width);
                } break;
                case 'X': {
                    if(width == 0xff) {
                        width = 16;
                    }
                    u64 arg = va_arg(args, u64);
                    writex64(arg, width);
                } break;
                case 'd': {
                    if(width == 0xff) {
                        width = 1;
                    }
                    i32 arg = va_arg(args, i32);
                    writed64((i64)arg);
                } break;
                case 'D': {
                    if(width == 0xff) {
                        width = 1;
                    }
                    i64 arg = va_arg(args, i64);
                    writed64(arg);
                } break;
                case 'u': {
                    if(width == 0xff) {
                        width = 32;
                    }
                    u32 arg = va_arg(args, u32);
                    writeu64((u64)arg);
                } break;
                case 'U': {
                    if(width == 0xff) {
                        width = 32;
                    }
                    u64 arg = va_arg(args, u64);
                    writeu64(arg);
                } break;
                case 'c': {
                    char ch = va_arg(args, int);
                    writec(ch);
                } break;
                case 's': {
                    char* str = va_arg(args, char*);
                    writes(str);
                } break;
            }
        }
        fmt += 1;
    }
}

static void printf(char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

static bool isprint(char c) {
    return 0x20 <= c && c < 0x7f;
}

static void memdump(void* addr, size_t count) {
    u8* bytes = addr;
    size_t row_size = 8;
    size_t nrows = (count + row_size - 1) / row_size;
    size_t last_row_size = (count % row_size == 0) ? row_size : (count % row_size);
    for(size_t row = 0; row < nrows; ++row) {
        for(size_t col = 0; col != row_size; ++col) {
            size_t i = col + row*row_size;
            if(row == nrows-1 && col >= last_row_size) {
                printf("     ");
            }
            else {
                u8 byte = bytes[i];
                printf("%2x ", (u32)byte);
            }
        }
        printf("| ");
        for(size_t col = 0; col != row_size; ++col) {
            size_t i = col + row*row_size;
            if(row == nrows-1 && col >= last_row_size) {
                printf(".");
            }
            else {
                if(isprint(bytes[i])) {
                    printf("%c", bytes[i]);
                }
                else {
                    printf(".");
                }
            }
        }
        writec('\n');
    }
}
