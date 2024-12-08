#include <common.h>
#include <stdatomic.h>
#include <boot_info.h>
#include <elf.h>
#include "font.h"

BootInfo* boot_info;
static u16 cursor;

// our identity map is somewhere in the higher half
static void* paddr2kaddr(uintptr_t p) { return (void*) (boot_info->identity_map_ptr + p); }
static uintptr_t kaddr2paddr(void* p) { return (uintptr_t) p - boot_info->identity_map_ptr; }

// core components
#include "str.c"

#ifdef __x86_64__
#include "arch/x64/x64.c"
#endif

// Implemented in mem.c (we should move physical memory management out of arch-specific)
static void* alloc_physical_chunk(void);
static void free_physical_chunk(void* ptr);

static void* alloc_physical_page(void);
static void free_physical_page(void* ptr);

// forward decls
#include "print.c"
#include "vmem.c"
#include "threads.c"
#include "scheduler.c"
#include "spall.h"

#ifdef __x86_64__
#include "arch/x64/mem.c"
#include "arch/x64/acpi.c"
#include "arch/x64/multicore.c"
#include "arch/x64/disasm.c"
#include "arch/x64/irq.c"
#endif

#include "syscall.c"
#include "pci.h"
#include "ethernet.c"
#include "pci.c"

// components which depend on architecture stuff
#define IMPL
#include "threads.c"
#include "scheduler.c"

// loader.s
extern int kernel_idle(void* arg);

void kmain(BootInfo* restrict info) {
    boot_info = info;
    boot_info->cores[0].self = &boot_info->cores[0];
    boot_info->cores[0].kernel_stack = paddr2kaddr((uintptr_t) boot_info->cores[0].kernel_stack);
    boot_info->cores[0].kernel_stack_top = paddr2kaddr((uintptr_t) boot_info->cores[0].kernel_stack_top);

    // convert pointers into kernel addresses
    boot_info->kernel_pml4 = paddr2kaddr((uintptr_t) boot_info->kernel_pml4);
    boot_info->mem_map.regions = paddr2kaddr((uintptr_t) boot_info->mem_map.regions);

    kprintf("Beginning kernel boot... PML4=%p\n", boot_info->kernel_pml4);

    if (!has_cpu_support()) {
        panic("Here's a nickel, kid. Buy yourself a computer.\n");
    }

    char brand_str[128] = {};
    get_cpu_str(brand_str);
    kprintf("Booting %s\n", brand_str);

    irq_startup(0);
    init_physical_page_alloc(&boot_info->mem_map);

    parse_acpi();
    kprintf("ACPI processed...\n");

    enable_apic();
    kprintf("Found %d cores | TSC freq %d MHz\n", boot_info->core_count, boot_info->tsc_freq);

    subdivide_memory(&boot_info->mem_map, boot_info->core_count);

    init_pci();

    spall_header();
    spall_begin_event("AAA", 0);
    spall_end_event(0);

    kprintf("GPU: %p (%d * %d), stride=%d\n", boot_info->fb.pixels, boot_info->fb.width, boot_info->fb.height, boot_info->fb.stride);

    static _Alignas(4096) const uint8_t desktop_elf[] = {
        #embed "../../userland/desktop.elf"
    };

    void* desktop_elf_ptr = paddr2kaddr(((uintptr_t) desktop_elf - boot_info->elf_virtual_ptr) + boot_info->elf_physical_ptr);

    Env* toy = env_create();
    Thread* mine = env_load_elf(toy, desktop_elf_ptr, sizeof(desktop_elf));

    kernel_idle_state = new_thread_state(kernel_idle, 0, 0, false);

    // jump into timer interrupt, we're going to run tasks now
    spall_begin_event("main", 0);
    irq_begin_timer();
}

