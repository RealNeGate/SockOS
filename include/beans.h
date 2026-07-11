// based on L4, like any good microkernel would be :p
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define NULL_HANDLE 0u
typedef unsigned long KHandle;

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
    RESULT_BAD_PARAM      = -9,
    RESULT_BAD_MEMORY     = -10,
};

// SYS_thread_create: flags
enum {
    // thread will spawn without being runnable,
    // thread_control(THREAD_CTRL_RESUME) is necessary to
    // start running.
    THREAD_FLAGS_SUSPEND = 1,

    // treats argument as a handle
    THREAD_FLAGS_GRANT0  = 2,
    THREAD_FLAGS_GRANT1  = 4,
};

// SYS_thread_control: request type
enum {
    THREAD_CTRL_EXIT,
    THREAD_CTRL_SLEEP,

    THREAD_CTRL_PAUSE,
    THREAD_CTRL_RESUME,

    THREAD_CTRL_SET_PRIO,

    #ifdef __x86_64__
    THREAD_CTRL_SET_FS,
    #endif
};

// SYS_event_op: type
enum {
    EVENT_OP_WAIT,
    EVENT_OP_SIGNAL,
};

// SYS_mailbox_ipc: type
enum {
    MAILBOX_IPC_SEND = 1,
    MAILBOX_IPC_RECV = 2,
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
        uint64_t untyped   : 4;
        uint64_t   typed   : 4;
        uint64_t   strings : 3;
        uint64_t   flags   : 5;
        uint64_t     cmd   : 48;
    };
    uint64_t raw;
} MSG_Tag;

typedef struct {
    size_t send_len;
    char*  send_str;

    size_t recv_len;
    char*  recv_str;
} StringDope;

typedef struct UTCB {
    void* tls_addr;

    // since the UTCB is bound to the base of FS, this
    // allows us to read the UTCB address without READFSBASE.
    struct UTCB* self;

    // R/O Config
    uintptr_t error_code;
    uintptr_t exception_ip;

    // R/W Config
    uintptr_t exception_handler;

    // Message passing space (64B)
    //   First is an MSG_Tag
    _Alignas(64) struct {
        // Message regs
        uint64_t mr[8];

        // Handle regs
        KHandle hr[8];

        // String regs
        StringDope sr[4];
    };
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
/*#define PP_NARG(...) PP_NARG_(__VA_ARGS__,PP_RSEQ_N())
#define PP_NARG_(...) PP_ARG_N(__VA_ARGS__)
#define PP_ARG_N(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, N,...) N
#define PP_RSEQ_N() 16,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0*/

// #define X(name, ret, ...) static ret name(__VA_ARGS__) { return CONCAT(PARAMS_, PP_NARG(__VA_ARGS__)); }
// #include "syscall_table.h"

static UTCB* get_utcb(void) {
    uint64_t result;
    asm volatile ("mov %q0, fs:[8]" : "=r" (result));
    return (UTCB*) result;
}

static __inline long __syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6, long a7, long a8) {
    register long ret __asm__("rax") = n;
    register long b1  __asm__("rdi") = a1;
    register long b2  __asm__("rsi") = a2;
    register long b3  __asm__("rdx") = a3;
    register long b4  __asm__("r10") = a4;
    register long b5  __asm__("r8")  = a5;
    register long b6  __asm__("r9")  = a6;
    register long b7  __asm__("r12") = a7;
    register long b8  __asm__("r13") = a8;
    __asm__ __volatile__ ("syscall" : "=r"(ret) : "r"(ret), "r"(b1), "r"(b2), "r"(b3), "r"(b4), "r"(b5), "r"(b6), "r"(b7), "r"(b8) : "rcx", "r11", "memory");
    return ret;
}

#define syscall0(n) __syscall(n, 0, 0, 0, 0, 0, 0, 0, 0)
#define syscall1(n, a1) __syscall(n, a1, 0, 0, 0, 0, 0, 0, 0)
#define syscall2(n, a1, a2) __syscall(n, a1, a2, 0, 0, 0, 0, 0, 0)
#define syscall3(n, a1, a2, a3) __syscall(n, a1, a2, a3, 0, 0, 0, 0, 0)
#define syscall4(n, a1, a2, a3, a4) __syscall(n, a1, a2, a3, a4, 0, 0, 0, 0)
#define syscall5(n, a1, a2, a3, a4, a5) __syscall(n, a1, a2, a3, a4, a5, 0, 0, 0)
#define syscall6(n, a1, a2, a3, a4, a5, a6) __syscall(n, a1, a2, a3, a4, a5, a6, 0, 0)
#define syscall7(n, a1, a2, a3, a4, a5, a6, a7) __syscall(n, a1, a2, a3, a4, a5, a6, a7, 0)
#define syscall8(n, a1, a2, a3, a4, a5, a6, a7, a8) __syscall(n, a1, a2, a3, a4, a5, a6, a7, a8)

static MSG_Tag msg_tag(int untyped, int typed, int strs, uint64_t cmd) {
    return (MSG_Tag){ .untyped = untyped, .typed = typed, .strings = strs, .cmd = cmd };
}

