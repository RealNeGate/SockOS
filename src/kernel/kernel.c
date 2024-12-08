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
#include "pci.c"

// components which depend on architecture stuff
#define IMPL
#include "threads.c"
#include "scheduler.c"

#define STR2(x) #x
#define STR(x) STR2(x)

static int draw_background(void *arg) {
    u8 mult = 0;

    for (;;) {
        // spall_begin_event("draw", 1);
        u64 gradient_x = (boot_info->fb.width + 255) / 256;
        u64 gradient_y = (boot_info->fb.height + 255) / 256;

        for (size_t j = 0; j < boot_info->fb.height; j++) {
            u32 g = ((j / gradient_y) / 2) + 0x7F;
            g = (g + mult) & 0xFF;

            for (size_t i = 0; i < boot_info->fb.width; i++) {
                u32 b = ((i / gradient_x) / 2) + 0x7F;
                b = (b + mult) & 0xFF;

                boot_info->fb.pixels[i + (j * boot_info->fb.stride)] = 0xFF000000 | (g << 16) | (b << 8);
            }
        }

        kprintf("A\n");
        // spall_end_event(1);

        sched_wait(boot_info->cores[0].current_thread, 50000);
        thread_yield();

        mult += 1;
    }
}

static int other_guy(void *arg) {
    for (;;) {
        kprintf("A");
    }
    return 0;
}

static int other_other_guy(void *arg) {
    for (;;) {
        kprintf("B");
    }
    return 0;
}

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

    pci_scan_all();

    spall_header();
    spall_begin_event("AAA", 0);

    // tiny i know
    /* void* physical_stack = alloc_physical_chunk();
    thread_create(NULL, draw_background, (uintptr_t) physical_stack, CHUNK_SIZE, false);
    physical_stack = alloc_physical_chunk();
    thread_create(NULL, other_guy,       (uintptr_t) physical_stack, CHUNK_SIZE, false);
    physical_stack = alloc_physical_chunk();
    thread_create(NULL, other_other_guy, (uintptr_t) physical_stack, CHUNK_SIZE, false); */
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

