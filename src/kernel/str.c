static void* memset(void* buffer, int c, size_t n) {
    uint8_t* buf = (uint8_t*)buffer;
    for (size_t i = 0; i < n; i++) {
        buf[i] = c;
    }
    return (void*)buf;
}

static void* memcpy(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    uint8_t* s = (uint8_t*)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return (void*)dest;
}

static int memcmp(const void* a, const void* b, size_t n) {
    uint8_t* aa = (uint8_t*)a;
    uint8_t* bb = (uint8_t*)b;

    for (size_t i = 0; i < n; i++) {
        if (aa[i] != bb[i]) return aa[i] - bb[i];
    }

    return 0;
}

static bool memeq(const void* a, const void* b, size_t n) {
    uint8_t* aa = (uint8_t*)a;
    uint8_t* bb = (uint8_t*)b;

    for (size_t i = 0; i < n; i++) {
        if (aa[i] != bb[i]) return false;
    }

    return true;
}

