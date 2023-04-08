#pragma once

#include <stdint.h>
#include <common.h>

typedef struct CPUState {
    // u8  fxsave[512 + 16];
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rdi, rsi, rbp, rbx, rdx, rcx, rax;
    u64 interrupt_num, error;
    u64 rip, cs, flags, rsp, ss;
} CPUState;

static inline void x86_invalidate_page(unsigned long addr) {
    asm volatile("invlpg [%0]" ::"r" (addr) : "memory");
}

static inline u64 x86_get_cr2(void) {
    u64 result;
    asm volatile ("mov %q0, cr2" : "=a" (result));
    return result;
}

static inline PageTable* x86_get_cr3(void) {
    u64 result;
    asm volatile ("mov %q0, cr3" : "=a" (result));
    return (PageTable*) result;
}

// IO Ports on x86
u8 io_in8(u16 port);
u16 io_in16(u16 port);
u32 io_in32(u16 port);

void io_out8(u16 port, u8 value);
void io_out16(u16 port, u8 value);
void io_out32(u16 port, u8 value);

static inline void io_wait(void) {
    asm volatile(
        "jmp 1f;"
        "1:jmp 1f;"
        "1:"
    );
}

static inline u64 __readmsr(u32 r) {
    u32 edx, eax;
    asm volatile ("rdmsr" : "=d"(edx), "=a"(eax) : "c"(r));
    return (((u64) edx) << 32) | (u64) eax;
}

static inline void __writemsr(u32 r, u64 v) {
    u32 eax = v & 0xffffffff;
    u32 edx = v >> 32;
    asm volatile ("wrmsr" : : "c" (r), "a" (eax), "d" (edx));
}

static inline void x86_cli() {
    asm volatile ("cli");
}

static inline void x86_sti() {
    asm volatile ("sti");
}

static inline void x86_hlt() {
    asm volatile ("hlt");
}

static inline void halt() {
    x86_cli();
    for(;;) {
        x86_hlt();
    }
}

static inline int x86_get_cpuid(unsigned int leaf, unsigned int *eax, unsigned int *ebx, unsigned int *ecx, unsigned int *edx) {
    asm volatile ("cpuid" : "=a" (*eax), "=b" (*ebx), "=c" (*ecx), "=d" (*edx) : "0" (leaf));
    return 1;
}

static bool has_cpu_support(void) {
    u32 eax, ebx, ecx, edx;
    x86_get_cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
    // Do we have at least the constant TSC page?
    if (eax < 0x80000007) {
        return false;
    }

    return true;
}

static void cpuid_regcpy(char *buf, u32 eax, u32 ebx, u32 ecx, u32 edx) {
    memcpy(buf + 0,  (char *)&eax, 4);
    memcpy(buf + 4,  (char *)&ebx, 4);
    memcpy(buf + 8,  (char *)&ecx, 4);
    memcpy(buf + 12, (char *)&edx, 4);
}

static void get_cpu_str(char *str) {
    u32 eax, ebx, ecx, edx;
    x86_get_cpuid(0x80000002, &eax, &ebx, &ecx, &edx);
    cpuid_regcpy(str, eax, ebx, ecx, edx);
    x86_get_cpuid(0x80000003, &eax, &ebx, &ecx, &edx);
    cpuid_regcpy(str+16, eax, ebx, ecx, edx);
    x86_get_cpuid(0x80000004, &eax, &ebx, &ecx, &edx);
    cpuid_regcpy(str+32, eax, ebx, ecx, edx);
    return;
}
