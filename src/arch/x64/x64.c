#include "x64.h"

// loader.s
extern int kernel_idle(void* arg);

static inline int x86_get_cpuid(unsigned int leaf, unsigned int *eax, unsigned int *ebx, unsigned int *ecx, unsigned int *edx) {
    asm volatile ("cpuid" : "=a" (*eax), "=b" (*ebx), "=c" (*ecx), "=d" (*edx) : "0" (leaf));
    return 1;
}

static bool has_cpu_support(void) {
    u32 eax, ebx, ecx, edx;
    x86_get_cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
    // Do we have at least the constant TSC page?
    if (eax < 0x80000007) {
        return false;
    }

    return true;
}

static void cpuid_regcpy(char *buf, u32 eax, u32 ebx, u32 ecx, u32 edx) {
    memcpy(buf + 0,  (char *)&eax, 4);
    memcpy(buf + 4,  (char *)&ebx, 4);
    memcpy(buf + 8,  (char *)&ecx, 4);
    memcpy(buf + 12, (char *)&edx, 4);
}

static void get_cpu_str(char *str) {
    u32 eax, ebx, ecx, edx;
    x86_get_cpuid(0x80000002, &eax, &ebx, &ecx, &edx);
    cpuid_regcpy(str, eax, ebx, ecx, edx);
    x86_get_cpuid(0x80000003, &eax, &ebx, &ecx, &edx);
    cpuid_regcpy(str+16, eax, ebx, ecx, edx);
    x86_get_cpuid(0x80000004, &eax, &ebx, &ecx, &edx);
    cpuid_regcpy(str+32, eax, ebx, ecx, edx);
    return;
}

void x86_set_kernel_gs(int core_id) {
    // fill GS base with PerCPU*
    x86_writemsr(IA32_KERNEL_GS_BASE, (uintptr_t) &boot_info->cores[core_id]);
    asm volatile ("swapgs");
}

void pci_init(void);
void arch_init(int core_id) {
    // Stuff we only handle once
    x86_set_kernel_gs(core_id);

    if (core_id == 0) {
        if (!has_cpu_support()) {
            panic("Here's a nickel, kid. Buy yourself a computer.\n");
        }

        char brand_str[128] = {};
        get_cpu_str(brand_str);
        kprintf("Booting %s\n", brand_str);

        kpool_init(&boot_info->mem_map);

        x86_parse_acpi();
        kprintf("ACPI processed...\n");

        x86_enable_apic();
        kprintf("Found %d cores | TSC freq %d MHz\n", boot_info->core_count, boot_info->tsc_freq);

        kpool_subdivide(boot_info->core_count);

        sched_init();
        pci_init();

        kernel_idle_state = new_thread_state(kernel_idle, 0, 0, 0, false);

        x86_irq_startup(core_id);
        x86_boot_cores();

        { // Spawn desktop
            static _Alignas(4096) const uint8_t desktop_elf[] = {
                #embed "../../../userland/desktop.elf"
            };

            Env* env = env_create();

            // framebuffer passed along
            KObject_VMO* fb_vmo = vmo_create_physical(0x80000000, 800 * 600 * sizeof(uint32_t));
            KHandle fb = env_open_handle(env, 0, &fb_vmo->super);

            void* desktop_elf_ptr = paddr2kaddr(((uintptr_t) desktop_elf - boot_info->elf_virtual_ptr) + boot_info->elf_physical_ptr);
            Thread* mine = env_load_elf(env, desktop_elf_ptr, sizeof(desktop_elf));
        }
    } else {
        sched_init();
        x86_irq_startup(core_id);
    }

    PerCPU* cpu = &boot_info->cores[core_id];

    // setup TSS, it'll store the relevant kernel stack
    {
        //  access    limit
        //    VV        VV
        u64 tss_ptr = (u64) &cpu->tss[0];
        cpu->gdt[TSS/8   ] = 0x0000890000000067 | ((tss_ptr & 0xFFFFFF) << 16) | (((tss_ptr >> 24) & 0xFF) << 56);
        cpu->gdt[TSS/8 +1] = (tss_ptr >> 32ull);

        // disable I/O map base
        ((u16*) cpu->tss)[0x102 / 4] = 0xDFFF;

        // set IST1, when an interrupt happens we'll be using this as the kernel
        // stack.
        cpu->tss[0x24 / 4] = ((u64) cpu->kernel_stack_top);
        cpu->tss[0x28 / 4] = ((u64) cpu->kernel_stack_top) >> 32ull;

        asm volatile ("mov ax, 0x28\nltr ax" ::: "ax");
    }

    kprintf("Hello Mr. CPU! %p %d\n", cpu, core_id);

    // jump into timer interrupt, we're going to run tasks now
    x86_irq_handoff(core_id);
}

void _putchar(char ch) {
    io_out8(0x3f8, ch);
}

PerCPU* cpu_get(void) {
    u64 result;
    asm volatile ("mov %q0, gs:[0]" : "=a" (result));
    return (PerCPU*) result;
}

u64 x86_get_cr2(void) {
    u64 result;
    asm volatile ("mov %q0, cr2" : "=a" (result));
    return result;
}

u64 x86_get_cr3(void) {
    u64 result;
    asm volatile ("mov %q0, cr3" : "=a" (result));
    return result;
}

void x86_halt(void) {
    asm volatile ("cli");
    for(;;) {
        asm volatile ("hlt");
    }
}

u64 x86_readmsr(u32 r) {
    u32 edx, eax;
    asm volatile ("rdmsr" : "=d"(edx), "=a"(eax) : "c"(r));
    return (((u64) edx) << 32) | (u64) eax;
}

void x86_writemsr(u32 r, u64 v) {
    u32 eax = v & 0xffffffff;
    u32 edx = v >> 32;
    asm volatile ("wrmsr" : : "c" (r), "a" (eax), "d" (edx));
}

static inline void x86_cli() {
    asm volatile ("cli");
}

static inline void x86_sti() {
    asm volatile ("sti");
}

static inline void x86_hlt() {
    asm volatile ("hlt");
}

void sched_yield(void) {
    asm volatile ("int 32");
}

uint64_t get_time_ticks(void) {
    return __rdtsc();
}
