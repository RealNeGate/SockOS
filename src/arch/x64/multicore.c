#include "x64.h"

// Toaruos SMP boot for reference
// https://github.com/klange/toaruos/blob/master/kernel/arch/x86_64/smp.c
static Lock print_lock;

void smp_main(PerCPU* cpu) {
    spin_lock(&print_lock);
    kprintf("Hello Mr. CPU! %p %d\n", cpu, cpu->core_id);
    spin_unlock(&print_lock);

    arch_init(cpu->core_id);
    x86_halt();
}

u32 apic_get_errors(void) {
    return *((volatile u32 *)(boot_info->lapic_base + 0x280));
}

void send_ipi(u64 lapic_id, u64 val) {
    APIC(0x310) = lapic_id << 24;
    APIC(0x300) = val;

    while (APIC(0x300) & (1 << 12)) {}
}

static void powernap(u64 micros) {
    u64 now = __rdtsc();
    u64 endtime = now + (micros*boot_info->tsc_freq);

    while (now = __rdtsc(), now < endtime) {
        // keep waiting
        asm volatile ("pause");
    }
}

void boot_cores(void) {
    if (boot_info->core_count == 1) {
        return;
    }

    FOR_N(k, 1, boot_info->core_count) {
        char* sp = kpool_alloc_page();
        kprintf("KernelStack[%d]: %p - %p\n", k, sp, sp + 4095);

        // if windows can get away with small kernel stacks so can we
        PerCPU* restrict cpu = &boot_info->cores[k++];
        cpu->self = cpu;
        cpu->kernel_stack     = (uint8_t*) sp;
        cpu->kernel_stack_top = (uint8_t*) sp + 4096;
    }

    // map the bootstrap code
    //   we just copy it to 0x1000 (with the data going before that) because
    //   we're silly and goofy and need it available somewhere the 16bit stuff can go
    extern char bootstrap_start[];
    extern char bootstrap_entry[];
    extern char bootstrap_end[];
    extern char premain[];

    char* dst = (char*) 0x1000;
    memcpy(dst - (bootstrap_entry - bootstrap_start), bootstrap_start, bootstrap_end - bootstrap_start);

    // fill in relocations
    volatile u64* slots = (volatile u64*) (0x1000 - (bootstrap_entry - bootstrap_start));
    slots[0] = (u64) boot_info->kernel_pml4; // pml4
    slots[1] = (u64) &smp_main;
    slots[2] = (u64) &boot_info->cores[0];
    slots[3] = (u64) sizeof(PerCPU);
    slots[4] = (u64) 0x1000 + (premain - bootstrap_entry);

    // Walk the cores and boot them
    FOR_N(i, 1, boot_info->core_count) {
        u32 lapic_id = boot_info->cores[i].lapic_id;

        // Send a CPU INIT
        send_ipi(lapic_id, 0x4501);

        // sleep for a while
        powernap(200);

        // Send a STARTUP IPI
        send_ipi(lapic_id, 0x4601);

        powernap(200);
    }

    powernap(1000000);
    x86_halt();
}
