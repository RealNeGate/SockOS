#include "x64.h"

// Toaruos SMP boot for reference
// https://github.com/klange/toaruos/blob/master/kernel/arch/x86_64/smp.c
void smp_main(PerCPU* cpu) {
    arch_init(cpu->core_id);
}

u32 apic_get_errors(void) {
    return *((volatile u32 *)(boot_info->lapic_base + 0x280));
}

void x86_send_ipi(u64 lapic_id, u64 val) {
    APIC(0x310) = lapic_id << 24;
    APIC(0x300) = val;

    while (APIC(0x300) & (1 << 12)) {}
}

void arch_wake_up(int core_id) {
    if (cpu_get()->core_id != core_id) {
        u32 lapic_id = boot_info->cores[core_id].lapic_id;
        if (atomic_compare_exchange_strong(&boot_info->cores[core_id].sched->idleing, &(bool){ true }, false)) {
            // sending an IPI which triggers int#32 (timer interrupt)
            x86_send_ipi(lapic_id, 0x4820);
            kprintf("Wake! %d\n", lapic_id);
        }
    }
}

static void powernap(u64 micros) {
    u64 now = __rdtsc();
    u64 endtime = now + (micros*boot_info->tsc_freq);

    while (now = __rdtsc(), now < endtime) {
        // keep waiting
        asm volatile ("pause");
    }
}

void x86_boot_cores(void) {
    if (boot_info->core_count == 1) {
        return;
    }

    u64* gdt_table = kheap_alloc(boot_info->core_count * sizeof(u64));
    gdt_table[0] = 0;

    char* chunk = kpool_alloc_chunk();
    size_t mark = 0;

    FOR_N(k, 1, boot_info->core_count) {
        if (mark + 65536 >= CHUNK_SIZE) {
            chunk = kpool_alloc_chunk();
            mark  = 0;
        }

        char* sp = &chunk[mark];
        kprintf("KernelStack[%d]: %p - %p\n", k, sp, sp + 65535);

        // if windows can get away with small kernel stacks so can we
        PerCPU* restrict cpu = &boot_info->cores[k];
        cpu->self = cpu;
        cpu->kernel_stack     = (uint8_t*) sp;
        cpu->kernel_stack_top = (uint8_t*) sp + 65536;

        // setup per-core GDT
        u64 gdt_base = (u64) boot_info->cores[k].gdt;
        boot_info->cores[k].gdt_pointer[0] = sizeof(boot_info->cores[0].gdt) - 1;
        memcpy(&boot_info->cores[k].gdt_pointer[1], &gdt_base, sizeof(u64));

        u64* gdt = boot_info->cores[k].gdt;
        gdt[0] = 0;
        gdt[1] = 0xAF9A000000FFFF; // kernel CS
        gdt[2] = 0xCF92000000FFFF; // kernel DS
        gdt[3] = 0xCFF2000000FFFF; // user DS
        gdt[4] = 0xAFFA000000FFFF; // user CS
        gdt[5] = 0;                // TSS
        gdt[6] = 0;                // TSS

        gdt_table[k] = (u64) &boot_info->cores[k].gdt_pointer[0];
    }

    // map the bootstrap code
    //   we just copy it to 0x1000 (with the data going before that) because
    //   we're silly and goofy and need it available somewhere the 16bit stuff can go
    extern char bootstrap_data_start[];
    extern char bootstrap_start[];
    extern char bootstrap_end[];
    extern char premain[];

    // other cores need to be able to see the bootstrap chunk once they transition to paging
    memmap_view(boot_info->kernel_pml4, 0x1000, 0x1000, PAGE_ALIGN(bootstrap_end - bootstrap_start), VMEM_PAGE_WRITE);

    char* dst = paddr2kaddr(0x1000);
    memcpy(dst, bootstrap_start, bootstrap_end - bootstrap_start);

    // fill in relocations
    volatile u64* slots = paddr2kaddr(0x1000 + (bootstrap_data_start - bootstrap_start));
    slots[0] = kaddr2paddr(boot_info->kernel_pml4); // pml4
    slots[1] = (u64) &smp_main;
    slots[2] = (u64) &boot_info->cores[0];
    slots[3] = (u64) sizeof(PerCPU);
    slots[4] = (u64) premain;
    slots[5] = (u64) gdt_table;

    // Walk the cores and boot them
    FOR_N(i, 1, boot_info->core_count) {
        u32 lapic_id = boot_info->cores[i].lapic_id;
        // Send a CPU INIT
        x86_send_ipi(lapic_id, 0x4501);
    }

    powernap(1000000);

    FOR_N(i, 1, boot_info->core_count) {
        u32 lapic_id = boot_info->cores[i].lapic_id;
        // Send a STARTUP IPI
        x86_send_ipi(lapic_id, 0x4601);
    }
}

