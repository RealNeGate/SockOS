
#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

enum {
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

    APIC_CALIBRATION_TICKS = 1000000,
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

// in irq.asm
extern void syscall_handler(void);
extern void do_context_switch(CPUState* state, PageTable* address_space);

// this is where all interrupts get pointed to, from there it's redirected to irq_int_handler
extern void isr_handler(void);

extern void irq_enable(IDT* idt);
extern void irq_disable(void);
extern void isr3(void);

static const char* interrupt_names[] = {
    #define X(id, name) [id] = name,
    #include "irq_list.h"
};

volatile IDTEntry _idt[256];
static CPUState kernel_idle_state;

static volatile bool calibrating_apic_timer;
static u64 apic_freq;

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

// access MMIO registers
static volatile void* mmio_reg(volatile void* base, ptrdiff_t offset) {
    return ((volatile char*)base) + offset;
}

static uint32_t micros_to_apic_time(uint64_t t) {
    return (t * boot_info->tsc_freq) / apic_freq;
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

// jumping into the scheduler after this
void calibrate_apic_timer(void) {
    // calibrate APIC timer
    calibrating_apic_timer = true;
    apic_freq = __rdtsc();
    APIC(0x380) = APIC_CALIBRATION_TICKS;

    while (calibrating_apic_timer) {
        asm volatile ("hlt");
    }
}

void irq_startup(int core_id) {
    if (core_id == 0) {
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
    }

    irq_disable_pic();

    // enable syscall/sysret
    {
        u64 x = __readmsr(IA32_EFER);
        __writemsr(IA32_EFER, x | 1);
        // the location where syscall will throw the user to
        __writemsr(IA32_LSTAR, (uintptr_t) &syscall_handler);
        // syscall and sysret segments
        _Static_assert(KERNEL_CS + 8  == KERNEL_DS, "SYSCALL expectations");
        _Static_assert(0x10      + 16 == USER_CS,   "SYSRET expectations");
        _Static_assert(0x10      + 8  == USER_DS,   "SYSRET expectations");
        __writemsr(IA32_STAR, ((u64)KERNEL_CS << 32ull) | (0x10ull << 48ull));

        __writemsr(IA32_KERNEL_GS_BASE, (uintptr_t) &boot_info->cores[core_id]);
        x86_swapgs();

        // we're storing the per_cpu info here, syscall will use this
        __writemsr(IA32_KERNEL_GS_BASE, (uintptr_t) &boot_info->cores[core_id]);
    }

    IDT idt;
    idt.limit = sizeof(_idt) - 1;
    idt.base = (uintptr_t)_idt;
    irq_enable(&idt);
}

void irq_begin_timer(void) {
    // Enable APIC timer
    //   0xF0 - Spurious Interrupt Vector Register
    //   Punting spurious interrupts (see PIC/CPU race condition, this is a fake interrupt)
    APIC(0xF0) |= 0x1FF;
    //   320h - LVT timer register
    //     we just want a simple one-shot timer on IRQ32
    APIC(0x320) = 0x00000 | 32;
    //   timer divide reg
    APIC(0x3E0) = 0b1011;
}

static int times = 0;
PageTable* irq_int_handler(CPUState* state, PageTable* old_address_space, PerCPU* cpu) {
    u64 now = __rdtsc();

    if (state->interrupt_num != 32) {
        kprintf("int %d: error=0x%x (%s)\n", state->interrupt_num, state->error, interrupt_names[state->interrupt_num]);
        kprintf("  rip=%x:%p rsp=%x:%p\n", state->cs, state->rip, state->ss, state->rsp);
    }

    if (state->interrupt_num == 14) {
        u64 access_addr = x86_get_cr2();

        kprintf("  cr3=%p\n\n", old_address_space);

        // print memory access address
        u64 translated;
        if (memmap__translate(old_address_space, access_addr, &translated)) {
            kprintf("  access: %p (translated: %p)\n", access_addr, translated);
        } else {
            kprintf("  access: %p (NOT PRESENT)\n", access_addr);
        }

        if (times++ == 2) {
            halt();
        }

        VMem_AddrSpace* addr_space = NULL;
        if (cpu->current_thread && cpu->current_thread->parent) {
            addr_space = &cpu->current_thread->parent->addr_space;
        }

        // update hardware page tables to match
        VMem_PTEUpdate update;
        if (addr_space != NULL && vmem_segfault(addr_space, access_addr, false, &update)) {
            // there's two events:
            //   update software PTEs => update hardware PTEs
            //
            // if we lose the CASes to write hardware PTEs but win the software ones, threads which
            // acknowledged the incorrect value will simply segfault again and update to a consistent
            // view. If the memory map update makes "backwards progress" (new form causes more segfaults,
            // thus updates can't be accomodated for in existing segfaults), we'll require TLB shootdowns
            // and an exclusive lock on the address space.
            static const uint64_t shifts[3] = { 39, 30, 21 };

            // convert software page properties into hardware page flags
            uint64_t page_flags = PAGE_WRITE;

            uint64_t virt_addr = 0;
            PageTable* curr = addr_space->hw_tables;
            for (size_t i = 0; i < 3; i++) {
                size_t index = (virt_addr >> shifts[i]) & 0x1FF;

                // the intermediate page tables need to have permissions that are "above" the child pages, so we'll OR our
                // flags with it.
                u64 entry = atomic_load_explicit(&curr->entries[index], memory_order_relaxed);
                for (;;) {
                    u64 new_entry = entry;
                    // no table? add one
                    PageTable* new_pt = NULL;
                    if ((entry & ~0x1FFull) == 0) {
                        new_pt = alloc_physical_page();
                        memset(new_pt, 0, sizeof(PageTable));

                        new_entry = ((u64) new_pt) | page_flags | PAGE_PRESENT;
                    } else {
                        // bits missing? add one
                        new_entry |= page_flags;
                    }
                    // no progress necessary, it's already behaving
                    if (entry == new_entry) { break; }
                    // transaction, if we fail at least someone allocate the
                    // physical page (so we don't spam allocations as much)
                    if (atomic_compare_exchange_strong(&curr->entries[index], &entry, new_entry)) { break; }
                    // throw away our new_pt
                    if (new_pt == NULL) { free_physical_page(new_pt); }
                }

                curr = (PageTable*) (entry & ~0x1FF);
            }

            size_t pte_index = (virt_addr >> 12) & 0x1FF; // 4KiB

            u64 old_pte = curr->entries[pte_index];
            u64 new_pte = (update.translated & 0xFFFFFFFFF000) | page_flags | PAGE_PRESENT;
            atomic_compare_exchange_strong(&curr->entries[pte_index], &old_pte, new_pte);

            x86_invalidate_page(virt_addr);
        } else {
            // dissassemble code
            if (memmap__translate(old_address_space, state->rip, &translated)) {
                // relocate higher half addresses to the ELF in physical memory
                if (state->rip >= 0xFFFFFFFF80000000ull) {
                    translated = state->rip;
                }

                kprintf("  code:   %p (translated: %p)\n\n", state->rip, translated);
                // x86_print_disasm((uint8_t*) translated, 16);
            } else {
                kprintf("  code:   NOT PRESENT\n");
            }

            halt();
        }

        return old_address_space;
    } else if (state->interrupt_num == 32) {
        if (calibrating_apic_timer) {
            apic_freq = (now - apic_freq) / APIC_CALIBRATION_TICKS;
            calibrating_apic_timer = false;

            kprintf("APIC timer freq = %d\n", apic_freq);
        }

        spall_end_event(0);
        PerCPU* cpu = get_percpu();

        u64 next_wake;
        spin_lock(&threads_lock);
        Thread* next = sched_try_switch(now / boot_info->tsc_freq, &next_wake);
        spin_unlock(&threads_lock);

        uint64_t new_now_time = (__rdtsc() / boot_info->tsc_freq);
        uint64_t until_wake = 1;
        if (next_wake > new_now_time) {
            until_wake = next_wake - new_now_time;
        }

        // if we're switching, save old thread state
        if (cpu->current_thread != NULL) {
            cpu->current_thread->state = *state;
        }

        if (next == NULL) {
            kprintf("  >> IDLE! (for %d ms, %d ticks) <<\n", until_wake / 1000, micros_to_apic_time(until_wake));

            // send EOI
            APIC(0xB0) = 0;
            // set new one-shot
            APIC(0x380) = micros_to_apic_time(until_wake);

            spall_begin_event("sleep", 0);
            *state = kernel_idle_state;
            cpu->current_thread = NULL;
            return boot_info->kernel_pml4;
        }

        // do thread context switch, if we changed
        if (cpu->current_thread != next) {
            kprintf("  >> SWITCH %p -> %p (for %d ms, %d ticks) <<\n\n", cpu->current_thread, next, until_wake / 1000, micros_to_apic_time(until_wake));

            *state = next->state;
            cpu->current_thread = next;
        } else {
            kprintf("  >> STAY %p (for %d ms, %d ticks) <<\n\n", cpu->current_thread, until_wake / 1000, micros_to_apic_time(until_wake));
        }

        // send EOI
        APIC(0xB0) = 0;
        // set new one-shot
        APIC(0x380) = micros_to_apic_time(until_wake);

        // switch into thread's address space, for kernel threads they'll use kernel_pml4
        char str[32];
        str[0] = 't', str[1] = 'a', str[2] = 's', str[3] = 'k', str[4] = '-';

        const char hex[] = "0123456789abcdef";
        int i = 5;
        uint64_t addr = (uint64_t) next;
        for(int j = 16; j--;) {
            str[i++] = hex[(addr >> (j*4)) & 0xF];
        }
        str[i++] = 0;
        spall_begin_event(str, 0);

        PageTable* next_pml4 = next->parent ? next->parent->addr_space.hw_tables : boot_info->kernel_pml4;

        kprintf("AAA %p\n", next_pml4);
        // dump_pages(next_pml4, 0, 0);

        return next_pml4;
    } else {
        halt();
        return old_address_space;
    }
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
