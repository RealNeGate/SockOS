#include <kernel.h>
#include "term.h"
#include <elf.h>

#define EBR_IMPL
#include "ebr.h"

#define NBHM_IMPL
#include "nbhm.h"

KObject_Mailbox* kernel_root_mailbox;
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

////////////////////////////////
// Mini-ELF loader
////////////////////////////////
// this is the trusted ELF loader for priveleged programs, normal apps will probably
// be loaded via a shared object.
Thread* env_load_elf(Env* env, const u8* program, size_t program_size) {
    ON_DEBUG(ENV)(kprintf("Loading a program! %p\n", program));
    Elf64_Ehdr* elf_header = (Elf64_Ehdr*) program;

    KObject_VMO* vmo_ptr = vmo_create_physical(kaddr2paddr((void*) program), program_size, VMEM_PAGE_WRITE | VMEM_PAGE_EXEC);
    KHandle elf_vmo = env_open_handle(env, 0, &vmo_ptr->super);

    ////////////////////////////////
    // map segments
    ////////////////////////////////
    size_t segment_size = elf_header->e_phentsize;
    size_t segment_header_bounds = elf_header->e_phoff + elf_header->e_phnum*segment_size;
    kassert(segment_header_bounds < program_size, "segments do not fit into file");

    const u8* segments = program + elf_header->e_phoff;
    FOR_N(i, 0, elf_header->e_phnum) {
        Elf64_Phdr* segment = (Elf64_Phdr*) (segments + i*segment_size);
        if (segment->p_type != PT_LOAD) {
            continue;
        }

        // check segment permissions
        kassert(segment->p_filesz <= segment->p_memsz, "no enough space in memory for file data");
        kassert((segment->p_align & (segment->p_align - 1)) == 0, "alignment is not a power-of-two");

        // file offset % page_size == virtual addr % page_size, it allows us to file map
        // awkward offsets because the virtual address is just as awkward :p
        uintptr_t vaddr  = segment->p_vaddr & -PAGE_SIZE;
        size_t offset    = segment->p_offset & -PAGE_SIZE;
        size_t file_size = (segment->p_filesz + PAGE_SIZE - 1) & -PAGE_SIZE;
        size_t mem_size  = (segment->p_memsz  + PAGE_SIZE - 1) & -PAGE_SIZE;

        ON_DEBUG(ENV)(kprintf("[elf] segment: %p (%d) => ... (%d)\n", segment->p_vaddr, segment->p_memsz, segment->p_filesz));

        if (file_size > 0) {
            vmem_add_range(env, elf_vmo, vaddr, offset, file_size, VMEM_PAGE_WRITE);
        }

        if (mem_size > file_size) {
            // zero pages
            vmem_add_range(env, 0, vaddr + file_size, 0, mem_size - file_size, VMEM_PAGE_WRITE);
        }
    }

    // tiny i know
    size_t stack_size = 2*1024*1024;
    uintptr_t stack_ptr = vmem_map(env, 0, 0, 0, stack_size, VMEM_PAGE_WRITE, NULL);

    ON_DEBUG(ENV)(kprintf("[elf] entry=%p\n", elf_header->e_entry));
    ON_DEBUG(ENV)(kprintf("[elf] stack=%p\n", stack_ptr));

    KObject_VMO* initrd_vmo = vmo_create_physical(kaddr2paddr(boot_info->initrd), boot_info->initrd_size, VMEM_PAGE_WRITE);
    KHandle initrd_handle = env_open_handle(env, 0, &initrd_vmo->super);

    return thread_create(env, (ThreadEntryFn*) elf_header->e_entry, initrd_handle, stack_ptr, stack_size);
}

void _putchar(char ch);
void kmain(BootInfo* restrict info) {
    boot_info = info;
    boot_info->cores[0].self = &boot_info->cores[0];
    boot_info->cores[0].irq_stack_top = paddr2kaddr((uintptr_t) boot_info->cores[0].irq_stack_top);

    // convert pointers into kernel addresses
    boot_info->initrd = paddr2kaddr((uintptr_t) boot_info->initrd);
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

    #if 0
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
    #endif

    #if 0
    Env* env = env_create();
    FOR_N(i, 0, 32) {
        printf("HANDLE %zu %p\n", i, 0xA000 + i*4096);
        vmem_add_range(env, 0, 0xA000 + i*4096, 0, 4096, 0);
        if (i == 1) {
            vmem_dump(env);
        }
    }
    vmem_dump(env);
    vmem_add_range(env, 0, 0xB000, 0, 8192, 0);
    vmem_dump(env);

    vmem_node_lookup(env, 0);
    vmem_node_lookup(env, 0x11002);
    vmem_node_lookup(env, 0x13002);

    #else
    static _Alignas(4096) const uint8_t init_elf[] = {
        #embed "../../objs/init.elf"
    };

    Env* env = env_create();
    void* init_elf_ptr = paddr2kaddr(((uintptr_t) init_elf - boot_info->elf_virtual_ptr) + boot_info->elf_physical_ptr);
    Thread* bootstrap = env_load_elf(env, init_elf_ptr, sizeof(init_elf));
    thread_resume(bootstrap, NULL);
    #endif

    kernel_root_mailbox = mailbox_create(boot_info->core_count);

    // Thread* t = thread_create(NULL, sched_load_balancer, 0, (uintptr_t) kheap_alloc(16384), 16384);
    // thread_resume(t, NULL);

    arch_handoff(0);
}
