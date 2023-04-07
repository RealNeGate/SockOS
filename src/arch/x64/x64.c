#pragma once

#include <stdint.h>
#include <cpuid.h>
#include <common.h>

typedef struct CPUState {
    // uint8_t  fxsave[512 + 16];
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t interrupt_num, error;
    uint64_t rip, cs, flags, rsp, ss;
} CPUState;

static inline uint64_t x86_get_cr2(void) {
    uint64_t result;
    asm volatile ("movq %%cr2, %q0" : "=a" (result));
    return result;
}

static inline PageTable* x86_get_cr3(void) {
    uint64_t result;
    asm volatile ("movq %%cr3, %q0" : "=a" (result));
    return (PageTable*) result;
}

// IO Ports on x86
static inline uint8_t io_in8(uint16_t port) {
    uint8_t value;
    asm volatile("inb %w1, %b0" : "=a" (value) : "Nd" (port));
    return value;
}

static inline uint16_t io_in16(uint16_t port) {
    uint16_t value;
    asm volatile("inw %w1, %w0" : "=a" (value) : "Nd" (port));
    return value;
}

static inline uint32_t io_in32(uint16_t port) {
    uint32_t value;
    __asm__ volatile("inl %w1, %0" : "=a" (value) : "Nd" (port));
    return value;
}

static inline void io_out8(uint16_t port, uint8_t value) {
    asm volatile("outb %b0, %w1" : : "a" (value), "Nd" (port));
}

static inline void io_out16(uint16_t port, uint16_t value) {
    asm volatile("outw %w0, %w1" : : "a" (value), "Nd" (port));
}

static inline void io_out32(uint16_t port, uint32_t value) {
    asm volatile("outl %0, %w1" : : "a" (value), "Nd" (port));
}

static inline void io_wait(void) {
    asm volatile(
        "jmp 1f;"
        "1:jmp 1f;"
        "1:"
    );
}


static inline uint64_t __readmsr(uint32_t r) {
    uint32_t edx, eax;
    asm volatile ("rdmsr" : "=d"(edx), "=a"(eax) : "c"(r));
    return (((uint64_t) edx) << 32) | (uint64_t) eax;
}

static inline void __writemsr(uint32_t r, uint64_t v) {
    uint32_t eax = v & 0xffffffff;
    uint32_t edx = v >> 32;
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
	uint32_t eax, ebx, ecx, edx;
	__get_cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
	// Do we have at least the constant TSC page?
	if (eax < 0x80000007) {
		return false;
	}

	return true;
}

static void cpuid_regcpy(char *buf, uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx) {
	memcpy(buf + 0,  (char *)&eax, 4);
	memcpy(buf + 4,  (char *)&ebx, 4);
	memcpy(buf + 8,  (char *)&ecx, 4);
	memcpy(buf + 12, (char *)&edx, 4);
}

static void get_cpu_str(char *str) {
	uint32_t eax, ebx, ecx, edx;
	__get_cpuid(0x80000002, &eax, &ebx, &ecx, &edx);
	cpuid_regcpy(str, eax, ebx, ecx, edx);
	__get_cpuid(0x80000003, &eax, &ebx, &ecx, &edx);
	cpuid_regcpy(str+16, eax, ebx, ecx, edx);
	__get_cpuid(0x80000004, &eax, &ebx, &ecx, &edx);
	cpuid_regcpy(str+32, eax, ebx, ecx, edx);
	return;
}
