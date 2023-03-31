
static inline void writec(char c) {
    com_writec(c);
    term_printc(c);
}

static inline void writes(char* str) {
    while(*str) {
        writec(*str++);
    }
}

static inline void writex32(uint32_t num) {
    const char hex[] = "0123456789abcdef";
    char buf[16] = {0};
    int idx = sizeof buf - 1;
    for(int i = 0; i != 8; ++i) {
        uint8_t digit = num % 16;
        num /= 16;
        buf[--idx] = hex[digit];
    }
    char* str = &buf[idx];
    writec('0');
    writec('x');
    writes(str);
}

static inline void writex64(uint64_t num) {
    const char hex[] = "0123456789abcdef";
    char buf[32] = {0};
    int idx = sizeof buf - 1;
    for(int i = 0; i != 16; ++i) {
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
            switch(*fmt) {
                case 'x': {
                    uint32_t arg = va_arg(args, uint32_t);
                    writex32(arg);
                } break;
                case 'X': {
                    uint64_t arg = va_arg(args, uint64_t);
                    writex64(arg);
                } break;
                case 'd': {
                    int32_t arg = va_arg(args, int32_t);
                    writed64((int64_t)arg);
                } break;
                case 'D': {
                    int64_t arg = va_arg(args, int64_t);
                    writed64(arg);
                } break;
                case 'u': {
                    uint32_t arg = va_arg(args, uint32_t);
                    writeu64((uint64_t)arg);
                } break;
                case 'U': {
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