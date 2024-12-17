#include "x64.h"
#include "../../kernel/threads.h"

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

enum {
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
extern _Noreturn void do_context_switch(CPUState* state, uintptr_t address_space);

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
CPUState kernel_idle_state;

static volatile enum {
    APIC_UNCALIBRATED,
    APIC_CALIBRATING,
    APIC_CALIBRATED,
} apic_timer_status;

static u64 apic_freq;

typedef struct {
    void* ctx;
    void (*fn)(void* ctx);
} InterruptLineCallback;

static InterruptLineCallback line_callbacks[24];

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

    // Wait for IO
    asm volatile(
        "jmp 1f;"
        "1:jmp 1f;"
        "1:"
    );
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

void x86_irq_startup(int core_id) {
    if (core_id == 0) {
        FOR_N(i, 0, 256) _idt[i] = (IDTEntry){ 0 };
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
        SET_INTERRUPT(81, false);

        // allow int3 in ring 3
        _idt[3].type_attr |= (3 << 5);
    }

    irq_disable_pic();

    // enable syscall/sysret
    {
        u64 x = x86_readmsr(IA32_EFER);
        x86_writemsr(IA32_EFER, x | 1);
        // the location where syscall will throw the user to
        x86_writemsr(IA32_LSTAR, (uintptr_t) &syscall_handler);
        // syscall and sysret segments
        _Static_assert(KERNEL_CS + 8  == KERNEL_DS, "SYSCALL expectations");
        _Static_assert(0x10      + 16 == USER_CS,   "SYSRET expectations");
        _Static_assert(0x10      + 8  == USER_DS,   "SYSRET expectations");
        x86_writemsr(IA32_STAR, ((u64)KERNEL_CS << 32ull) | (0x10ull << 48ull));

        // we're storing the per_cpu info here, syscall will use this
        x86_writemsr(IA32_KERNEL_GS_BASE, (uintptr_t) &boot_info->cores[core_id]);
    }

    IDT idt;
    idt.limit = sizeof(_idt) - 1;
    idt.base = (uintptr_t) _idt;
    irq_enable(&idt);
}

u32 ioapic_read(u64 ioapic_addr, u32 reg) {
    *(u32 volatile *)ioapic_addr = reg;
    return *(u32 volatile *)(ioapic_addr + 0x10);
}

void ioapic_write(u64 ioapic_addr, u32 reg, u32 val) {
    *(u32 volatile *)ioapic_addr = reg;
    *(u32 volatile *)(ioapic_addr + 0x10) = val;
}

void set_interrupt_line(u32 line, void fn(void*), void* ctx) {
    // redirect table has 2 32-bit entries per interrupt slot
    u32 redirect_table_idx = (line * 2) + 0x10;
    u32 entry = 0x50 + line;

    line_callbacks[line].ctx = ctx;
    line_callbacks[line].fn = fn;

    ioapic_write(boot_info->ioapic_base, redirect_table_idx,     1 << 16); // Mask the interrupt while we tweak
    ioapic_write(boot_info->ioapic_base, redirect_table_idx + 1, boot_info->cores[0].lapic_id << 24);
    ioapic_write(boot_info->ioapic_base, redirect_table_idx,     entry);

    kprintf("idx: 0x%x, added line: %d | %016b\n", redirect_table_idx, line, entry);
}

void arch_handoff(int core_id) {
    // Enable APIC timer
    //   0xF0 - Spurious Interrupt Vector Register
    //   Punting spurious interrupts (see PIC/CPU race condition, this is a fake interrupt)
    APIC(0xF0) |= 0x1FF;
    //   320h - LVT timer register
    //     we just want a simple one-shot timer on IRQ32
    APIC(0x320) = 0x00000 | 32;
    //   timer divide reg
    APIC(0x3E0) = 0b1011;

    // calibrate APIC timer
    if (core_id == 0) {
        apic_timer_status = APIC_CALIBRATING;
        apic_freq = __rdtsc();
        APIC(0x380) = APIC_CALIBRATION_TICKS;

        for (;;) {
            asm volatile ("hlt");
        }
    } else {
        // maybe we should signal the cores in this case... idk
        while (apic_timer_status != APIC_CALIBRATED) {
            asm volatile ("pause");
        }

        kprintf("CPU-%d is now ready to work!\n", core_id);
        spall_begin_event("main", core_id);

        asm volatile ("int 32");
    }
}

uintptr_t x86_irq_int_handler(CPUState* state, uintptr_t cr3, PerCPU* cpu) {
    if (apic_timer_status == APIC_CALIBRATED) {
        spall_end_event(cpu->core_id);
        spall_begin_event(interrupt_names[state->interrupt_num], cpu->core_id);
    }

    u64 now = __rdtsc();
    PageTable* old_address_space = paddr2kaddr(cr3);

    #if DEBUG_IRQ
    if (state->interrupt_num != 14 && state->interrupt_num != 32) {
        kprintf("CPU-%d: %s (%d): error=0x%x\n", cpu->core_id, interrupt_names[state->interrupt_num], state->interrupt_num, state->error);
        kprintf("  rip=%x:%p rsp=%x:%p\n", state->cs, state->rip, state->ss, state->rsp);
    }
    #endif

    if (state->interrupt_num == 14) {
        u64 access_addr = x86_get_cr2();

        Env* env = NULL;
        Thread* curr = cpu->current_thread;
        if (curr && curr->parent) {
            env = curr->parent;
        }

        if (env != NULL) {
            rwlock_lock_shared(&env->addr_space.lock);

            // update hardware page tables to match
            VMem_PTEUpdate update;
            bool is_write = state->error & 2;
            if (vmem_segfault(env, access_addr, is_write, &update)) {
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
                uint64_t page_flags = 0;
                if (update.flags & VMEM_PAGE_USER)  { page_flags |= PAGE_USER;  }
                if (update.flags & VMEM_PAGE_WRITE) { page_flags |= PAGE_WRITE; }

                PageTable* curr = env->addr_space.hw_tables;
                kassert(curr == old_address_space, "aren't we editting the old_address_space?");

                for (size_t i = 0; i < 3; i++) {
                    size_t index = (access_addr >> shifts[i]) & 0x1FF;

                    // the intermediate page tables need to have permissions that are "above" the child pages, so we'll OR our
                    // flags with it.
                    u64 entry = atomic_load_explicit(&curr->entries[index], memory_order_relaxed);
                    for (;;) {
                        u64 new_entry = entry;
                        // no table? add one
                        PageTable* new_pt = NULL;
                        if (entry & PAGE_PRESENT) {
                            // bits missing? add one
                            new_entry |= page_flags;
                        } else {
                            new_pt = kpool_alloc_page();
                            memset(new_pt, 0, sizeof(PageTable));

                            new_entry = kaddr2paddr(new_pt) | page_flags | PAGE_PRESENT;
                        }
                        // no progress necessary, it's already behaving
                        if (entry == new_entry) { break; }
                        // transaction, if we fail at least someone allocate the
                        // physical page (so we don't spam allocations as much)
                        if (atomic_compare_exchange_strong(&curr->entries[index], &entry, new_entry)) { entry = new_entry; break; }
                        // throw away our new_pt
                        if (new_pt == NULL) { kpool_free_page(new_pt); }
                    }

                    curr = paddr2kaddr(entry & ~0x1FF);
                    kassert(curr != NULL, "missing page table, didn't we just insert it?");
                }

                size_t pte_index = (access_addr >> 12) & 0x1FF; // 4KiB

                u64 old_pte = curr->entries[pte_index];
                u64 new_pte = (update.translated & 0xFFFFFFFFF000) | page_flags | PAGE_PRESENT;
                if (old_pte != new_pte) {
                    atomic_compare_exchange_strong(&curr->entries[pte_index], &old_pte, new_pte);
                    ON_DEBUG(VMEM)(kprintf("[vmem] updated PTE %p -> %p!\n", old_pte, new_pte));
                }

                rwlock_unlock_shared(&env->addr_space.lock);
                return cr3;
            } else {
                rwlock_unlock_shared(&env->addr_space.lock);
            }
        }

        #if DEBUG_IRQ
        // just throw error
        kprintf("CPU-%d: Page Fault: error=%#x, env=%p\n", cpu->core_id, state->error, env);
        kprintf("  cr3=%p\n\n", cr3);

        // print memory access address
        u64 translated;
        if (memmap_translate(old_address_space, access_addr, &translated)) {
            kprintf("  access: %d:%p (translated: %p)\n", state->ss, access_addr, translated);
        } else {
            kprintf("  access: %d:%p (NOT PRESENT)\n", state->ss, access_addr);
        }

        // dissassemble code
        if (memmap_translate(old_address_space, state->rip, &translated)) {
            kprintf("  code:   %d:%p (translated: %p, %x)\n\n", state->cs, state->rip, translated, *(u8*) paddr2kaddr(translated));
            // x86_print_disasm((uint8_t*) translated, 16);
        } else {
            kprintf("  code:   %d:%p (NOT PRESENT)\n", state->cs, state->rip);
        }
        #endif

        x86_halt();
        return cr3;
    } else if (state->interrupt_num == 32) {
        if (apic_timer_status == APIC_CALIBRATING) {
            apic_freq = (now - apic_freq) / APIC_CALIBRATION_TICKS;

            kprintf("APIC timer freq = %d\n", apic_freq);

            spall_header();
            spall_begin_event("main", cpu->core_id);
            apic_timer_status = APIC_CALIBRATED;
        }

        cpu->sched->idleing = false;

        u64 now_micros = now / boot_info->tsc_freq;

        u64 next_wake;
        spin_lock(&cpu->sched->lock);
        Thread* next = sched_pick_next(cpu, now_micros, &next_wake);
        spin_unlock(&cpu->sched->lock);

        // if we're switching, save old thread state
        if (cpu->current_thread != NULL) {
            cpu->current_thread->state = *state;
        }

        // send EOI
        APIC(0xB0) = 0;

        // don't arm the timer again if we're gonna sleep forever
        uint64_t new_now_time = (__rdtsc() / boot_info->tsc_freq);
        if (next_wake != UINT64_MAX) {
            uint64_t until_wake = next_wake > new_now_time ? next_wake - new_now_time : 1;

            // set new one-shot
            APIC(0x380) = micros_to_apic_time(next_wake > new_now_time ? next_wake - new_now_time : 1);

            if (cpu->current_thread != next) {
                ON_DEBUG(IRQ)(kprintf("[irq] CPU-%d: switch %p -> %p for %f ms\n", cpu->core_id, cpu->current_thread, next, (next_wake - new_now_time) / 1000.0f));
            } else {
                ON_DEBUG(IRQ)(kprintf("[irq] CPU-%d: stay on %p for %f ms\n", cpu->core_id, next, (next_wake - new_now_time) / 1000.0f));
            }
        } else {
            cpu->sched->idleing = true;

            if (cpu->current_thread != next) {
                ON_DEBUG(IRQ)(kprintf("[irq] CPU-%d: switch %p -> %p... \"forever\"\n", cpu->core_id, cpu->current_thread, next));
            } else {
                ON_DEBUG(IRQ)(kprintf("[irq] CPU-%d: stay on %p... \"forever\"\n", cpu->core_id, next));
            }
        }

        if (next == NULL) {
            spall_end_event(cpu->core_id);
            spall_begin_event("sleep", cpu->core_id);

            *state = kernel_idle_state;
            cpu->current_thread   = NULL;
            cpu->kernel_stack_top = NULL;
            return kaddr2paddr(boot_info->kernel_pml4);
        }

        // do thread context switch, if we changed
        if (cpu->current_thread != next) {
            *state = next->state;

            cpu->current_thread   = next;
            cpu->kernel_stack_top = (void*) next->kstack_addr;

        }

        #if DEBUG_SPALL
        {
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

            spall_end_event(cpu->core_id);
            spall_begin_event(str, cpu->core_id);
        }
        #endif

        PageTable* next_pml4 = next->parent ? next->parent->addr_space.hw_tables : boot_info->kernel_pml4;
        return kaddr2paddr(next_pml4);
    } else if (state->interrupt_num >= 0x50) {
        // interrupt lines
        InterruptLineCallback* c = &line_callbacks[state->interrupt_num - 0x50];
        if (c->fn) {
            c->fn(c->ctx);

            APIC(0xB0) = 0;
        } else {
            x86_halt();
        }
        return cr3;
    } else {
        x86_halt();
        return cr3;
    }
}

CPUState new_thread_state(void* entrypoint, uintptr_t arg, uintptr_t stack, size_t stack_size, bool is_user) {
    // the stack will grow downwards.
    // the other registers are zeroed by default.
    CPUState s = {
        .rip = (uintptr_t) entrypoint, .rsp = stack + stack_size,
        .rdi = arg,
        .flags = 0x200, .cs = 0x08, .ss = 0x10,
    };

    if (is_user) {
        // |3 is for adding the requested privilege level (RPL) to the selectors
        s.cs = USER_CS | 3;
        s.ss = USER_DS | 3;
    }

    return s;
}
