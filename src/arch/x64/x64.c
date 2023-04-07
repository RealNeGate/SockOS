#pragma once

#include <stdint.h>
#include <cpuid.h>
#include <common.h>

typedef struct CPUState {
    // u8  fxsave[512 + 16];
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rdi, rsi, rbp, rbx, rdx, rcx, rax;
    u64 interrupt_num, error;
    u64 rip, cs, flags, rsp, ss;
} CPUState;

static inline u64 x86_get_cr2(void) {
    u64 result;
    asm volatile ("movq %%cr2, %q0" : "=a" (result));
    return result;
}

static inline PageTable* x86_get_cr3(void) {
    u64 result;
    asm volatile ("movq %%cr3, %q0" : "=a" (result));
    return (PageTable*) result;
}

// IO Ports on x86
static inline u8 io_in8(u16 port) {
    u8 value;
    asm volatile("inb %w1, %b0" : "=a" (value) : "Nd" (port));
    return value;
}

static inline u16 io_in16(u16 port) {
    u16 value;
    asm volatile("inw %w1, %w0" : "=a" (value) : "Nd" (port));
    return value;
}

static inline u32 io_in32(u16 port) {
    u32 value;
    __asm__ volatile("inl %w1, %0" : "=a" (value) : "Nd" (port));
    return value;
}

static inline void io_out8(u16 port, u8 value) {
    asm volatile("outb %b0, %w1" : : "a" (value), "Nd" (port));
}

static inline void io_out16(u16 port, u16 value) {
    asm volatile("outw %w0, %w1" : : "a" (value), "Nd" (port));
}

static inline void io_out32(u16 port, u32 value) {
    asm volatile("outl %0, %w1" : : "a" (value), "Nd" (port));
}

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

static bool has_cpu_support(void) {
	u32 eax, ebx, ecx, edx;
	__get_cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
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
	__get_cpuid(0x80000002, &eax, &ebx, &ecx, &edx);
	cpuid_regcpy(str, eax, ebx, ecx, edx);
	__get_cpuid(0x80000003, &eax, &ebx, &ecx, &edx);
	cpuid_regcpy(str+16, eax, ebx, ecx, edx);
	__get_cpuid(0x80000004, &eax, &ebx, &ecx, &edx);
	cpuid_regcpy(str+32, eax, ebx, ecx, edx);
	return;
}
