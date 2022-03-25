#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "efi.h"
#include "elf.h"

int itoa(uint32_t i, uint8_t base, uint16_t* buf) {
	static const char bchars[] = "0123456789ABCDEF";
	
    int pos = 0;
    int o_pos = 0;
    int top = 0;
	uint16_t tbuf[32];
	
    if (i == 0 || base > 16) {
        buf[0] = '0';
        buf[1] = '\0';
        return 0;
    }
	
    while (i != 0) {
        tbuf[pos] = bchars[i % base];
        pos++;
        i /= base;
    }
    top = pos--;
	
    for (o_pos = 0; o_pos < top; pos--, o_pos++) {
        buf[o_pos] = tbuf[pos];
    }
    buf[o_pos] = 0;
	return o_pos;
}

void printhex(EFI_SYSTEM_TABLE *st, uint32_t number) {
	uint16_t buffer[32];
	itoa(number, 16, buffer);
	buffer[31] = 0;
	
	st->ConOut->OutputString(st->ConOut, (int16_t *)buffer);
	st->ConOut->OutputString(st->ConOut, (int16_t *)L"\n\r");
}

void println(EFI_SYSTEM_TABLE *st, uint16_t *str) {
	st->ConOut->OutputString(st->ConOut, (int16_t *)str);
	st->ConOut->OutputString(st->ConOut, (int16_t *)L"\n\r");
}

#define panic(x, y) do { println((x), (y)); return 1; } while (false);

#define LOADER_BUFFER_SIZE (1 * 1024 * 1024)
#define KERNEL_BUFFER_SIZE (1 * 1024 * 1024)

#define MEM_MAP_BUFFER_SIZE (16 * 1024)
static _Alignas(4096) char mem_map_buffer[MEM_MAP_BUFFER_SIZE];

#define PAGE_SIZE 4096

typedef struct {
	uintptr_t base, pages;
} MemRegion;

#define MAX_MEM_REGIONS (1024)
static MemRegion mem_regions[MAX_MEM_REGIONS];

typedef struct {
	uint64_t entries[512];
} PageTable;

// This is all the crap we throw into the loader
typedef struct {
	void* entrypoint;
	PageTable* kernel_pml4;
	
	size_t mem_region_count;
	MemRegion* mem_regions;
	
	struct {
		uint32_t width, height;
		uint32_t stride; // in pixels
		uint32_t* pixels;
		// TODO(NeGate): pixel format :p
	} fb;
} BootInfo;

typedef void(*LoaderFunction)(BootInfo* info);

void* memset(void* buffer, int c, size_t n) {
	uint8_t* buf = (uint8_t*) buffer;
	for (size_t i = 0; i < n; i++) {
		buf[i] = c;
	}
	return (void*) buf;
}

void *memcpy(void *dest, const void *src, size_t n) {
	uint8_t* d = (uint8_t*) dest;
	uint8_t* s = (uint8_t*) src;
	for (size_t i = 0; i < n; i++) {
		d[i] = s[i];
	}
	return (void *)dest;
}

int memcmp(const void *a, const void *b, size_t n) {
	uint8_t* aa = (uint8_t*) a;
	uint8_t* bb = (uint8_t*) b;
	
	for (size_t i = 0; i < n; i++) {
		if (aa[i] != bb[i]) return aa[i] - bb[i];
	}
	
	return 0;
}

typedef struct {
	size_t capacity;
	size_t used;
	
	// zero slot is the root PML4 thingy
	PageTable* tables;
} PageTableContext;

static size_t estimate_page_table_count(size_t size) {
	size_t count = 0;
	count += (size+4095)       / 4096;       // 4KiB pages
	count += (size+2097151)    / 2097152;    // 2MiB pages
	count += (size+1073741823) / 1073741824; // 1GiB pages
	return count;
}

static _Alignas(4096) BootInfo boot_info;