static __inline long mailbox_ipc(KHandle mailbox, KHandle to, MSG_Tag tag, KHandle* from) {
    UTCB* utcb = get_utcb();

    register long ret __asm__("rax") = SYS_mailbox_ipc;
    register long b1  __asm__("rdi") = mailbox;
    register long b2  __asm__("rsi") = to;
    register long b3  __asm__("rdx") = tag.raw;
    register long b4  __asm__("r10") = 0;
    register long b5  __asm__("r8")  = utcb->mr[0];
    register long b6  __asm__("r9")  = utcb->mr[1];
    register long b7  __asm__("r12") = utcb->mr[2];
    register long b8  __asm__("r13") = utcb->mr[3];

    __asm__ __volatile__ ("syscall" : "+r"(ret), "+r"(b4), "+r"(b5), "+r"(b6), "+r"(b7), "+r"(b8) : "r"(b1), "r"(b2), "r"(b3) : "rcx", "r11", "memory");

    // writeback, technically not necessary
    if (from) { *from = b4; }
    utcb->mr[0] = b5;
    utcb->mr[1] = b6;
    utcb->mr[2] = b7;
    utcb->mr[3] = b8;
    return ret;
}

static MSG_Tag mailbox_call(KHandle mailbox, MSG_Tag tag) {
    tag.flags = MAILBOX_IPC_SEND;
    return (MSG_Tag){ .raw = mailbox_ipc(mailbox, NULL_HANDLE, tag, NULL) };
}

static MSG_Tag mailbox_send(KHandle mailbox, KHandle to, MSG_Tag tag, KHandle* from) {
    tag.flags = MAILBOX_IPC_SEND;
    return (MSG_Tag){ .raw = mailbox_ipc(mailbox, to, tag, from) };
}

static MSG_Tag mailbox_reply(KHandle mailbox, KHandle to, MSG_Tag tag, KHandle* from, uint64_t arg0, uint64_t arg1) {
    tag.flags = MAILBOX_IPC_SEND | MAILBOX_IPC_RECV;
    return (MSG_Tag){ .raw = mailbox_ipc(mailbox, to, tag, from) };
}

static __inline MSG_Tag mailbox_wait(KHandle mailbox, KHandle* from) {
    UTCB* utcb = get_utcb();
    MSG_Tag tag = { .flags = MAILBOX_IPC_RECV };

    register long ret __asm__("rax") = SYS_mailbox_ipc;
    register long b1  __asm__("rdi") = mailbox;
    register long b2  __asm__("rsi") = 0;
    register long b3  __asm__("rdx") = tag.raw;
    register long b4  __asm__("r10") = 0;
    register long b5  __asm__("r8")  = 0;
    register long b6  __asm__("r9")  = 0;
    register long b7  __asm__("r12") = 0;
    register long b8  __asm__("r13") = 0;

    __asm__ __volatile__ ("syscall" : "+r"(ret), "+r"(b4), "+r"(b5), "+r"(b6), "+r"(b7), "+r"(b8) : "r"(b1), "r"(b2), "r"(b3) : "rcx", "r11", "memory");

    // writeback, technically not necessary
    if (from) { *from = b4; }
    utcb->mr[0] = b5;
    utcb->mr[1] = b6;
    utcb->mr[2] = b7;
    utcb->mr[3] = b8;
    return (MSG_Tag){ .raw = ret };
}

static KHandle mailbox_create(void) {
    return syscall0(SYS_mailbox_create);
}

static KHandle vmo_create(size_t size)  { return syscall1(SYS_vmo_create, size); }
static size_t vmo_get_size(KHandle vmo) { return syscall1(SYS_vmo_get_size, vmo); }

static uintptr_t get_root_mailbox(void) {
    return syscall0(SYS_root_mailbox);
}

static void* mem_map(KHandle env, void* addr, KHandle vmo, size_t vmo_offset, size_t size, int prot, int flags) {
    MapControl ctrl = { .prot = prot, .addr = ((uintptr_t) addr >> 4ull) };
    return (void*) syscall6(SYS_mem_map, env, ctrl.raw, vmo, vmo_offset, size, flags);
}

// the most common case of memory mapping is just allocating private pages
static void* mem_map_private(KHandle env, size_t size, int prot, int flags) {
    MapControl ctrl = { .prot = prot, .addr = 0 };
    return (void*) syscall6(SYS_mem_map, env, ctrl.raw, 0, 0, size, flags);
}

static int mem_unmap(KHandle env, uintptr_t addr, size_t size) {
    return syscall3(SYS_mem_unmap, env, addr, size);
}

static uintptr_t mem_translate(KHandle env, const void* addr) {
    return syscall2(SYS_mem_translate, env, (long) addr);
}

static KHandle event_create(void) {
    return syscall0(SYS_event_create);
}

static int event_wait(KHandle event) {
    return syscall2(SYS_event_op, EVENT_OP_WAIT, event);
}

static int event_signal(KHandle event) {
    return syscall2(SYS_event_op, EVENT_OP_SIGNAL, event);
}

static KHandle thread_create(KHandle env, void* fn, uintptr_t arg0, uintptr_t arg1, size_t stack_size, int flags) {
    char* stack = mem_map_private(env, stack_size, PROT_RW, 0);
    UTCB* utcb  = mem_map_private(env, sizeof(UTCB), PROT_RW, 0);
    if (env == NULL_HANDLE) {
        utcb->self = utcb;
    }
    return syscall7(SYS_thread_create, env, (uint64_t) fn, (uint64_t) (stack+stack_size), arg0, arg1, (uint64_t) utcb, flags);
}

// variants of thread_control
static void thread_sleep(uint64_t micros) {
    syscall4(SYS_thread_control, NULL_HANDLE, THREAD_CTRL_SLEEP, micros, 0);
}

static void thread_exit(uint64_t code) {
    syscall4(SYS_thread_control, NULL_HANDLE, THREAD_CTRL_EXIT, code, 0);
}

static void sys_debug_log(KHandle vmo, size_t len) {
    syscall2(SYS_debug_log, vmo, len);
}
#endif
