#include <kernel.h>

BootInfo* boot_info;

// murmur3 32-bit
uint32_t mur3_32(const void *key, int len, uint32_t h) {
    // main body, work on 32-bit blocks at a time
    for (int i=0;i<len/4;i++) {
        uint32_t k = ((uint32_t*) key)[i]*0xcc9e2d51;
        k = ((k << 15) | (k >> 17))*0x1b873593;
        h = (((h^k) << 13) | ((h^k) >> 19))*5 + 0xe6546b64;
    }

    // load/mix up to 3 remaining tail bytes into a tail block
    uint32_t t = 0;
    uint8_t *tail = ((uint8_t*) key) + 4*(len/4);
    switch(len & 3) {
        case 3: t ^= tail[2] << 16;
        case 2: t ^= tail[1] <<  8;
        case 1: {
            t ^= tail[0] <<  0;
            h ^= ((0xcc9e2d51*t << 15) | (0xcc9e2d51*t >> 17))*0x1b873593;
        }
    }

    // finalization mix, including key length
    h = ((h^len) ^ ((h^len) >> 16))*0x85ebca6b;
    h = (h ^ (h >> 13))*0xc2b2ae35;
    return h ^ (h >> 16);
}

static uint8_t print_buffer[8192];
void kmain(BootInfo* restrict info) {
    boot_info = info;
    boot_info->cores[0].self = &boot_info->cores[0];
    boot_info->cores[0].kernel_stack = paddr2kaddr((uintptr_t) boot_info->cores[0].kernel_stack);
    boot_info->cores[0].kernel_stack_top = paddr2kaddr((uintptr_t) boot_info->cores[0].kernel_stack_top);

    // convert pointers into kernel addresses
    boot_info->kernel_pml4 = paddr2kaddr((uintptr_t) boot_info->kernel_pml4);
    boot_info->mem_map.regions = paddr2kaddr((uintptr_t) boot_info->mem_map.regions);

    print_ring_init(print_buffer, sizeof(print_buffer));

    kprintf("Beginning kernel boot...\n");
    arch_init(0);
}
