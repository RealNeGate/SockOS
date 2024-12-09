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
    spall_begin_event("init", 0);
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

        pci_init();

        static _Alignas(4096) const uint8_t desktop_elf[] = {
            #embed "../../../userland/desktop.elf"
        };

        void* desktop_elf_ptr = paddr2kaddr(((uintptr_t) desktop_elf - boot_info->elf_virtual_ptr) + boot_info->elf_physical_ptr);

        Thread* mine = env_load_elf(env_create(), desktop_elf_ptr, sizeof(desktop_elf));
        kernel_idle_state = new_thread_state(kernel_idle, 0, 0, false);
    }

    // jump into timer interrupt, we're going to run tasks now
    spall_begin_event("main", 0);
    x86_irq_startup(core_id);
}

void put_char(int ch) {
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

static void thread_yield(void) {
    asm volatile ("int 32");
}
