#include <common.h>
#include <stdatomic.h>
#include <boot_info.h>
#include <elf.h>
#include "font.h"

BootInfo* boot_info;
static u16 cursor;

static void kernel_halt(void);

// core components
#include "str.c"

#ifdef __x86_64__
#include "arch/x64/x64.c"
#endif

// forward decls
#include "threads.c"
#include "print.c"

#ifdef __x86_64__
#include "arch/x64/mem.c"
#include "arch/x64/acpi.c"
#include "arch/x64/multicore.c"
#include "arch/x64/irq.c"
#include "arch/x64/syscall.c"
#endif

// components which depend on architecture stuff
#define IMPL
#include "threads.c"

int foobar(void* arg) {
    for (size_t i = 500; i < 1000; i++) {
        boot_info->fb.pixels[i] = 0xFF00FFFF;
    }

    // kill itself (then switch away)
    thread_kill(threads_current);
    asm ("int 32");
    return 0;
}

#define STR2(x) #x
#define STR(x) STR2(x)

static void kernel_halt(void) {
    // TODO(NeGate): we should add some key to spawn a shell once we have that
    kprintf("No tasks running...\n");

}

static int draw_background(void *arg) {
    u8 mult = 0;
    kprintf("here?\n");
    for (;;) {
        u64 gradient_x = (boot_info->fb.width + 255) / 256;
        u64 gradient_y = (boot_info->fb.height + 255) / 256;

        for (size_t j = 0; j < boot_info->fb.height; j++) {
            u32 g = ((j / gradient_y) / 2) + 0x7F;
            g *= mult;

            for (size_t i = 0; i < boot_info->fb.width; i++) {
                u32 b = ((i / gradient_x) / 2) + 0x7F;
                b *= mult;

                boot_info->fb.pixels[i + (j * boot_info->fb.stride)] = 0xFF000000 | (g << 16) | (b << 8);
            }
        }
        sched_wait(threads_current, 1000*1000);
        kprintf("we return?\n");
        mult += 1;
    }
}

static int kernel_init(void* arg) {
    // boot_cores();

    // extern Buffer test2;
    // Env* toy = env_create();
    //Thread* mine = env_load_elf(toy, test2.data, test2.length);

    kprintf("spinning to win\n");
    for (;;) {}
    return 0;
}

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
    enable_apic();

    kprintf("ACPI processed...\n");
    kprintf("Found %d cores | TSC freq %d MHz\n", boot_info->core_count, boot_info->tsc_freq);

    // we're switching into a proper kernel task such that we can sleep correctly
    uintptr_t nutstack = (uintptr_t) alloc_physical_pages(KERNEL_STACK_SIZE / PAGE_SIZE);
    thread_create(NULL, draw_background, nutstack, KERNEL_STACK_SIZE, false);

    uintptr_t nustack = (uintptr_t) alloc_physical_pages(KERNEL_STACK_SIZE / PAGE_SIZE);
    thread_create(NULL, kernel_init, nustack, KERNEL_STACK_SIZE, false);

    irq_startup(0);

    // jump into timer interrupt, we're going to run tasks now
    asm volatile ("int 32");
}
