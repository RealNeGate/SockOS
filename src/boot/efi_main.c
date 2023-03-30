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

#define panic(fmt, ...)       \
do {                          \
    printf(fmt, __VA_ARGS__); \
    halt();                   \
} while (false);

#include "efi.h"
#include "efi_util.c"
#include "elf.h"
#include "elf_loader.c"


#define PAGE_4K(x) (((x) + 0xFFF) / 0x1000)
#define PAGE_2M(x) (((x) + 0x1FFFFF) / 0x200000)
#define PAGE_1G(x) (((x) + 0x3FFFFFFF) / 0x40000000)

#define LOADER_BUFFER_SIZE (4 * 1024)
#define KERNEL_BUFFER_SIZE (16 * 1024 * 1024)

#define MEM_MAP_BUFFER_SIZE (16 * 1024)
static alignas(4096) char mem_map_buffer[MEM_MAP_BUFFER_SIZE];

#define MAX_MEM_REGIONS (1024)
static alignas(4096) MemRegion mem_regions[MAX_MEM_REGIONS];

typedef void (*LoaderFunction)(BootInfo* info, uint8_t* stack);

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

static alignas(4096) BootInfo boot_info;

#define KERNEL_STACK_SIZE 0x200000
static alignas(4096) uint8_t kernel_stack[KERNEL_STACK_SIZE];

inline static size_t align_up(size_t a, size_t b) {
    return a + (b - (a % b)) % b;
}

static EFI_STATUS identity_map_some_pages(EFI_SYSTEM_TABLE* st, PageTableContext* ctx, uint64_t da_address, uint64_t page_count) {
    PageTable* address_space = &ctx->tables[0];
    if (da_address & 0xFFFull) {
        panic("Unaligned identity mapping when mapping %X\n", da_address);
    }

    // Generate the page table mapping
    uint64_t pml4_index  = (da_address >> 39) & 0x1FF; // 512GB
    uint64_t pdpte_index = (da_address >> 30) & 0x1FF; // 1GB
    uint64_t pde_index   = (da_address >> 21) & 0x1FF; // 2MB
    uint64_t pte_index   = (da_address >> 12) & 0x1FF; // 4KB

    for (size_t i = 0; i < page_count; i++) {
        // 512GB
        PageTable* table_l3;
        if (address_space->entries[pml4_index] == 0) {
            // Allocate new L4 entry
            if (ctx->used + 1 >= ctx->capacity) panic("Fuck L4!\n");

            table_l3 = &ctx->tables[ctx->used++];
            memset(table_l3, 0, sizeof(PageTable));

            address_space->entries[pml4_index] = ((uint64_t)table_l3) | 3;
        } else {
            // Used the bits 51:12 as the table address
            table_l3 = (PageTable*)(address_space->entries[pml4_index] & 0xFFFFFFFFFFFFF000);
        }

        // 1GB
        PageTable* table_l2;
        if (table_l3->entries[pdpte_index] == 0) {
            // Allocate new L3 entry
            if (ctx->used + 1 >= ctx->capacity) panic("Fuck L3!\n");

            table_l2 = &ctx->tables[ctx->used++];
            memset(table_l2, 0, sizeof(PageTable));

            table_l3->entries[pdpte_index] = ((uint64_t)table_l2) | 3;
        } else {
            // Used the bits 51:12 as the table address
            table_l2 = (PageTable*)(table_l3->entries[pdpte_index] & 0xFFFFFFFFFFFFF000);
        }

        // 2MB
        PageTable* table_l1;
        if (table_l2->entries[pde_index] == 0) {
            // Allocate new L2 entry
            if (ctx->used + 1 >= ctx->capacity) panic("Fuck L2!\n");

            table_l1 = &ctx->tables[ctx->used++];
            memset(table_l1, 0, sizeof(PageTable));

            table_l2->entries[pde_index] = ((uint64_t)table_l1) | 3;
        } else {
            // Used the bits 51:12 as the table address
            table_l1 = (PageTable*)(table_l2->entries[pde_index] & 0xFFFFFFFFFFFFF000);
        }

        // 4KB
        // | 3 is because we make the pages both PRESENT and WRITABLE
        table_l1->entries[pte_index] = (da_address & 0xFFFFFFFFFFFFF000) | 3;
        da_address += PAGE_SIZE;

        pte_index++;
        if (pte_index >= 512) {
            pte_index = 0;
            pde_index++;
            if (pde_index >= 512) {
                pde_index = 0;
                pdpte_index++;
                if (pdpte_index >= 512) {
                    pdpte_index = 0;
                    pml4_index++;
                }
            }
        }
    }

    return 0;
}

