// based on L4, like any good microkernel would be :p
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define NULL_HANDLE 0u
typedef unsigned int KHandle;

// memory mapping prot
enum {
    PROT_NONE = 0,

    PROT_R = 1,
    PROT_W = 2,
    PROT_X = 4,

    PROT_RW    = PROT_R | PROT_W,
    PROT_RX    = PROT_R | PROT_X,

    PROT_RXW   = PROT_R | PROT_W | PROT_X,
};

// memory mapping flags
enum {
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

    // permission errors
    RESULT_BAD_PERMISSION = -8,
};

// SYS_thread_create: flags
enum {
    // thread will spawn without being runnable,
    // thread_control(THREAD_CTRL_RESUME) is necessary to
    // start running.
    THREAD_FLAGS_SUSPEND = 1,

    // treats argument as a handle
    THREAD_FLAGS_GRANT   = 2,
};

// SYS_thread_control: request type
enum {
    THREAD_CTRL_EXIT,
    THREAD_CTRL_SLEEP,

    THREAD_CTRL_PAUSE,
    THREAD_CTRL_RESUME,

    THREAD_CTRL_SET_PRIO,
};

typedef enum {
    #define X(name, ...) SYS_ ## name,
    #include "syscall_table.h"

    SYS_MAX,
} SyscallNum;

typedef union {
    struct {
        // LSB -> MSB: R,W,X
        uintptr_t prot : 4;
        uintptr_t addr : 60;
    };
    uintptr_t raw;
} MapControl;

typedef union {
    struct {
        uint64_t untyped : 4;
        uint64_t   typed : 4;
        uint64_t   flags : 8;
        uint64_t     cmd : 48;
    };
    uint64_t raw;
} MSG_Tag;

typedef struct {
    // R/O Config
    uintptr_t error_code;
    uintptr_t exception_ip;

    // R/W Config
    uintptr_t exception_handler;

    // Message passing space (64B)
    //   First is an MSG_Tag
    _Alignas(64) uint64_t words[8];
} UTCB;

// user-land state of PCI device
typedef struct {
    uint32_t vend_prod;

    // VMOs or 0 if I/O
    int bar_count;
    size_t sizes[6];
    KHandle bars[6];
} PCI_Desc;

#ifndef KERNEL_LAND
#include "syscall_helper.h"

/*#define PP_NARG(...) PP_NARG_(__VA_ARGS__,PP_RSEQ_N())
#define PP_NARG_(...) PP_ARG_N(__VA_ARGS__)
#define PP_ARG_N(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, N,...) N
#define PP_RSEQ_N() 16,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0*/

// #define X(name, ret, ...) static ret name(__VA_ARGS__) { return CONCAT(PARAMS_, PP_NARG(__VA_ARGS__)); }
// #include "syscall_table.h"

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
    return __syscall6_out1(SYS_mailbox_send, mailbox, info, arg0, arg1, (long) ptr, handle);
}

static uint64_t mailbox_reply(KHandle mailbox, uint64_t info, uint64_t args[2], void* ptr, KHandle* handle) {
    return __syscall6_out2(SYS_mailbox_reply, mailbox, info, &args[0], &args[1], (long) ptr, handle);
}

static uint64_t mailbox_wait(KHandle mailbox, uint64_t info, uint64_t args[2], void* ptr, KHandle* handle) {
    return __syscall6_out2(SYS_mailbox_wait, mailbox, info, &args[0], &args[1], (long) ptr, handle);
}

static KHandle mailbox_create(size_t max_rqs) { return syscall(SYS_mailbox_create, max_rqs); }

static KHandle vmo_create(size_t size)  { return syscall(SYS_vmo_create, size); }
static size_t vmo_get_size(KHandle vmo) { return syscall(SYS_vmo_get_size, vmo); }

static uintptr_t get_root_mailbox(void) {
    return syscall(SYS_root_mailbox);
}

static void* mem_map(KHandle env, void* addr, KHandle vmo, size_t vmo_offset, size_t size, int prot, int flags) {
    MapControl ctrl = { .prot = prot, .addr = ((uintptr_t) addr >> 4ull) };
    return (void*) syscall(SYS_mem_map, env, ctrl.raw, vmo, vmo_offset, size, flags);
}

// the most common case of memory mapping is just allocating private pages
static void* mem_map_private(KHandle env, size_t size, int prot, int flags) {
    MapControl ctrl = { .prot = prot, .addr = 0 };
    return (void*) syscall(SYS_mem_map, env, ctrl.raw, 0, 0, size, flags);
}

static int mem_unmap(KHandle env, uintptr_t addr, size_t size) {
    return syscall(SYS_mem_unmap, env, addr, size);
}

static uintptr_t mem_translate(KHandle env, const void* addr) {
    return syscall(SYS_mem_translate, env, addr);
}

static KHandle event_create(void) {
    return syscall(SYS_event_create);
}

static int event_wait(KHandle event) {
    return syscall(SYS_event_wait, event);
}

static int event_signal(KHandle event) {
    return syscall(SYS_event_signal, event);
}

static KHandle thread_create(KHandle env, void* fn, uintptr_t arg, size_t stack_size, int flags) {
    char* stack = mem_map_private(env, stack_size, PROT_RW, 0);
    char* utcb  = mem_map_private(env, sizeof(UTCB), PROT_RW, 0);

    return syscall(SYS_thread_create, env, fn, stack+stack_size, arg, utcb, flags);
}

// variants of thread_control
static void thread_sleep(uint64_t micros) {
    syscall(SYS_thread_control, NULL_HANDLE, THREAD_CTRL_SLEEP, micros, NULL);
}

static void thread_exit(uint64_t code) {
    syscall(SYS_thread_control, NULL_HANDLE, THREAD_CTRL_EXIT, code, NULL);
}
#endif
