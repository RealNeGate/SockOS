#include <kernel.h>

BootInfo* boot_info;

void kmain(BootInfo* restrict info) {
    boot_info = info;
    boot_info->cores[0].self = &boot_info->cores[0];
    boot_info->cores[0].kernel_stack = paddr2kaddr((uintptr_t) boot_info->cores[0].kernel_stack);
    boot_info->cores[0].kernel_stack_top = paddr2kaddr((uintptr_t) boot_info->cores[0].kernel_stack_top);

    // convert pointers into kernel addresses
    boot_info->kernel_pml4 = paddr2kaddr((uintptr_t) boot_info->kernel_pml4);
    boot_info->mem_map.regions = paddr2kaddr((uintptr_t) boot_info->mem_map.regions);

    spall_header();

    kprintf("Beginning kernel boot...\n");
    arch_init(0);
}
