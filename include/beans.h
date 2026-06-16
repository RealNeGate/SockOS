#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef unsigned int KHandle;

// memory mapping prot
enum {
    PROT_NONE  = 0,
    PROT_READ  = 1,
    PROT_WRITE = 2,
    PROT_EXEC  = 4,
};

// memory mapping flags
enum {
    // reserves address space
    MEM_PLACEHOLDER = 8,
    // addr isn't treated as a hint, if we can't allocate at that location
    // then we'll fail. Note that explicitly mapping over placeholder memory
    // will always succeed.
    MEM_FIXED       = 16,
};

enum {
    RESULT_SUCCESS   =  0,

    RESULT_NO_MEM    = -1,
    RESULT_BAD_ALIGN = -2,

    // PCI errors
    RESULT_NO_BAR    = -3, // the BAR you're reading isn't there (OOB?)
    RESULT_IO_BAR    = -4, // you tried to map an I/O BAR into memory

    // Handle errors
    RESULT_NO_HANDLE    = -5, // Handle was 0
    RESULT_WRONG_HANDLE = -6, // Type mismatch

    // Mailbox errors
    RESULT_PACKET_TOO_BIG = -7,
};

typedef enum {
    #define X(name, ...) SYS_ ## name,
    #include "syscall_table.h"

    SYS_MAX,
} SyscallNum;

#ifndef KERNEL_LAND
#include "syscall_helper.h"

// info { len:48 fn:15 handle:1 }
// mailbox_send (box, info, arg0, arg1, body, handle)
//
// mailbox_reply(box, info, arg0, arg1, body, handle)
// mailbox_wait (box, info, arg0, arg1, body, handle)
//                          ^^^^^^^^^^
//                          out params
//
//                0    1     2     3     4     5
static uint64_t mailbox_send(KHandle mailbox, uint64_t info, uint64_t arg0, uint64_t arg1, void* ptr, KHandle* handle) {
    return syscall(SYS_mailbox_send, mailbox, info, arg0, arg1, ptr, handle);
}

static uint64_t mailbox_reply(KHandle mailbox, uint64_t info, uint64_t args[2], void* ptr, KHandle* handle) {
    return __syscall6_out2(SYS_mailbox_reply, mailbox, info, &args[0], &args[1], (long) ptr, handle);
}

static uint64_t mailbox_wait(KHandle mailbox, uint64_t info, uint64_t args[2], void* ptr, KHandle* handle) {
    return __syscall6_out2(SYS_mailbox_wait, mailbox, info, &args[0], &args[1], (long) ptr, handle);
}

static KHandle vmo_create(size_t size)  { return syscall(SYS_vmo_create, size); }
static size_t vmo_get_size(KHandle vmo) { return syscall(SYS_vmo_get_size, vmo); }

static void* mmap(KHandle env, KHandle vmo, uintptr_t addr, size_t size, uint32_t flags, size_t offset) { return (void*) syscall(SYS_mmap, env, vmo, addr, size, flags, offset); }
static void* mpin(KHandle vmo, size_t offset, size_t size, uintptr_t* out_paddr) { return (void*) syscall(SYS_mpin, vmo, offset, size, out_paddr); }
#endif
