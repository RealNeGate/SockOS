#include <stddef.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdalign.h>
#include <boot_info.h>

#include "crt.c"
#include "com.c"
#include "term.c"
#include "printf.c"

#undef panic
#define panic(fmt, ...)       \
do {                          \
    printf(fmt, # __VA_ARGS__); \
    for (;;) {}               \
} while (false);

#include "efi.h"
#include "efi_util.c"
#include "elf.h"
#include "elf_loader.c"

#define PAGE_4K(x) (((x) + 0xFFF) / 0x1000)
#define PAGE_2M(x) (((x) + 0x1FFFFF) / 0x200000)
#define PAGE_1G(x) (((x) + 0x3FFFFFFF) / 0x40000000)

#define KERNEL_FILENAME L"kernel.so"

#define KERNEL_BUFFER_SIZE (16 * 1024 * 1024)

typedef void (*LoaderFunction)(BootInfo* info, u8* stack, u64 gdt_base);

typedef struct {
    size_t capacity;
    size_t used;

    // zero slot is the root PML4 thingy
    PageTable* tables;
} PageTableContext;

static size_t estimate_page_table_count(size_t size) {
    size_t count = 0;
    count += PAGE_4K(size); // 4KiB pages
    count += PAGE_2M(size); // 2MiB pages
    count += PAGE_1G(size); // 1GiB pages
    return count;
}

static alignas(4096) BootInfo kernel_boot_info;

inline static size_t align_up(size_t a, size_t b) {
    return a + (b - (a % b)) % b;
}

static PageTable* get_or_alloc(PageTableContext* ctx, PageTable* table, int index) {
    if (table->entries[index] == 0) {
        if (ctx->used + 1 >= ctx->capacity) {
            panic("Fuck!\n");
        }

        PageTable* new_table = &ctx->tables[ctx->used++];
        memset(new_table, 0, sizeof(PageTable));

        table->entries[index] = ((u64)new_table) | 3;
        return new_table;
    } else {
        return (PageTable*)(table->entries[index] & 0xFFFFFFFFF000);
    }
}

static void map_pages(PageTableContext* ctx, u64 virt_addr, u64 phys_addr, u64 page_count) {
    printf("  Making a map %X -> %X .. %X\n", virt_addr, phys_addr, phys_addr + page_count*4096 - 1);

    PageTable* address_space = &ctx->tables[0];
    if ((phys_addr | virt_addr) & 0xFFFull) {
        panic("Bad addresses for mapping %X -> %X\n", virt_addr, phys_addr);
    }

    for (size_t i = 0; i < page_count; i++) {
        // Generate the page table mapping
        u64 pte_index   = (virt_addr >> 12) & 0x1FF; // 4KB

        PageTable* curr = address_space;
        curr = get_or_alloc(ctx, curr, (virt_addr >> 39) & 0x1FF); // 512GB
        curr = get_or_alloc(ctx, curr, (virt_addr >> 30) & 0x1FF); // 1GB
        curr = get_or_alloc(ctx, curr, (virt_addr >> 21) & 0x1FF); // 2MB

        // 4KB
        // | 3 is because we make the pages both PRESENT and WRITABLE
        curr->entries[pte_index] = (phys_addr & 0xFFFFFFFFF000) | 3;
        phys_addr += PAGE_SIZE;
        virt_addr += PAGE_SIZE;
    }
}

static void map_pages_id(PageTableContext* ctx, u64 addr, u64 page_count) {
    map_pages(ctx, addr, addr, page_count);
}

static void mem_map_mark(MemMap* mem_map, u64 base, u64 npages, u64 type) {
    for(int i = 0; i != mem_map->nregions; ++i) {
        MemRegion* region = &mem_map->regions[i];
        size_t region_size = region->pages * 4096;
        // Find the region that contains the base
        if(!(region->base <= base && base < region->base + region_size)) {
            continue;
        }
        // If the specified range falls perfectly onto the bounds of
        // region, then simply change the type
        if(region->base == base && region->pages == npages) {
            region->type = type;
            return;
        }
        u64 old_type = region->type;
        // If the specified base isn't on the start of the region
        // we split the memory region into two regions
        if(base > region->base) {
            if(mem_map->nregions + 1 > mem_map->cap) {
                panic("Too many mem_map entries created!");
            }
            for(int j = mem_map->nregions; j-- > i+1;) {
                MemRegion* src = &mem_map->regions[j];
                MemRegion* dst = &mem_map->regions[j+1];
                *dst = *src;
            }
            size_t offset_pages = (base - region->base) / 0x1000;
            mem_map->nregions += 1;
            MemRegion* new_region = &mem_map->regions[i+1];
            new_region->base = base;
            new_region->pages = region->pages - offset_pages;
            new_region->type = type;
            region->pages = offset_pages;
            // Sync stuff
            i += 1;
            region = new_region;
        }
        // If the specified number of pages is smaller than size of the region
        // in pages, we split the region into two again
        if(npages < region->pages) {
            if(mem_map->nregions + 1 > mem_map->cap) {
                panic("Too many mem_map entries created!");
            }
            for(int j = mem_map->nregions; j-- > i+1;) {
                MemRegion* src = &mem_map->regions[j];
                MemRegion* dst = &mem_map->regions[j+1];
                *dst = *src;
            }
            mem_map->nregions += 1;
            MemRegion* new_region = &mem_map->regions[i+1];
            new_region->base = region->base + npages * 0x1000;
            new_region->pages = region->pages - npages;
            new_region->type = old_type;
            region->pages = npages;
        }
        return;

    }
    // If we can't find it within a region, we'll create a new mem_map entry
    if(mem_map->nregions + 1 > mem_map->cap) {
        panic("Too many mem_map entries created!");
    }
    // Find the index where to insert a new entry, has to be not in the map yet
    // otherwise the conclusion is that the range spans the boundary between
    // two mem_map regions, which isn't good
    int i = 1;
    for(; i < mem_map->nregions; ++i) {
        MemRegion prev_region = mem_map->regions[i-1];
        MemRegion next_region = mem_map->regions[i];
        u64 prev_end = prev_region.base + prev_region.pages * 0x1000;
        u64 next_start = next_region.base;
        if(prev_end <= base && base < next_start) {
            break;
        }
    }
    if(i == mem_map->nregions) {
        MemRegion last_region = mem_map->regions[mem_map->nregions-1];
        u64 last_region_end = last_region.base + last_region.pages * 0x1000;
        if(base < last_region_end) {
            panic("Specified region spans region boundary or there's no space in mem_map\n");
        }
    }
    // Move the entries from i to the last
    for(int j = mem_map->nregions; j-- > i;) {
        MemRegion* src = &mem_map->regions[j];
        MemRegion* dst = &mem_map->regions[j+1];
        *dst = *src;
    }
    // Insert the new entry
    mem_map->regions[i].base = base;
    mem_map->regions[i].pages = npages;
    mem_map->regions[i].type = type;
    mem_map->nregions += 1;
}

static void mem_map_merge_contiguous_ranges(MemMap* mem_map) {
    MemRegion* cmp_region = &mem_map->regions[0];
    int write_idx = 1;
    for(int i = 1; i < mem_map->nregions; ++i) {
        MemRegion* region = &mem_map->regions[i];
        // If this region and the compared region are contiguous and have the same
        // type, merge them
        if(region->type == cmp_region->type) {
            if(region->base == cmp_region->base + cmp_region->pages * 0x1000) {
                cmp_region->pages += region->pages;
                continue;
            }
        }
        // Otherwise commit the new cmp_region to mem_map at the index
        // and restart comparisons
        mem_map->regions[write_idx] = *region;
        cmp_region = &mem_map->regions[write_idx];
        write_idx += 1;
        continue;
    }
    mem_map->nregions = write_idx;
}

static bool mem_map_verify(MemMap* mem_map) {
    for(int i = 0; i != mem_map->nregions; ++i) {
        MemRegion* region = &mem_map->regions[i];
        u64 region_start = region->base;
        u64 region_end = region->base + region->pages * 0x1000;
        // Check region for overlapping ranges
        for(int j = 0; j != mem_map->nregions; ++j) {
            if(i == j) {
                continue;
            }
            MemRegion* cmp_region = &mem_map->regions[j];
            u64 cmp_region_start = cmp_region->base;
            u64 cmp_region_end = cmp_region->base + cmp_region->pages * 0x1000;
            if(cmp_region_start <= region_start && region_start < cmp_region_end) {
                return false;
            }
        }
    }
    return true;
}

EFI_STATUS efi_main(EFI_HANDLE img_handle, EFI_SYSTEM_TABLE* st) {
    EFI_STATUS status = st->ConOut->ClearScreen(st->ConOut);
    printf("[INFO] Booting\n");

    if(!com_init(PORT_COM1, COM_DEFAULT_BAUD)) {
        efi_println(st, L"Failed to initialize COM1 port output");
    }
    if(!com_init(PORT_COM2, COM_DEFAULT_BAUD)) {
        efi_println(st, L"Failed to initialize COM2 port output");
    }
    com_writes("COM1 port output success!\n");

    // Get linear framebuffer
    Framebuffer fb;
    {
        EFI_GRAPHICS_OUTPUT_PROTOCOL* graphics_output_protocol;
        EFI_GUID graphics_output_protocol_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

        status = st->BootServices->LocateProtocol(&graphics_output_protocol_guid, NULL, (void**)&graphics_output_protocol);
        if (status != 0) {
            panic("Error: Could not open protocol 3.\n\n");
        }

        fb.width  = graphics_output_protocol->Mode->Info->HorizontalResolution;
        fb.height = graphics_output_protocol->Mode->Info->VerticalResolution;
        fb.stride = graphics_output_protocol->Mode->Info->PixelsPerScanline;
        fb.pixels = (u32*)graphics_output_protocol->Mode->FrameBufferBase;
    }
    term_set_framebuffer(fb);
    term_set_wrap(true);
    printf("Framebuffer at %X\n", (u64) fb.pixels);

    // Load the kernel from disk
    char* kernel_buffer;
    size_t kernel_size = KERNEL_BUFFER_SIZE;
    {
        kernel_buffer = efi_alloc(st, KERNEL_BUFFER_SIZE);
        if (kernel_buffer == NULL) {
            panic("Failed to allocate space for kernel!\nStatus: %X\n", status);
        }

        EFI_GUID loaded_img_proto_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
        EFI_LOADED_IMAGE_PROTOCOL* loaded_img_proto;
        status = st->BootServices->OpenProtocol(img_handle, &loaded_img_proto_guid,
            (void**)&loaded_img_proto, img_handle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (status != 0) {
            panic("Failed to load img protocol!\nStatus: %X\n", status);
        }

        EFI_GUID simple_fs_proto_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
        EFI_HANDLE dev_handle = loaded_img_proto->DeviceHandle;
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* simple_fs_proto;
        status = st->BootServices->OpenProtocol(dev_handle, &simple_fs_proto_guid,
            (void**)&simple_fs_proto, img_handle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (status != 0) {
            panic("Failed to load fs protocol!\nStatus: %X\n", status);
        }

        EFI_FILE* fs_root;
        status = simple_fs_proto->OpenVolume(simple_fs_proto, &fs_root);
        if (status != 0) {
            panic("Failed to open fs root!\nStatus: %X\n", status);
        }

        EFI_FILE* kernel_file;
        status = fs_root->Open(fs_root, &kernel_file, (i16*)KERNEL_FILENAME, EFI_FILE_MODE_READ, EFI_FILE_READ_ONLY);
        if (status != 0) {
            panic("Failed to open kernel file!\nStatus: %X\n", status);
        }

        // Kernel buffer is right after the loader region
        kernel_file->Read(kernel_file, &kernel_size, kernel_buffer);
        if (kernel_size >= KERNEL_BUFFER_SIZE) {
            panic("Kernel too large to fit into buffer!\n");
        }

        // Verify ELF magic number
        if (memcmp(kernel_buffer, (u8[]) { 0x7F, 'E', 'L', 'F' }, 4) != 0) {
            panic("Kernel is not a valid ELF file!\n");
        }

        kernel_file->Close(kernel_file);
        fs_root->Close(fs_root);
    }

    // Load the kernel ELF
    ELF_Module kernel_module;
    if(!elf_load(st, kernel_buffer, &kernel_module)) {
        panic("Failed to load the kernel module");
    }
    printf("Loaded the kernel at: %X .. %X\n", kernel_module.phys_base, kernel_module.phys_base + kernel_size - 1);
    printf("Kernel entry: %X\n", kernel_module.entry_addr);

    // Create the stack for the kernel
    void* kstack_base = efi_alloc(st, KERNEL_STACK_SIZE * 2);

    // align kernel stack
    kstack_base = (void*) ((uintptr_t) kstack_base & -KERNEL_STACK_SIZE);

    void* kstack_end  = (u8*) kstack_base + KERNEL_STACK_SIZE;
    printf("Kernel stack: %X .. %X\n", kstack_base, kstack_end);

    // Allocate space for our page tables before we exit UEFI
    size_t page_tables_count = (16ull * 1024 * 1024) / 4096;
    printf("Allocating %X bytes for page tables\n", page_tables_count * 4096);
    PageTable* page_tables = efi_alloc_pages(st, page_tables_count);
    if (page_tables == NULL) {
        panic("Failed to allocate space for page tables!\n", status);
    }
    memset(&page_tables[0], 0, sizeof(PageTable));
    PageTableContext ctx = {
        .capacity = page_tables_count, .used = 1, .tables = page_tables
    };

    // Grab the ACPI table info
    u64 rsdp = 0;
    EFI_CONFIGURATION_TABLE *tables = st->ConfigurationTables;
    EFI_GUID acpi_guid = EFI_ACPI_20_TABLE_GUID;
    for (int i = 0; i < st->NumberOfTableEntries; i++) {
        if (!memcmp(&acpi_guid, &tables[i].VendorGuid, sizeof(EFI_GUID))) {
            rsdp = (u64)tables[i].VendorTable;
            break;
        }
    }
    if (rsdp == 0) {
        panic("Failed to get RDSP!");
    }

    kernel_boot_info.rsdp_addr = rsdp;
    printf("RSDP: %X\n", rsdp);
    printf("Beginning EFI handoff...\n");

    // Load latest memory map
    size_t fb_size_pages = PAGE_4K(fb.width * fb.height * sizeof(u32));
    size_t map_key;
    MemMap mem_map = efi_get_mem_map(st, &map_key);
    // TODO(flysand): there seems to be a bug in mem_map_mark, if we switch around the two lines below
    // the function refuses to remap the kernel memory region. Gotta investigate that when I'm not sleepy
    mem_map_mark(&mem_map, kernel_module.phys_base, PAGE_4K(kernel_module.size), MEM_REGION_KERNEL);
    mem_map_mark(&mem_map, (u64) kstack_base, PAGE_4K(KERNEL_STACK_SIZE), MEM_REGION_KSTACK);
    mem_map_mark(&mem_map, (u64) fb.pixels, fb_size_pages, MEM_REGION_FRAMEBUFFER);
    mem_map_merge_contiguous_ranges(&mem_map);
    if(!mem_map_verify(&mem_map)) {
        panic("MemMap contains overlapping ranges");
    }

    status = st->BootServices->ExitBootServices(img_handle, map_key);
    if (status != 0) {
        panic("Failed to exit EFI\nStatus: %X", status);
    }

    // Print memory map
    if (1) {
        printf("Memory map (%d entries):\n", mem_map.nregions);
        for(int i = 0; i != mem_map.nregions; ++i) {
            MemRegion region = mem_map.regions[i];
            char* name = mem_region_name(region.type);
            printf("%d: %X .. %X [%s]\n", i, region.base, region.base + region.pages*4096, name);
        }
    }

    kernel_boot_info.identity_map_ptr = (kernel_module.virt_base + kernel_module.size + 0x40000000 - 1) & -0x40000000;
    printf("Identity map @ %X\n", kernel_boot_info.identity_map_ptr);

    // Generate the page tables
    // Map everything in the memory map that's ever been allocated
    for(int i = 0; i != mem_map.nregions; ++i) {
        MemRegion region = mem_map.regions[i];

        // mirror the kernel into the higher half
        if(region.base == kernel_module.phys_base) {
            if(region.type != MEM_REGION_KERNEL) {
                panic("Something bad is located at kernel's paddr");
            }
            map_pages(&ctx, kernel_module.virt_base, region.base, region.pages);
        }

        map_pages(&ctx, kernel_boot_info.identity_map_ptr + region.base, region.base, region.pages);
    }

    // Map the loader's trampoline page in, when loader.s installs the new address space we need the
    // code there to stick around long enough to jump away.
    printf("  Making trampoline mapping %X\n", kernel_module.phys_base + kernel_module.entry_addr);
    map_pages_id(&ctx, kernel_module.phys_base + kernel_module.entry_addr, 1);
    map_pages(&ctx, kernel_boot_info.identity_map_ptr + (uintptr_t) &kernel_boot_info, (uintptr_t) &kernel_boot_info, (sizeof(BootInfo) + 4095) / 4096);

    // memset(framebuffer, 0, framebuffer_stride * framebuffer_height * sizeof(u32));
    for (size_t j = 0; j < 50; j++) {
        for (size_t i = 0; i < 50; i++) {
            fb.pixels[i + (j * fb.stride)] = 0xFFFF7F1F;
        }
    }

    #if 0
    printf("TSS descriptor:\n");
    memdump(tss, 16);
    #endif

    kernel_boot_info.elf_virtual_ptr = kernel_module.virt_base;
    kernel_boot_info.elf_physical_ptr = kernel_module.phys_base;
    kernel_boot_info.elf_mapped_size = kernel_module.size;
    kernel_boot_info.fb = fb;
    kernel_boot_info.mem_map = mem_map;
    kernel_boot_info.kernel_pml4 = &page_tables[0];
    kernel_boot_info.cores[0].kernel_stack = kstack_base;
    kernel_boot_info.cores[0].kernel_stack_top = kstack_end;

    // GDT setup
    {
        u64 gdt_base = kernel_boot_info.identity_map_ptr + (uintptr_t) kernel_boot_info.cores[0].gdt;
        kernel_boot_info.cores[0].gdt_pointer[0] = sizeof(kernel_boot_info.cores[0].gdt) - 1;
        memcpy(&kernel_boot_info.cores[0].gdt_pointer[1], &gdt_base, sizeof(u64));

        u64* gdt = kernel_boot_info.cores[0].gdt;
        gdt[0] = 0;
        gdt[1] = 0xAF9A000000FFFF; // kernel CS
        gdt[2] = 0xCF92000000FFFF; // kernel DS
        gdt[3] = 0xCFF2000000FFFF; // user DS
        gdt[4] = 0xAFFA000000FFFF; // user CS
        gdt[5] = 0;                // TSS
        gdt[6] = 0;                // TSS
    }

    uint32_t* sp = (uint32_t*) kstack_base;
    sp[0] = KERNEL_STACK_COOKIE;
    sp[1] = 0;

    LoaderFunction loader = (LoaderFunction) (kernel_boot_info.elf_physical_ptr + kernel_module.entry_addr);
    // printf("TSS_PTR=%X\n", tss_ptr);
    // printf("Jumping to the kernel: %X (sp=%X)\n", loader, kstack_end);

    kernel_boot_info.elf_virtual_entry = kernel_boot_info.elf_virtual_ptr + kernel_module.entry_addr;
    loader(&kernel_boot_info, kstack_end, kernel_boot_info.identity_map_ptr + (uintptr_t) kernel_boot_info.cores[0].gdt_pointer);
    return 0;
}
