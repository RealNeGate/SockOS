
#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

enum {
    IA32_APIC_BASE = 0x1B,
    IA32_APIC_BASE_MSR_BSP = 0x100, // Processor is a BSP

    // syscall related MSRs
    IA32_EFER  = 0xC0000080,
    IA32_STAR  = 0xC0000081,
    IA32_LSTAR = 0xC0000082,
    IA32_KERNEL_GS_BASE = 0xC0000102,

    // LAPIC interrupts
    INTR_LAPIC_TIMER      = 0xF0,
    INTR_LAPIC_SPURIOUS   = 0xF1,
    INTR_LAPIC_IPI        = 0xF2,
    INTR_LAPIC_RESCHEDULE = 0xF3
};

typedef struct __attribute__((packed)) IDTEntry {
    u16 offset_1;  // offset bits 0..15
    u16 selector;  // a code segment selector in GDT or LDT
    u8  ist;       // bits 0..2 holds Interrupt Stack Table offset, rest of bits zero.
    u8  type_attr; // type and attributes
    u16 offset_2;  // offset bits 16..31
    u32 offset_3;  // offset bits 32..63
    u32 reserved;  // reserved
} IDTEntry;
_Static_assert(sizeof(IDTEntry) == 16, "expected sizeof(IDTEntry) to be 16 bytes");

typedef struct __attribute__((packed)) {
    u16 limit;
    u64 base;
} IDT;
_Static_assert(sizeof(IDT) == 10, "expected sizeof(IDT) to be 10 bytes");

volatile IDTEntry _idt[256];

volatile u32* apic;
#define APIC(reg_num) apic[(reg_num) >> 2]

// in irq.asm
extern void syscall_handler(void);
extern void do_context_switch(CPUState* state, PageTable* address_space);

// this is where all interrupts get pointed to, from there it's redirected to irq_int_handler
extern void isr_handler(void);

extern void irq_enable(IDT* idt);
extern void irq_disable(void);
extern void isr3(void);

volatile IDTEntry _idt[256];

CPUState new_thread_state(void* entrypoint, uintptr_t stack, size_t stack_size, bool is_user);

static void irq_disable_pic(void) {
    // set ICW1
    io_out8(PIC1_COMMAND, 0x11);
    io_out8(PIC2_COMMAND, 0x11);

    // set ICW2 (IRQ base offsets)
    io_out8(PIC1_DATA, 0x20);
    io_out8(PIC2_DATA, 0x28);

    // set ICW3
    io_out8(PIC1_DATA, 0x04);
    io_out8(PIC2_DATA, 0x02);

    // set ICW4
    io_out8(PIC1_DATA, 0x01);
    io_out8(PIC2_DATA, 0x01);

    // set OCW1 (interrupt masks) */
    io_out8(PIC1_DATA, 0xFF);
    io_out8(PIC2_DATA, 0xFF);

    // PIC mask
    io_wait();
}

static void irq_set_pit(int hz) {
    // http://www.osdever.net/bkerndev/Docs/pit.htm
    int divisor = 1193180 / hz; // Calculate our divisor
    io_out8(0x43, 0b00110100);
    io_out8(0x40, divisor & 0xFF);
    io_out8(0x40, (divisor & 0xFF00) >> 8);
}

// access MMIO registers
static volatile void* mmio_reg(volatile void* base, ptrdiff_t offset) {
    return ((volatile char*)base) + offset;
}

#define SET_INTERRUPT(num, has_error) do {              \
    extern void isr ## num();                           \
    uintptr_t callback_addr = (uintptr_t) &isr ## num;  \
    _idt[num] = (IDTEntry){                             \
        .offset_1 = callback_addr & 0xFFFF,             \
        .selector = 0x08,                               \
        .ist = 1,                                       \
        .type_attr = 0x8E,                              \
        .offset_2 = (callback_addr >> 16) & 0xFFFF,     \
        .offset_3 = (callback_addr >> 32) & 0xFFFFFFFF, \
        .reserved = 0,                                  \
    };                                                  \
} while (0)

