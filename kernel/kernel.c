#include <kernel.h>
#include "term.h"
#include <elf.h>

#define EBR_IMPL
#include "ebr.h"

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
    env_grant_rights(env, KACCESS_WRITE, &vmo_ptr->super);

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
            vmem_add_range(env, vmo_ptr, vaddr, offset, file_size, VMEM_PAGE_WRITE);
        }

        if (mem_size > file_size) {
            // zero pages
            vmem_add_range(env, NULL, vaddr + file_size, 0, mem_size - file_size, VMEM_PAGE_WRITE);
        }
    }

    // tiny i know
    size_t stack_size = 2*1024*1024;
    uintptr_t stack_ptr = vmem_map(env, NULL, 0, 0, stack_size, VMEM_PAGE_WRITE, NULL);

    ON_DEBUG(ENV)(kprintf("[elf] entry=%p\n", elf_header->e_entry));
    ON_DEBUG(ENV)(kprintf("[elf] stack=%p\n", stack_ptr));

    KObject_VMO* initrd_vmo = vmo_create_physical(kaddr2paddr(boot_info->initrd), boot_info->initrd_size, VMEM_PAGE_WRITE);
    KObjectID initrd_handle = env_grant_rights(env, KACCESS_WRITE, &initrd_vmo->super);

    return thread_create(env, (ThreadEntryFn*) elf_header->e_entry, initrd_handle, stack_ptr, stack_size);
}

size_t map_entry_count;
MapFileEntry* map_entries;

MapFileEntry* map_entry_get(uint32_t rva) {
    size_t left = 0;
    size_t right = map_entry_count;
    while (left < right) {
        size_t middle = (left + right) / 2;
        if (map_entries[middle].rva > rva) {
            right = middle;
        } else {
            left = middle + 1;
        }
    }

    if (right - 1 >= map_entry_count) {
        return NULL;
    }
    return &map_entries[right - 1];
}

void _putchar(char ch);
void kmain(BootInfo* restrict info) {
    boot_info = info;
    boot_info->cores[0].self = &boot_info->cores[0];
    boot_info->cores[0].irq_stack_top = paddr2kaddr((uintptr_t) boot_info->cores[0].irq_stack_top);

    // convert pointers into kernel addresses
    boot_info->map_file = paddr2kaddr((uintptr_t) boot_info->map_file);
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

    if (1) {
        char* stream = boot_info->map_file;
        char* end = stream + boot_info->map_file_size;

        // Skip description line
        while (*stream && *stream != '\n') {
            stream++;
        }

        // Count lines
        int line_count = 0;
        for (const char* curr = stream; *curr; curr++) {
            line_count += *curr == '\n';
        }

        map_entries = kheap_alloc(line_count * sizeof(MapFileEntry));
        while (stream != end) {
            stream += 1;

            int cnt = 0;
            const char* line = stream;
            const char* row[10];

            bool prev_word = false;
            while (*stream && *stream != '\n') {
                bool in_word = *stream != ' ' && *stream != '\n';
                if (in_word && !prev_word) {
                    kassert(cnt < 10, "Too many columns!");
                    row[cnt++] = stream;
                }

                if (!in_word && prev_word) {
                    stream[0] = 0;
                }
                prev_word = in_word, stream += 1;
            }
            stream[0] = 0;

            #if 0
            kprintf("ROW %d ", cnt);
            FOR_N(i, 0, cnt) {
                kprintf("'%s' ", row[i]);
            }
            kprintf("\n");
            #endif

            // parse VMA
            uint32_t vma = 0;
            for (const char* str = row[0]; *str; str++) {
                int ch = *str - '0';
                if (*str >= 'A' && *str <= 'F') { ch = (*str - 'A') + 0xA; }
                else if (*str >= 'a' && *str <= 'f') { ch = (*str - 'a') + 0xA; }

                vma = vma*16 + ch;
            }

            // skip section
            bool skip = false;
            for (const char* str = row[4]; *str; str++) {
                if (*str == '.') { skip = true; break; }
            }

            if (!skip) {
                // kprintf("VMA %#x %s\n", vma, row[4]);

                kassert(map_entry_count == 0 || map_entries[map_entry_count - 1].rva <= vma, "Not sorted?");
                map_entries[map_entry_count++] = (MapFileEntry){ vma, row[4] };
            }

            if (cnt == 0) {
                break;
            }
        }
    }

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
        #embed "../objs/init.elf"
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
