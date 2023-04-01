
static inline void writec(char c) {
    com_writec(c);
    term_printc(c);
}

static inline void writes(char* str) {
    while(*str) {
        writec(*str++);
    }
}

static inline void writex32(uint32_t num, size_t width) {
    const char hex[] = "0123456789abcdef";
    char buf[16] = {0};
    int idx = sizeof buf - 1;
    for(int i = 0; i != 8; ++i) {
        if(i == width) {
            break;
        }
        uint8_t digit = num % 16;
        num /= 16;
        buf[--idx] = hex[digit];
    }
    char* str = &buf[idx];
    writec('0');
    writec('x');
    writes(str);
}

static inline void writex64(uint64_t num, size_t width) {
    const char hex[] = "0123456789abcdef";
    char buf[32] = {0};
    int idx = sizeof buf - 1;
    for(int i = 0; i != 16; ++i) {
        if(i == width) {
            break;
        }
        uint8_t digit = num % 16;
        num /= 16;
        buf[--idx] = hex[digit];
    }
    char* str = &buf[idx];
    writec('0');
    writec('x');
    writes(str);
}

static inline void writed64(int64_t dec) {
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

static inline void writeu64(uint64_t dec) {
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
                    uint32_t arg = va_arg(args, uint32_t);
                    writex32(arg, width);
                } break;
                case 'X': {
                    if(width == 0xff) {
                        width = 16;
                    }
                    uint64_t arg = va_arg(args, uint64_t);
                    writex64(arg, width);
                } break;
                case 'd': {
                    if(width == 0xff) {
                        width = 1;
                    }
                    int32_t arg = va_arg(args, int32_t);
                    writed64((int64_t)arg);
                } break;
                case 'D': {
                    if(width == 0xff) {
                        width = 1;
                    }
                    int64_t arg = va_arg(args, int64_t);
                    writed64(arg);
                } break;
                case 'u': {
                    if(width == 0xff) {
                        width = 32;
                    }
                    uint32_t arg = va_arg(args, uint32_t);
                    writeu64((uint64_t)arg);
                } break;
                case 'U': {
                    if(width == 0xff) {
                        width = 32;
                    }
                    uint64_t arg = va_arg(args, uint64_t);
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
    uint8_t* bytes = addr;
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
                uint8_t byte = bytes[i];
                printf("%2x ", (uint32_t)byte);
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