static void print_memory_map(EFI_SYSTEM_TABLE* st, PageTable* table, int depth) {
	wchar_t buffer[64];
	for (int i = 0; i < depth; i++) {
		buffer[i*2 + 0] = ' ';
		buffer[i*2 + 1] = ' ';
	}
	
	buffer[depth*2 + 0] = '0';
	buffer[depth*2 + 1] = 'x';
	int pos = (depth*2 + 2) + itoa((uint64_t) table, 16, &buffer[depth*2 + 2]);
	
	buffer[pos + 0] = '\r';
	buffer[pos + 1] = '\n';
	st->ConOut->OutputString(st->ConOut, (int16_t *)buffer);
	
	if (depth >= 3) {
		return;
	}
	
	for (int i = 0; i < 512; i++) {
		if (table->entries[i]) {
			PageTable* p = (PageTable*)(table->entries[i] & 0xFFFFFFFFFFFFF000);
			print_memory_map(st, p, depth + 1);
		}
	}
}

static EFI_STATUS identity_map_some_pages(EFI_SYSTEM_TABLE* st, PageTableContext* ctx, uint64_t da_address, uint64_t page_count) {
	PageTable* address_space = &ctx->tables[0];
	if (da_address & 0x1FFull) {
		printhex(st, da_address);
		panic(st, L"Unaligned identity mapping");
	}
	
	// Generate the page table mapping
	uint64_t pml4_index = (da_address >> 39)  & 0x1FF; // 512GB
	uint64_t pdpte_index = (da_address >> 30) & 0x1FF; // 1GB
	uint64_t pde_index = (da_address >> 21)   & 0x1FF; // 2MB
	uint64_t pte_index = (da_address >> 12)   & 0x1FF; // 4KB
	
	for (size_t i = 0; i < page_count; i++) {
		// 512GB
		PageTable* table_l3;
		if (address_space->entries[pml4_index] == 0) {
			// Allocate new L4 entry
			if (ctx->used+1 >= ctx->capacity) panic(st, L"Fuck L4!");
			
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
			if (ctx->used+1 >= ctx->capacity) panic(st, L"Fuck L3!");
			
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
			if (ctx->used+1 >= ctx->capacity) panic(st, L"Fuck L2!");
			
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
	println(st, L"Beginning EFI Boot...");
	
	// Load the kernel and loader from disk
	char* kernel_loader_region;
	{
		EFI_PHYSICAL_ADDRESS addr;
		status = st->BootServices->AllocatePages(AllocateAnyPages, EfiLoaderData, 0x200, &addr);
		if (status != 0) {
			printhex(st, status);
			panic(st, L"Failed to allocate space for loader + kernel!");
		}
		kernel_loader_region = (char*)addr;
		
		EFI_GUID loaded_img_proto_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
		EFI_LOADED_IMAGE_PROTOCOL *loaded_img_proto;
		status = st->BootServices->OpenProtocol(img_handle, &loaded_img_proto_guid, (void **)&loaded_img_proto, img_handle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
		if (status != 0) {
			printhex(st, status);
			panic(st, L"Failed to load img protocol!");
		}
		
		EFI_GUID simple_fs_proto_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
		EFI_HANDLE dev_handle = loaded_img_proto->DeviceHandle;
		EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *simple_fs_proto;
		status = st->BootServices->OpenProtocol(dev_handle, &simple_fs_proto_guid, (void **)&simple_fs_proto, img_handle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
		if (status != 0) {
			printhex(st, status);
			panic(st, L"Failed to load fs protocol!");
		}
		
		EFI_FILE *fs_root;
		status = simple_fs_proto->OpenVolume(simple_fs_proto, &fs_root);
		if (status != 0) {
			printhex(st, status);
			panic(st, L"Failed to open fs root!");
		}
		
		
		EFI_FILE *kernel_file;
		status = fs_root->Open(fs_root, &kernel_file, (int16_t *)L"kernel.o", EFI_FILE_MODE_READ, 0);
		if (status != 0) {
			printhex(st, status);
			panic(st, L"Failed to open kernel.o!");
		}
		
		// Kernel buffer is right after the loader region
		size_t size = KERNEL_BUFFER_SIZE;
		char *kernel_buffer = &kernel_loader_region[LOADER_BUFFER_SIZE];
		kernel_file->Read(kernel_file, &size, kernel_buffer);
		
		if (size >= KERNEL_BUFFER_SIZE) {
			panic(st, L"Kernel too large to fit into buffer!");
		}
		
		// Verify ELF magic number
		if (memcmp(kernel_buffer, (uint8_t[]){ 0x7F, 'E', 'L', 'F' }, 4) != 0) {
			panic(st, L"kernel.o is not a valid ELF file!");
		}
		
		EFI_FILE *loader_file;
		status = fs_root->Open(fs_root, &loader_file, (int16_t *)L"loader.bin", EFI_FILE_MODE_READ, 0);
		if (status != 0) {
			printhex(st, status);
			panic(st, L"Failed to open loader.bin!");
		}
		
		size = LOADER_BUFFER_SIZE;
		char *loader_buffer = &kernel_loader_region[0];
		loader_file->Read(loader_file, &size, loader_buffer);
		
		if (size >= LOADER_BUFFER_SIZE) {
			panic(st, L"Loader too large to fit into buffer!");
		}
		
		println(st, L"Loaded the kernel and loader!");
		printhex(st, (uint32_t) ((uintptr_t)loader_buffer));
	}
	
	// Get linear framebuffer
	{
        EFI_GRAPHICS_OUTPUT_PROTOCOL* graphics_output_protocol;
        EFI_GUID graphics_output_protocol_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
		
		status = st->BootServices->LocateProtocol(&graphics_output_protocol_guid, NULL, (void **) &graphics_output_protocol);
        if (status != 0) {
            panic(st, L"Error: Could not open protocol 3.\n");
        }
		
		boot_info.fb.width = graphics_output_protocol->Mode->Info->HorizontalResolution;
		boot_info.fb.height = graphics_output_protocol->Mode->Info->VerticalResolution;
		boot_info.fb.stride = graphics_output_protocol->Mode->Info->PixelsPerScanline;
		boot_info.fb.pixels = (uint32_t*) graphics_output_protocol->Mode->FrameBufferBase;
	}
	
	// Generate the kernel page tables
	//   they don't get used quite yet but it's
	//   far easier to just parse ELF in C than it is in assembly so we wanna get
	//   it out of the way
	//   
	//   we don't wanna keep everything from the UEFI memory map, only the relevant
	//   details so the memory map, framebuffer, the page tables themselves
	{
		char* elf_file = &kernel_loader_region[LOADER_BUFFER_SIZE];
		Elf64_Ehdr* elf_header = (Elf64_Ehdr*) elf_file;
		
		Elf64_Shdr* string_table = (Elf64_Shdr*) &elf_file[elf_header->e_shoff + (elf_header->e_shstrndx * elf_header->e_shentsize)];
		if (string_table->sh_type != SHT_STRTAB) {
			panic(st, L"No string table found in kernel.o");
		}
		
		// Figure out how much space we actually need
		size_t pages_necessary = 0;
		Elf64_Shdr* text_section = NULL;
		for (size_t i = 0; i < elf_header->e_shnum; i++) {
			Elf64_Shdr* section = (Elf64_Shdr*) &elf_file[elf_header->e_shoff + (i * elf_header->e_shentsize)];
			
			if (section->sh_flags & SHF_ALLOC) {
				char* name = &elf_file[string_table->sh_offset + section->sh_name];
				if (memcmp(name, ".text", 5) == 0) {
					text_section = section;
				}
				
				pages_necessary += (section->sh_size + (PAGE_SIZE-1)) / PAGE_SIZE;
			}
		}
		
		if (!text_section) {
			panic(st, L"No text section in kernel.o");
		}
		
		// Allocate said space plus enough space for page tables
		// Framebuffer is also kept so we wanna make some pages for it
		size_t page_tables_necessary = 
			estimate_page_table_count(pages_necessary) +
			estimate_page_table_count(boot_info.fb.width * boot_info.fb.height * 4) +
			estimate_page_table_count(MEM_MAP_BUFFER_SIZE) +
			estimate_page_table_count(4096) +
			1;
		
		EFI_PHYSICAL_ADDRESS addr;
		status = st->BootServices->AllocatePages(AllocateAnyPages, EfiLoaderData,
												 pages_necessary, &addr);
		if (status != 0) {
			printhex(st, status);
			panic(st, L"Failed to allocate space for kernel!");
		}
		char* section_memory = (char*) addr;
		
		// 512 entries per page
		status = st->BootServices->AllocatePages(AllocateAnyPages, EfiLoaderData,
												 page_tables_necessary, &addr);
		if (status != 0) {
			printhex(st, status);
			panic(st, L"Failed to allocate space for kernel!");
		}
		PageTable* page_tables = (PageTable*) addr;
		
		// Load stuff into those newly minted pages
		size_t pages_used = 0;
		for (size_t i = 0; i < elf_header->e_shnum; i++) {
			Elf64_Shdr* section = (Elf64_Shdr*) &elf_file[elf_header->e_shoff + (i * elf_header->e_shentsize)];
			
			if (section->sh_flags & SHF_ALLOC) {
				char* name = &elf_file[string_table->sh_offset + section->sh_name];
				char* dst_memory = &section_memory[pages_used*PAGE_SIZE];
				
				if (memcmp(name, ".text", 5) == 0) {
					// mark offbrand entrypoint
					// TODO(NeGate): We're assuming that the entrypoint is
					// at the start of .text... like ballers
					boot_info.entrypoint = dst_memory;
				}
				
				// Load ELF stuff into memory
				memcpy(dst_memory,
					   &elf_file[section->sh_offset],
					   section->sh_size);
				
				pages_used += (section->sh_size + (PAGE_SIZE-1)) / PAGE_SIZE;
			}
		}
		
		// TODO(NeGate): handle any relocations
		
		// setup root PML4 thingy
		boot_info.kernel_pml4 = &page_tables[0];
		
		memset(&page_tables[0], 0, sizeof(PageTable));
		PageTableContext ctx = {
			.capacity = page_tables_necessary,
			.used = 1,
			.tables = page_tables
		};
		
		// Identity map all the stuff we wanna keep
		if (identity_map_some_pages(st, &ctx, (uint64_t) section_memory, pages_necessary)) {
			return 1;
		}
		
		uint64_t fb_page_count = ((boot_info.fb.width * boot_info.fb.height * 4) + 4095) / 4096;
		if (identity_map_some_pages(st, &ctx, (uint64_t) boot_info.fb.pixels, fb_page_count)) {
			return 1;
		}
		
		if (identity_map_some_pages(st, &ctx, (uint64_t) &mem_map_buffer[0], MEM_MAP_BUFFER_SIZE + 4095) / 4096) {
			return 1;
		}
		
		if (identity_map_some_pages(st, &ctx, (uint64_t) &boot_info, 1)) {
			return 1;
		}
		
		print_memory_map(st, &page_tables[0], 0);
		println(st, L"Generated page tables!");
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
		status = st->BootServices->GetMemoryMap(&size,
												(EFI_MEMORY_DESCRIPTOR *)mem_map_buffer,
												&map_key, &desc_size,
												&desc_version);
		
		if (status != 0) {
			printhex(st, status);
			panic(st, L"Failed to get memory map!");
		}
		
		size_t desc_count = size / desc_size;
		size_t mem_region_count = 0;
		for (int i = 0; i < desc_count && mem_region_count < MAX_MEM_REGIONS; i++) {
			EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)(mem_map_buffer + (i * desc_size));
			
			if (desc->Type == EfiConventionalMemory && desc->PhysicalStart >= 0x300000) {
				/*println(st, L"Range:");
				printhex(st, desc->PhysicalStart);
				printhex(st, desc->NumberOfPages * 4096);
				println(st, L"");*/
				
				mem_regions[mem_region_count].base = desc->PhysicalStart;
				mem_regions[mem_region_count].pages = desc->NumberOfPages;
				mem_region_count++;
			}
		}
		mem_regions[mem_region_count].base = 0;
	}
	
	status = st->BootServices->ExitBootServices(img_handle, map_key);
	if (status != 0) {
		printhex(st, status);
		panic(st, L"Failed to exit EFI");
	}
	
	//memset(framebuffer, 0, framebuffer_stride * framebuffer_height * sizeof(uint32_t));
	for (size_t j = 0; j < 50; j++) {
		for (size_t i = 0; i < 50; i++) {
			boot_info.fb.pixels[i + (j * boot_info.fb.stride)] = 0xFFFF7F1F;
		}
	}
	
	// Boot the loader
	((LoaderFunction)kernel_loader_region)(&boot_info);
	return 0;
}
