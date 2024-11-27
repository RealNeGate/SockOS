#include <common.h>
#include <stdatomic.h>
#include <boot_info.h>
#include <elf.h>
#include "font.h"

BootInfo* boot_info;
static u16 cursor;

// core components
#include "str.c"

#ifdef __x86_64__
#include "arch/x64/x64.c"
#endif

// forward decls
#include "threads.c"
#include "scheduler.c"
#include "print.c"
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
        spall_begin_event("draw", 1);
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

        spall_end_event(1);

        sched_wait(boot_info->cores[0].current_thread, 1000*1000);
        thread_yield();

        mult += 1;
    }
}

static int other_guy(void *arg) {
    uint64_t sum = 0;
    for (;;) {
        sum++; // kprintf("A");
    }
    return 0;
}

static int other_other_guy(void *arg) {
    uint64_t sum = 0;
    for (;;) {
        sum++; // kprintf("B");
    }
    return 0;
}

// loader.s
extern int kernel_idle(void* arg);

void kmain(BootInfo* restrict info) {
    boot_info = info;
    kprintf("Beginning kernel boot...\n");

    if (!has_cpu_support()) {
        panic("Here's a nickel, kid. Buy yourself a computer.\n");
    }

    char brand_str[128] = {};
    get_cpu_str(brand_str);
    kprintf("Booting %s\n", brand_str);

    init_physical_page_alloc(&boot_info->mem_map);

    parse_acpi();
    kprintf("ACPI processed...\n");

    enable_apic();
    kprintf("Found %d cores | TSC freq %d MHz\n", boot_info->core_count, boot_info->tsc_freq);

    subdivide_memory(&boot_info->mem_map, boot_info->core_count);

    // TODO(flysand): figure out why it doesn't work.
    // pci_scan_all();

    spall_header();
    spall_begin_event("AAA", 0);

    // tiny i know
    void* physical_stack = alloc_physical_chunk();
    thread_create(NULL, draw_background, (uintptr_t) physical_stack, CHUNK_SIZE, false);
    physical_stack = alloc_physical_chunk();
    thread_create(NULL, other_guy,       (uintptr_t) physical_stack, CHUNK_SIZE, false);
    physical_stack = alloc_physical_chunk();
    thread_create(NULL, other_other_guy, (uintptr_t) physical_stack, CHUNK_SIZE, false);
    spall_end_event(0);

    /*extern Buffer desktop_elf;
    Env* toy = env_create();
    Thread* mine = env_load_elf(toy, desktop_elf.data, desktop_elf.length);*/

    kernel_idle_state = new_thread_state(kernel_idle, 0, 0, false);

    irq_startup(0);

    // jump into timer interrupt, we're going to run tasks now
    spall_begin_event("main", 0);
    calibrate_apic_timer();
}