EFI_STATUS efi_main(EFI_HANDLE img_handle, EFI_SYSTEM_TABLE* st) {
    EFI_STATUS status;

    status = st->ConOut->ClearScreen(st->ConOut);

    if(!com_init(COM_DEFAULT_BAUD)) {
        efi_println(st, L"Failed to initialize COM1 port output");
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
        fb.pixels = (uint32_t*)graphics_output_protocol->Mode->FrameBufferBase;
    }
    boot_info.fb = fb;
    term_set_framebuffer(fb);
    term_set_wrap(true);
    printf("Framebuffer at %X\n", (uint64_t) fb.pixels);

    // Load the kernel and loader from disk
    char* kernel_loader_region;
    {
        kernel_loader_region = efi_alloc(st, KERNEL_BUFFER_SIZE + LOADER_BUFFER_SIZE);
        if (kernel_loader_region == NULL) {
            panic("Failed to allocate space for loader + kernel!\nStatus: %x\n", status);
        }

        EFI_GUID loaded_img_proto_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
        EFI_LOADED_IMAGE_PROTOCOL* loaded_img_proto;
        status = st->BootServices->OpenProtocol(img_handle, &loaded_img_proto_guid,
            (void**)&loaded_img_proto, img_handle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (status != 0) {
            panic("Failed to load img protocol!\nStatus: %x\n", status);
        }

        EFI_GUID simple_fs_proto_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
        EFI_HANDLE dev_handle = loaded_img_proto->DeviceHandle;
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* simple_fs_proto;
        status = st->BootServices->OpenProtocol(dev_handle, &simple_fs_proto_guid,
            (void**)&simple_fs_proto, img_handle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (status != 0) {
            panic("Failed to load fs protocol!\nStatus: %x\n", status);
        }

        EFI_FILE* fs_root;
        status = simple_fs_proto->OpenVolume(simple_fs_proto, &fs_root);
        if (status != 0) {
            panic("Failed to open fs root!\nStatus: %x\n", status);
        }

        EFI_FILE* kernel_file;
        status = fs_root->Open(fs_root, &kernel_file, (int16_t*)L"kernel.elf", EFI_FILE_MODE_READ, 0);
        if (status != 0) {
            panic("Failed to open kernel.elf!\nStatus: %x\n", status);
        }

        // Kernel buffer is right after the loader region
        size_t size = KERNEL_BUFFER_SIZE;
        char*  kernel_buffer = &kernel_loader_region[LOADER_BUFFER_SIZE];
        kernel_file->Read(kernel_file, &size, kernel_buffer);
        if (size >= KERNEL_BUFFER_SIZE) {
            panic("Kernel too large to fit into buffer!\n");
        }

        // Verify ELF magic number
        if (memcmp(kernel_buffer, (uint8_t[]) { 0x7F, 'E', 'L', 'F' }, 4) != 0) {
            panic("kernel.o is not a valid ELF file!\n");
        }

        EFI_FILE* loader_file;
        status = fs_root->Open(fs_root, &loader_file, (int16_t*)L"loader.bin", EFI_FILE_MODE_READ, 0);
        if (status != 0) {
            panic("Failed to open loader.bin!\nStatus: %x\n", status);
        }

        size = LOADER_BUFFER_SIZE;
        char* loader_buffer = &kernel_loader_region[0];
        loader_file->Read(loader_file, &size, loader_buffer);

        if (size >= LOADER_BUFFER_SIZE) {
            panic("Loader too large to fit into buffer!\n");
        }

        printf("Loaded the kernel and loader at: %X\n", kernel_loader_region);
    }

    ELF_Module kernel_module;
    if(!elf_load(st, kernel_loader_region + LOADER_BUFFER_SIZE, &kernel_module)) {
        panic("Failed to load the kernel module");
    }
    printf("Loaded the kernel at: %X\n", kernel_module.phys_base);
    printf("Kernel entry: %X\n", kernel_module.entry_addr);

    // Find kernel main
    boot_info.entrypoint = (void*) kernel_module.entry_addr;
    boot_info.kernel_virtual_used = kernel_module.phys_base;

    // Generate the kernel page tables
    //   they don't get used quite yet but it's
    //   far easier to just parse ELF in C than it is in assembly so we wanna get
    //   it out of the way
    //
    //   we don't wanna keep everything from the UEFI memory map, only the relevant
    //   details so the memory map, framebuffer, the page tables themselves
    {
        // setup root PML4 thingy

        // Allocate said space plus enough space for page tables
        // Framebuffer is also kept so we wanna make some pages for it
        size_t page_tables_necessary =
            estimate_page_table_count(kernel_module.size) +
            estimate_page_table_count(boot_info.fb.width * boot_info.fb.height * 4) +
            estimate_page_table_count(LOADER_BUFFER_SIZE) +
            estimate_page_table_count(MAX_MEM_REGIONS * sizeof(MemRegion)) +
            estimate_page_table_count(sizeof(BootInfo)) +
            estimate_page_table_count(KERNEL_STACK_SIZE) + 1;

        page_tables_necessary += estimate_page_table_count(page_tables_necessary);

        // 512 entries per page
        PageTable* page_tables = efi_alloc_pages(st, page_tables_necessary);
        if (page_tables == NULL) {
            panic("Failed to allocate space for kernel!\nStatus: %x\n", status);
        }


        memset(&page_tables[0], 0, sizeof(PageTable));
        PageTableContext ctx = {
            .capacity = page_tables_necessary, .used = 1, .tables = page_tables
        };

        // Identity map all the stuff we wanna keep
        if (identity_map_some_pages(st, &ctx, kernel_module.phys_base, PAGE_4K(kernel_module.size))) {
            return 1;
        }

        uint64_t fb_page_count = PAGE_4K(boot_info.fb.width * boot_info.fb.height * 4);
        if (identity_map_some_pages(st, &ctx, (uint64_t) boot_info.fb.pixels, fb_page_count)) {
            return 1;
        }

        if (identity_map_some_pages(st, &ctx, (uint64_t) page_tables, PAGE_4K(page_tables_necessary * 8))) {
            return 1;
        }

        if (identity_map_some_pages(st, &ctx, (uint64_t)&kernel_loader_region[0], PAGE_4K(LOADER_BUFFER_SIZE))) {
            return 1;
        }

        if (identity_map_some_pages(st, &ctx, (uint64_t) &mem_regions[0], PAGE_4K(MAX_MEM_REGIONS * sizeof(MemRegion)))) {
            return 1;
        }

        if (identity_map_some_pages(st, &ctx, (uint64_t) &kernel_stack[0], PAGE_4K(KERNEL_STACK_SIZE))) {
            return 1;
        }

        if (identity_map_some_pages(st, &ctx, (uint64_t) &boot_info, 1)) {
            return 1;
        }
        boot_info.kernel_pml4 = &page_tables[0];
        // Free ELF file... maybe?
    }

    // Load latest memory map
    size_t map_key;
    {
        size_t desc_size;
        uint32_t desc_version;
        size_t size = MEM_MAP_BUFFER_SIZE;

        // you can't make any more UEFI calls after this GetMemoryMap because it'll
        // *apparently* cause some issues and change the map_key which is used to
        // actually exit the UEFI crap
        status = st->BootServices->GetMemoryMap(&size, (EFI_MEMORY_DESCRIPTOR*)mem_map_buffer, &map_key, &desc_size, &desc_version);

        if (status != 0) {
            panic("Failed to get memory map!\nStatus: %x", status);
        }

        size_t desc_count = size / desc_size;
        size_t mem_region_count = 0;
        for (int i = 0; i < desc_count && mem_region_count < MAX_MEM_REGIONS; i++) {
            EFI_MEMORY_DESCRIPTOR* desc = (EFI_MEMORY_DESCRIPTOR*)(mem_map_buffer + (i * desc_size));

            if (desc->Type == EfiConventionalMemory && desc->PhysicalStart >= 0x300000) {
                /*println(st, L"Range:");
                printhex(st, desc->PhysicalStart);
                printhex(st, desc->NumberOfPages * 4096);
                println(st, L"");*/

                mem_regions[mem_region_count].base  = desc->PhysicalStart;
                mem_regions[mem_region_count].pages = desc->NumberOfPages;
                mem_region_count++;
            }
        }
        mem_regions[mem_region_count].base = 0;

        boot_info.mem_regions = mem_regions;
        boot_info.mem_region_count = mem_region_count;
    }

    status = st->BootServices->ExitBootServices(img_handle, map_key);
    if (status != 0) {
        panic("Failed to exit EFI\nStatus: %x", status);
    }

    // memset(framebuffer, 0, framebuffer_stride * framebuffer_height * sizeof(uint32_t));
    for (size_t j = 0; j < 50; j++) {
        for (size_t i = 0; i < 50; i++) {
            boot_info.fb.pixels[i + (j * boot_info.fb.stride)] = 0xFFFF7F1F;
        }
    }

    printf("Jumping to the kernel...\n");
    ((LoaderFunction)kernel_loader_region)(&boot_info, kernel_stack + KERNEL_STACK_SIZE);
    return 0;
}
