#pragma once

#include <stdint.h>
#include <stddef.h>

#include "syscall_helper.h"

typedef unsigned int KHandle;

typedef enum {
    #define X(name, ...) SYS_ ## name,
    #include "../src/kernel/syscall_table.h"

    SYS_MAX,
} SyscallNum;
