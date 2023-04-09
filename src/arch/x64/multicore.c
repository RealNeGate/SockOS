// Toaruos SMP boot for reference
// https://github.com/klange/toaruos/blob/master/kernel/arch/x86_64/smp.c

extern char bootstrap_start[];
extern char bootstrap_end[];

uintptr_t smp_stack_base = 0;
uintptr_t smp_lapic_addr = 0;

void smp_main(PerCPU* cpu) {
    kprintf("Hello: %d\n", cpu - boot_info->cores);
    // set up the GS, interrupts, and paging

    // add the idle task to the job queue

    // boot the scheduler
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

    // allocate percpu info
    FOREACH_N(i, 1, boot_info->core_count) {
        PerCPU* restrict cpu = &boot_info->cores[i];

        // if windows can get away with this so can we
        cpu->kernel_stack = alloc_physical_pages(KERNEL_STACK_SIZE / PAGE_SIZE);
        cpu->kernel_stack_top = cpu->kernel_stack + KERNEL_STACK_SIZE;
    }

    // map the bootstrap code
    //   we just copy it to 0x1000 (with the data going before that) because
    //   we're silly and goofy and need it available somewhere the 16bit stuff can go
    extern char bootstrap_start[];
    extern char bootstrap_entry[];
    extern char bootstrap_end[];
    extern char premain[];

    kassert((premain - bootstrap_entry) == 0x186, "expected layout %p", premain - bootstrap_entry);

    char* dst = (char*) 0x1000;
    memcpy(dst - (bootstrap_entry - bootstrap_start), bootstrap_start, bootstrap_end - bootstrap_start);

    // fill in relocations
    volatile u64* slots = (volatile u64*) (0x1000 - (bootstrap_entry - bootstrap_start));
    slots[0] = (u64) boot_info->kernel_pml4; // pml4
    slots[1] = (u64) &smp_main;
    slots[2] = (u64) &boot_info->cores[0];
    slots[3] = (u64) sizeof(PerCPU);
    slots[4] = 0; // atomic counter for core slot reserving
    slots[5] = (u64) 0x1000 + (premain - bootstrap_entry);

    // Walk the cores and boot them
    FOREACH_N(i, 1, boot_info->core_count) {
        u32 lapic_id = boot_info->cores[i].lapic_id;

        kprintf("Starting CPU 0x%x\n", i);

        // Send a CPU INIT
        send_ipi(lapic_id, 0x4501);

        // sleep for a while
        powernap(200);
        kprintf("Errors? %d\n", apic_get_errors());

        // Send a STARTUP IPI
        send_ipi(lapic_id, 0x4601);

        powernap(200);
        kprintf("Errors? %d\n", apic_get_errors());
        break;
    }

    powernap(1000000);
    halt();
}
