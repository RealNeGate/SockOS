void* memset(void* buffer, int c, size_t n) {
    u8* buf = (u8*)buffer;
    for (size_t i = 0; i < n; i++) {
        buf[i] = c;
    }
    return (void*)buf;
}

void* memcpy(void* dest, const void* src, size_t n) {
    u8* d = (u8*)dest;
    u8* s = (u8*)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return (void*)dest;
}

int memcmp(const void* a, const void* b, size_t n) {
    u8* aa = (u8*)a;
    u8* bb = (u8*)b;

    for (size_t i = 0; i < n; i++) {
        if (aa[i] != bb[i]) return aa[i] - bb[i];
    }

    return 0;
}

bool memeq(const void* a, const void* b, size_t n) {
    u8* aa = (u8*)a;
    u8* bb = (u8*)b;

    for (size_t i = 0; i < n; i++) {
        if (aa[i] != bb[i]) return false;
    }

    return true;
}

