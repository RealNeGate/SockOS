#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

enum {
    IA32_APIC_BASE = 0x1B,
    IA32_APIC_BASE_MSR_BSP = 0x100, // Processor is a BSP

    // LAPIC interrupts
    INTR_LAPIC_TIMER      = 0xF0,
    INTR_LAPIC_SPURIOUS   = 0xF1,
    INTR_LAPIC_IPI        = 0xF2,
    INTR_LAPIC_RESCHEDULE = 0xF3
};

typedef struct CPUState {
    // uint8_t  fxsave[512 + 16];
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t interrupt_num, error;
    uint64_t rip, cs, flags, rsp, ss;
} CPUState;

typedef struct __attribute__((packed)) IDTEntry {
    uint16_t offset_1;  // offset bits 0..15
    uint16_t selector;  // a code segment selector in GDT or LDT
    uint8_t  ist;       // bits 0..2 holds Interrupt Stack Table offset, rest of bits zero.
    uint8_t  type_attr; // type and attributes
    uint16_t offset_2;  // offset bits 16..31
    uint32_t offset_3;  // offset bits 32..63
    uint32_t reserved;  // reserved
} IDTEntry;
_Static_assert(sizeof(IDTEntry) == 16, "expected sizeof(IDTEntry) to be 16 bytes");

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} IDT;
_Static_assert(sizeof(IDT) == 10, "expected sizeof(IDT) to be 10 bytes");

volatile IDTEntry _idt[256];

volatile uint32_t* apic;
#define APIC(reg_num) apic[(reg_num) >> 2]

// this is where all interrupts get pointed to, from there it's redirected to irq_int_handler
extern void isr_handler(void);

extern void irq_enable(IDT* idt);
extern void irq_disable(void);
extern void isr3(void);

// IO Ports on x86
extern uint8_t io_in8(uint16_t port);
extern uint16_t io_in16(uint16_t port);
extern uint32_t io_in32(uint16_t port);
extern void io_out8(uint16_t port, uint8_t value);
extern void io_out16(uint16_t port, uint16_t value);
extern void io_out32(uint16_t port, uint32_t value);
extern void io_wait(void);
extern void __writemsr(uint32_t r, uint32_t v);

static uint64_t __readmsr(uint32_t r) {
    uint32_t edx, eax;
    asm volatile ("rdmsr" : "=d"(edx), "=a"(eax) : "c"(r));
    return (((uint64_t) edx) << 32) | (uint64_t) eax;
}

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

#define SET_INTERRUPT(num) do {                         \
    extern void isr ## num();                           \
    uintptr_t callback_addr = (uintptr_t) &isr ## num;  \
    _idt[num] = (IDTEntry){                             \
        .offset_1 = callback_addr & 0xFFFF,             \
        .selector = 0x08,                               \
        .ist = 0,                                       \
        .type_attr = 0x8E,                              \
        .offset_2 = (callback_addr >> 16) & 0xFFFF,     \
        .offset_3 = (callback_addr >> 32) & 0xFFFFFFFF, \
        .reserved = 0,                                  \
    };                                                  \
} while (0)

#define SET_EXCEPTION(num) do {                         \
    extern void isr ## num();                           \
    uintptr_t callback_addr = (uintptr_t) &isr ## num;  \
    _idt[num] = (IDTEntry){                             \
        .offset_1 = callback_addr & 0xFFFF,             \
        .selector = 0x08,                               \
        .ist = 0,                                       \
        .type_attr = 0x8F,                              \
        .offset_2 = (callback_addr >> 16) & 0xFFFF,     \
        .offset_3 = (callback_addr >> 32) & 0xFFFFFFFF, \
        .reserved = 0,                                  \
    };                                                  \
} while (0)

void irq_startup(void) {
    FOREACH_N(i, 0, 256) _idt[i] = (IDTEntry){ 0 };
    SET_INTERRUPT(3);
    SET_EXCEPTION(8);
    SET_INTERRUPT(9);
    SET_EXCEPTION(13);
    SET_EXCEPTION(14);

    SET_INTERRUPT(32);
    SET_INTERRUPT(33);
    SET_INTERRUPT(34);
    SET_INTERRUPT(35);
    SET_INTERRUPT(36);
    SET_INTERRUPT(37);
    SET_INTERRUPT(38);
    SET_INTERRUPT(39);
    SET_INTERRUPT(40);
    SET_INTERRUPT(41);
    SET_INTERRUPT(42);
    SET_INTERRUPT(43);
    SET_INTERRUPT(44);
    SET_INTERRUPT(45);
    SET_INTERRUPT(46);
    SET_INTERRUPT(47);

    irq_disable_pic();

    // Enable APIC
    if (1) {
        uint64_t x = __readmsr(IA32_APIC_BASE);
        x |= (1u << 11u); // enable APIC
        __writemsr(IA32_APIC_BASE, x);
        if (x & IA32_APIC_BASE_MSR_BSP) {
            kprintf("We're the main core\n");
        }

        // get the address (it's above the 63-12 bits)
        if (memmap__view(boot_info->kernel_pml4, x & ~0xFFF, PAGE_SIZE, (void**) &apic)) {
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
        APIC(0x380) = 10000;
    }

    // asm volatile ("1: jmp 1b");
    IDT idt = { .limit = sizeof(_idt) - 1, .base = (uintptr_t)_idt };
    irq_enable(&idt);
}

void irq_int_handler(CPUState* state) {
    kprintf("int %d: %x\n", state->interrupt_num, state->error);
    #if 0
    kprintf("  rip=%x:%x rsp=%x:%x\n  ", state->cs, state->rip, state->ss, state->rsp, state->flags);

    uint8_t* mem = (uint8_t*) state->rip;
    for (int i = -2; i <= 2; i++) {
        kprintf("%x ", mem[i]);
    }
    kprintf("\n");
    #endif

    // APIC interrupts require an EOI signal to be sent before
    // another one can come in, it's a little write only register
    // in the LAPIC.
    if (state->interrupt_num == 32) {
        apic[0xB0 / 4] = 0;
    }
}
