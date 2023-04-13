#include <stdint.h>
#include <stddef.h>

#include "syscall_helper.h"

typedef uint64_t Handle;

enum {
    // sleep(micros)
    SYS_sleep = 0,
};

int _start(void) {
    for (;;) {
        syscall(SYS_sleep, 2000*1000);
    }
    return 0;
}
