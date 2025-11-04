#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "syscall_helper.h"

typedef unsigned int KHandle;

typedef enum {
    #define X(name, ...) SYS_ ## name,
    #include "../../src/kernel/syscall_table.h"

    SYS_MAX,
} SyscallNum;

// Wrappers for syscalls
static uint64_t mailbox_wait(KHandle mailbox, uint64_t msg[4]) {
    return syscall(SYS_mailbox_wait, mailbox, msg);
}

static uint64_t mailbox_reply(KHandle mailbox, uint64_t msg[4], uint64_t ret0, uint64_t ret1) {
    return syscall(SYS_mailbox_reply, mailbox, msg, ret0, ret1);
}
