#include <beans.h>
#include <common.h>

int _start(KHandle bootstrap_vmo) {
    /* uint64_t info[4];
    int fb_bar = syscall(SYS_fb_grab, info);
    uint32_t* fb = (uint32_t*) syscall(SYS_mmap, fb_bar, 0, info[3]); */

    for (;;) {
        syscall(SYS_test, 0xAA);
        syscall(SYS_sleep, 100000);
    }

    return 0;
}
