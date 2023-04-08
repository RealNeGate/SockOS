// Toaruos SMP boot for reference
// https://github.com/klange/toaruos/blob/master/kernel/arch/x86_64/smp.c

extern char bootstrap_start[];
extern char bootstrap_end[];

uintptr_t smp_stack_base = 0;
uintptr_t smp_lapic_addr = 0;

void smp_main(void) {
    // set up the GS, interrupts, and paging
    
    // add the idle task to the job queue

    // boot the scheduler 
}

void apic_clear_errors(u64 lapic_base) {
    *((volatile u32 *)(lapic_base + 0x280)) = 0;
}

void send_ipi(u64 lapic_base, u64 lapic_id, u64 val) {
    *((volatile u32 *)(lapic_base + 0x310)) = lapic_id << 24;
    *((volatile u32 *)(lapic_base + 0x300)) = val;

    bool delivered = false;
    while (!delivered) {
        delivered = (*((volatile u32 *)(lapic_base + 0x300)) & (1 << 12));
    }
}

void boot_cores(BootInfo *info) {
    if (info->core_count == 1) {
        return;
    }

    // map the bootstrap code

    // Walk the cores and boot them
    /*
    for (int i = 1; i < info->core_count; i++) {
        u32 lapic_id = info->cores[i]->lapic_id;

        // smp_stack_base = alloc_page();
        
        // Send a CPU INIT
        apic_clear_errors(info->lapic_base);
        send_ipi(info->lapic_base, info->lapic_id, 0x4500);
        // sleep for a while

        // Send a STARTUP IPI
        apic_clear_errors(info->lapic_base);
        send_ipi(info->lapic_base, info->lapic_id, 0x4601);
    }
    */
}
