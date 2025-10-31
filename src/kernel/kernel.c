#include <kernel.h>
#include "term.h"

#define EBR_IMPL
#include "ebr.h"

#define NBHM_IMPL
#include "nbhm.h"

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

void _putchar(char ch);
void kmain(BootInfo* restrict info) {
    boot_info = info;
    boot_info->cores[0].self = &boot_info->cores[0];
    boot_info->cores[0].irq_stack_top = paddr2kaddr((uintptr_t) boot_info->cores[0].irq_stack_top);

    // convert pointers into kernel addresses
    boot_info->map_file = paddr2kaddr((uintptr_t) boot_info->map_file);
    boot_info->kernel_pml4 = paddr2kaddr((uintptr_t) boot_info->kernel_pml4);
    boot_info->mem_map.regions = paddr2kaddr((uintptr_t) boot_info->mem_map.regions);
    boot_info->fb.pixels = paddr2kaddr((uintptr_t) boot_info->fb.pixels);

    for (size_t j = 0; j < 50; j++) {
        for (size_t i = 0; i < 50; i++) {
            boot_info->fb.pixels[i + (j * boot_info->fb.stride)] = 0xFF1F7FFF;
        }
    }

    term_set_framebuffer(boot_info->fb);
    // term_set_wrap(true);

    kprintf("Beginning kernel boot...\n");

    arch_init(0);
    ebr_init();

    static _Alignas(4096) const uint8_t desktop_elf[] = {
        #embed "../../userland/desktop.elf"
    };

    if (0) {
        char* stream = boot_info->map_file;
        char* end = stream + boot_info->map_file_size;

        // Skip description line
        while (*stream && *stream != '\n') {
            stream++;
        }

        while (stream != end) {
            stream += 1;

            int cnt = 0;
            const char* line = stream;
            const char* row[10];

            bool prev_word = false;
            while (*stream && *stream != '\n') {
                bool in_word = *stream != ' ' && *stream != '\n';
                if (in_word && !prev_word) {
                    row[cnt++] = stream;
                }

                if (!in_word && prev_word) {
                    stream[0] = 0;
                }
                prev_word = in_word, stream += 1;
            }
            stream[0] = 0;

            printf("ROW %d ", cnt);
            FOR_N(i, 0, cnt) {
                printf("'%s' ", row[i]);
            }
            printf("\n");
            if (cnt == 0) {
                break;
            }
        }
    }

    #if 0
    Env* env = env_create();
    void* desktop_elf_ptr = paddr2kaddr(((uintptr_t) desktop_elf - boot_info->elf_virtual_ptr) + boot_info->elf_physical_ptr);
    Thread* bootstrap = env_load_elf(env, desktop_elf_ptr, sizeof(desktop_elf));
    thread_resume(bootstrap);
    #endif

    // Thread* t = thread_create(NULL, sched_load_balancer, 0, (uintptr_t) kheap_alloc(8192), 8192);
    // thread_resume(t);

    arch_handoff(0);
}
