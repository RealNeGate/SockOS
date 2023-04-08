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
#include "arch/x64/irq.c"
#include "arch/x64/syscall.c"
#endif

// components which depend on architecture stuff
#define IMPL
#include "threads.c"

void foobar(void) {
    for (size_t i = 500; i < 1000; i++) {
        boot_info->fb.pixels[i] = 0xFF00FFFF;
    }
}

#define STR2(x) #x
#define STR(x) STR2(x)

// this aligns start address to 16 and terminates byte array with explict 0
// which is not really needed, feel free to change it to whatever you want/need
#define INCBIN(name, file) \
__asm__(".section .rodata\n" \
    ".global incbin_" STR(name) "_start\n" \
    ".balign 16\n" \
    "incbin_" STR(name) "_start:\n" \
    ".incbin \"" file "\"\n" \
    \
    ".global incbin_" STR(name) "_end\n" \
    ".balign 1\n" \
    "incbin_" STR(name) "_end:\n" \
    ".byte 0\n" \
); \
extern __attribute__((aligned(16))) const u8 incbin_ ## name ## _start[]; \
extern                              const u8 incbin_ ## name ## _end[]

INCBIN(test_program, "test2.elf");

static void kernel_halt(void) {
    // TODO(NeGate): we should add some key to spawn a shell once we have that
    kprintf("No tasks running...\n");

    // wait forever
    halt();
}

void kmain(BootInfo* restrict info) {
    boot_info = info;
	kprintf("Beginning kernel boot...\n");

    // Draw fancy background
    u64 gradient_x = (boot_info->fb.width + 255) / 256;
    u64 gradient_y = (boot_info->fb.height + 255) / 256;

    for (size_t j = 0; j < boot_info->fb.height; j++) {
        u32 g = ((j / gradient_y) / 2) + 0x7F;

        for (size_t i = 0; i < boot_info->fb.width; i++) {
            u32 b = ((i / gradient_x) / 2) + 0x7F;

            boot_info->fb.pixels[i + (j * boot_info->fb.stride)] = 0xFF000000 | (g << 16) | (b << 8);
        }
    }

    if (!has_cpu_support()) {
        panic("Here's a nickel, kid. Buy yourself a computer.\n");
    }
    char brand_str[128] = {};
    get_cpu_str(brand_str);
    kprintf("Booting %s\n", brand_str);

    init_physical_page_alloc(&info->mem_map);
    parse_acpi(boot_info->rsdp);

    Env* toy = env_create();
    Thread* mine = env_load_elf(toy, incbin_test_program_start, incbin_test_program_end - incbin_test_program_start);

    // interrupts
    irq_startup();
}