void irq_startup(void) {
    FOREACH_N(i, 0, 256) _idt[i] = (IDTEntry){ 0 };
    SET_INTERRUPT(3,  false);
    SET_INTERRUPT(6,  false);
    SET_INTERRUPT(8,  true);
    SET_INTERRUPT(9,  false);
    SET_INTERRUPT(13, true);
    SET_INTERRUPT(14, true);
    SET_INTERRUPT(32, false);
    SET_INTERRUPT(33, false);
    SET_INTERRUPT(34, false);
    SET_INTERRUPT(35, false);
    SET_INTERRUPT(36, false);
    SET_INTERRUPT(37, false);
    SET_INTERRUPT(38, false);
    SET_INTERRUPT(39, false);
    SET_INTERRUPT(40, false);
    SET_INTERRUPT(41, false);
    SET_INTERRUPT(42, false);
    SET_INTERRUPT(43, false);
    SET_INTERRUPT(44, false);
    SET_INTERRUPT(45, false);
    SET_INTERRUPT(46, false);
    SET_INTERRUPT(47, false);

    irq_disable_pic();

    // Enable APIC
    if (1) {
        u64 x = __readmsr(IA32_APIC_BASE);
        x |= (1u << 11u); // enable APIC
        __writemsr(IA32_APIC_BASE, x);
        if (x & IA32_APIC_BASE_MSR_BSP) {
            kprintf("We're the main core\n");
        }

        // get the address (it's above the 63-12 bits) and identity map it
        apic = (void*) (x & ~0xFFF);
        if (memmap__view(boot_info->kernel_pml4, (uintptr_t) apic, (uintptr_t) apic, PAGE_SIZE, PAGE_WRITE)) {
            kprintf("Could not map view of local ACPI!!!\n");
            return;
        }

        kprintf("Found the APIC: 0x%x\n", apic);

        // 0xF0 - Spurious Interrupt Vector Register
        // Punting spurious interrupts (see PIC/CPU race condition, this is a fake interrupt)
        APIC(0xF0) |= 0x1FF;

        // 320h - LVT timer register
        //   we just want a simple periodic timer on IRQ32
        APIC(0x320) = 0x20000 | 32;
        //   timer divide reg
        APIC(0x3E0) = 0b1011;
        //   initial timer count
        APIC(0x380) = 1000000;
    }

    // enable syscall/sysret
    {
        u64 x = __readmsr(IA32_EFER);
        __writemsr(IA32_EFER, x | 1);
        // the location where syscall will throw the user to
        __writemsr(IA32_LSTAR, (uintptr_t) &syscall_handler);
        // syscall's code segment
        _Static_assert(KERNEL_CS + 8  == KERNEL_DS, "the data segment is implied to be 8 byte ahead... fix it");
        _Static_assert(0x10      + 16 == USER_CS,  "SYSRET expectations");
        _Static_assert(0x10      + 8  == USER_DS,  "SYSRET expectations");
        __writemsr(IA32_STAR, ((u64)KERNEL_CS << 32ull) | (0x10ull << 48ull));

        // we're storing the per_cpu info here, syscall will use this
        __writemsr(IA32_KERNEL_GS_BASE, (uintptr_t) &boot_info->main_cpu);
    }

    IDT idt = { .limit = sizeof(_idt) - 1, .base = (uintptr_t)_idt };
    irq_enable(&idt);
}

PageTable* irq_int_handler(CPUState* state, PageTable* old_address_space) {
    kprintf("int %d: error=%x\n", state->interrupt_num, state->error);
    kprintf("  rip=%x:%p rsp=%x:%p\n", state->cs, state->rip, state->ss, state->rsp);

    if (state->interrupt_num == 14) {
        u64 x = x86_get_cr2();
        u64 y = memmap__probe(old_address_space, x);

        kprintf("  cr2=%p (translated %p)\n", x, y);
        kprintf("  cr3=%p\n", old_address_space);
        return old_address_space;
    } else if (state->interrupt_num == 32) {
        Thread* next = threads_try_switch();
        if (next == NULL) {
            // return to kernel halting thread
            *state = new_thread_state(kernel_halt, (uintptr_t) boot_info->main_cpu.kernel_stack, KERNEL_STACK_SIZE, false);
            return boot_info->kernel_pml4;
        }

        // do thread context switch, if we changed
        if (threads_current != next) {
            // kprintf("switch: %p\n", next);

            // if we're switching, save old thread state
            if (threads_current != NULL) {
                threads_current->state = *state;
            }

            *state = next->state;
            threads_current = next;
        }

        // APIC interrupts require an EOI signal to be sent before
        // another one can come in, it's a little write only register
        // in the LAPIC.
        APIC(0xB0) = 0;

        return next->parent->address_space;
    } else {
        return old_address_space;
    }
}

PageTable* do_syscall(CPUState* state, PageTable* old_address_space) {
    kprintf("syscall%d(0x%x, 0x%x, 0x%x, 0x%x)\n", state->rax, state->rdi, state->rsi, state->rdx, state->r10);
    // kprintf("  rip=%p, rsp=%p\n", state->rcx, state->rsp);
    return old_address_space;
}

CPUState new_thread_state(void* entrypoint, uintptr_t stack, size_t stack_size, bool is_user) {
    // the stack will grow downwards.
    // the other registers are zeroed by default.
    CPUState s = {
        .rip = (uintptr_t) entrypoint, .rsp = stack + stack_size,
        .flags = 0x200, .cs = 0x08, .ss = 0x10,
    };

    if (is_user) {
        // |3 is for adding the requested privelege level (RPL) to the selectors
        s.cs = USER_CS | 3;
        s.ss = USER_DS | 3;
    }

    return s;
}
