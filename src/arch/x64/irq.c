#define PIC1_COMMAND 0x20
#define PIC1_DATA 0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA 0xA1

enum {
    IA32_APIC_BASE = 0x1B,
};

typedef struct CPUState {
    uint8_t  fxsave[512 + 16];
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t interrupt_num, error;
    uint64_t rip, cs, flags, rsp, ss;
} CPUState;

typedef struct __attribute__((packed)) IDTEntry {
    uint16_t offset_1;  // offset bits 0..15
    uint16_t selector;  // a code segment selector in GDT or LDT
    int8_t   ist;       // bits 0..2 holds Interrupt Stack Table offset, rest of bits zero.
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

// this is where all interrupts get pointed to, from there it's redirected to irq_int_handler
extern void isr_handler(void);

extern void irq_enable(IDT* idt);
extern void irq_disable(void);

// IO Ports on x86
extern uint8_t io_in8(uint16_t port);
extern uint16_t io_in16(uint16_t port);
extern uint32_t io_in32(uint16_t port);
extern void io_out8(uint16_t port, uint8_t value);
extern void io_out16(uint16_t port, uint16_t value);
extern void io_out32(uint16_t port, uint32_t value);
extern void io_wait(void);

static uint64_t __readmsr(unsigned long r) {
    uint32_t edx, eax;
    __asm__ ("rdmsr" : "=d"(edx), "=a"(eax) : "c"(r));
    return (((uint64_t) edx) << 32) | (uint64_t) eax;
}

static void irq_remap_pic() {
    io_out8(PIC1_COMMAND, 0x11);
    io_out8(PIC2_COMMAND, 0x11);

    io_out8(PIC1_DATA, 0x20);
    io_out8(PIC2_DATA, 0x28);
    io_out8(PIC1_DATA, 0x04);
    io_out8(PIC2_DATA, 0x02);
    io_out8(PIC1_DATA, 0x01);
    io_out8(PIC2_DATA, 0x01);

    // PIC mask
    io_out8(PIC1_DATA, 0xFF);
    io_out8(PIC2_DATA, 0xFF);
    io_wait();
}

#define SET_INTERRUPT(num) do {                         \
    extern void isr ## num();                           \
    uintptr_t callback_addr = (uintptr_t) &isr ## num;  \
    _idt[num] = (IDTEntry){                             \
        .offset_1 = callback_addr & 0xFFFF,             \
        .selector = 0x08,                               \
        .ist = 0,                                       \
        .type_attr = 0x8e,                              \
        .offset_2 = (callback_addr >> 16) & 0xFFFF,     \
        .offset_3 = (callback_addr >> 32) & 0xFFFFFFFF, \
        .reserved = 0,                                  \
    };                                                  \
} while (0)

void irq_startup(void) {
    FOREACH_N(i, 0, 256) _idt[i] = (IDTEntry){ 0 };
    /*SET_INTERRUPT(9);
    SET_INTERRUPT(8);
    SET_INTERRUPT(13);
    SET_INTERRUPT(14);

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
    SET_INTERRUPT(47);*/

    // irq_remap_pic();
    // irq_set_pit(64);

    // IDT idt = { .limit = sizeof(_idt) - 1, .base = (uintptr_t) _idt };
    // irq_enable(&idt);

    // Enable APIC
    {
        uint64_t x = __readmsr(IA32_APIC_BASE);
        /*x |= (1u << 11u); // enable APIC
        __writemsr(IA32_APIC_BASE, x);*/

        // get the address (it's above the 63-12 bits)
        uintptr_t local_apic_addr = (x & ~0xFFF);
        put_number((uint32_t) local_apic_addr);
        // memmap__identity(boot_info->kernel_pml4, local_apic_addr, 1);
    }
}

CPUState* irq_int_handler(CPUState* state) {
    switch (state->interrupt_num) {
        // Timer interrupt
        case 32: break;
        default: break;
    }

    return state;
}
