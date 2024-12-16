#pragma once
#include "../../kernel.h"

enum {
    KERNEL_CS  = 0x08,
    KERNEL_DS  = 0x10,
    USER_DS    = 0x18,
    USER_CS    = 0x20,
    TSS        = 0x28,
};

struct CPUState {
    u8  fxsave[512 + 16];
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rdi, rsi, rbp, rbx, rdx, rcx, rax;
    u64 interrupt_num, error;
    u64 rip, cs, flags, rsp, ss;
};

typedef enum PageFlags {
    PAGE_PRESENT   = 1,
    PAGE_WRITE     = 2,
    PAGE_USER      = 4,
    PAGE_WRITETHRU = 8,
    PAGE_NOCACHE   = 16,
    PAGE_ACCESSED  = 32,
} PageFlags;

enum {
    IA32_APIC_BASE = 0x1B,
    IA32_APIC_BASE_MSR_BSP = 0x100, // Processor is a BSP

    // syscall related MSRs
    IA32_EFER  = 0xC0000080,
    IA32_STAR  = 0xC0000081,
    IA32_LSTAR = 0xC0000082,
    I32_GS_BASE = 0xC0000101,
    IA32_KERNEL_GS_BASE = 0xC0000102,

    IA32_TSC_DEADLINE = 0x6E0,

    // LAPIC interrupts
    INTR_LAPIC_TIMER      = 0xF0,
    INTR_LAPIC_SPURIOUS   = 0xF1,
    INTR_LAPIC_IPI        = 0xF2,
    INTR_LAPIC_RESCHEDULE = 0xF3,
};

#define APIC(reg_num) ((volatile uint32_t*) boot_info->lapic_base)[(reg_num) >> 2]

extern CPUState kernel_idle_state;

_Noreturn void x86_halt(void);

void x86_parse_acpi(void);
void x86_enable_apic(void);
void x86_boot_cores(void);

void x86_irq_startup(int core_id);
void x86_irq_handoff(int core_id);

void x86_send_ipi(u64 lapic_id, u64 val);

uintptr_t x86_irq_int_handler(CPUState* state, uintptr_t cr3, PerCPU* cpu);

// MSRs
u64 x86_readmsr(u32 r);
void x86_writemsr(u32 r, u64 v);

// Control regs:
//   CR2 holds the linear address accessed when a segfault occurs.
//   CR3 holds the physical address to the root page table.
u64 x86_get_cr2(void);
u64 x86_get_cr3(void);

