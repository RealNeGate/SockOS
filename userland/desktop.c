#include <stdint.h>
#include <stddef.h>

#include "syscall_helper.h"

typedef unsigned int KHandle;

enum {
    // sleep(micros)
    SYS_sleep            = 0,
    // mmap(vmo, offset, size)
    SYS_mmap             = 1,
    // thread_create(start, arg)
    SYS_thread_create    = 2,
    // test()
    SYS_test             = 3,
    // pci_claim_device()
    SYS_pci_claim_device = 4,
};

void foo(void* arg) {
    for (;;) {
        syscall(SYS_test);
    }
}

void bar(void* arg) {
    for (;;) {
        syscall(SYS_test);
        syscall(SYS_sleep, 6*1000);
    }
}

void baz(void* arg) {
    for (;;) {
        syscall(SYS_test);
        syscall(SYS_sleep, 3*1000);
    }
}

int _start(KHandle bootstrap_channel) {
    uint32_t* pixels = (uint32_t*) syscall(SYS_mmap, 1, 0, 800 * 600 * sizeof(uint32_t));

    // uint32_t video_devs[] = { 0x12341111 };
    // int eth_pci = syscall(SYS_pci_claim_device, 1, video_devs);

    #if 0
    for (int i = 0; i < 4; i++) {
        syscall(SYS_thread_create, foo, NULL);
    }
    for (int i = 0; i < 4; i++) {
        syscall(SYS_thread_create, bar, NULL);
    }
    for (int i = 0; i < 4; i++) {
        syscall(SYS_thread_create, baz, NULL);
    }
    #endif

    uint8_t mult = 0;
    int width = 800, height = 600, stride = 800;
    for (;;) {
        uint64_t gradient_x = (width + 255) / 256;
        uint64_t gradient_y = (height + 255) / 256;

        for (size_t j = 0; j < height; j++) {
            uint32_t g = ((j / gradient_y) / 2) + 0x7F;
            g = (g + mult) & 0xFF;

            for (size_t i = 0; i < width; i++) {
                uint32_t b = ((i / gradient_x) / 2) + 0x7F;
                b = (b + mult) & 0xFF;

                pixels[i + (j * stride)] = 0xFF000000 | (g << 16) | (b << 8);
            }
        }

        syscall(SYS_sleep, 16*1000);
        mult += 1;
    }
    return 0;
}
