#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "efi.h"

typedef struct VideoModeInformation {
	uint8_t valid : 1, edidValid : 1;
	uint8_t bitsPerPixel;
	uint16_t widthPixels, heightPixels;
	uint16_t bytesPerScanlineLinear;
	uint64_t bufferPhysical;
	uint8_t edid[128];
} VideoModeInformation;

void itoa(uint32_t i, uint8_t base, uint16_t* buf) {
	static const char bchars[] = "0123456789ABCDEF";
	
    int pos = 0;
    int o_pos = 0;
    int top = 0;
	uint16_t tbuf[32];
	
    if (i == 0 || base > 16) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
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
static char mem_map_buffer[MEM_MAP_BUFFER_SIZE];

typedef struct {
	uintptr_t base, pages;
} MemRegion;

#define MAX_MEM_REGIONS (1024)
static MemRegion mem_regions[MAX_MEM_REGIONS];

void *memset(void *buffer, int c, size_t n) {
	uint8_t *buf = (uint8_t *)buffer;
	for (size_t i = 0; i < n; i++) {
		buf[i] = c;
	}
	return (void *)buf;
}
void *memcpy(void *dest, const void *src, size_t n) {
	uint8_t *d = (uint8_t *)dest;
	uint8_t *s = (uint8_t *)src;
	for (size_t i = 0; i < n; i++) {
		d[i] = s[i];
	}
	return (void *)dest;
}
int memcmp(const void *a, const void *b, size_t n) {
	uint8_t *aa = (uint8_t *)a;
	uint8_t *bb = (uint8_t *)b;
	
	for (size_t i = 0; i < n; i++) {
		if (aa[i] != bb[i]) return aa[i] - bb[i];
	}
	
	return 0;
}

EFI_STATUS efi_main(EFI_HANDLE img_handle, EFI_SYSTEM_TABLE *st) {
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
		
		if (size == KERNEL_BUFFER_SIZE) {
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
		
		if (size == LOADER_BUFFER_SIZE) {
			panic(st, L"Loader too large to fit into buffer!");
		}
		
		println(st, L"Loaded the kernel and loader!");
		printhex(st, (uint32_t) ((uintptr_t)loader_buffer));
		printhex(st, (uint32_t) ((uintptr_t)kernel_buffer));
	}
	
#if 0
	// Get linear framebuffer
	uint32_t* framebuffer;
	size_t framebuffer_width, framebuffer_height, framebuffer_stride;
	{
        EFI_GRAPHICS_OUTPUT_PROTOCOL* graphics_output_protocol;
        EFI_GUID graphics_output_protocol_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
		
		status = st->BootServices->LocateProtocol(3, &graphics_output_protocol_guid, NULL, (void **) &graphics_output_protocol);
        if (status != 0) {
            panic(st, L"Error: Could not open protocol 3.\n");
        }
		
		framebuffer_width = graphics_output_protocol->Mode->Info->HorizontalResolution;
		framebuffer_height = graphics_output_protocol->Mode->Info->VerticalResolution;
        framebuffer_stride = graphics_output_protocol->Mode->Info->PixelsPerScanLine;
		framebuffer = (uint32_t*) graphics_output_protocol->Mode->FrameBufferBase;
	}
	
	// Generate the kernel page tables
	//   they don't get used quite yet but it's
	//   far easier to just parse ELF in C than it is in assembly so we wanna get
	//   it out of the way
	{
		char* elf_file = &kernel_loader_region[LOADER_BUFFER_SIZE];
		Elf64_Ehdr* elf_header = (Elf64_Ehdr*) elf_file;
		
		// Figure out how much space we actually need
		size_t pages_necessary = 0;
		for (size_t i = 0; i < elf_header->e_phnum; i++) {
			Elf64_Phdr* segment = (Elf64_Phdr*) &elf_file[elf_header->e_phoff + (i * elf_header->e_phentsize)];
			
			pages_necessary += ();
		}
		
		// Allocate said space
		
		
		// Slap some pages on it
		
	}
#endif
	
	// Load latest memory map
	size_t map_key;
	{
		size_t desc_size;
		uint32_t desc_version;
		size_t size = MEM_MAP_BUFFER_SIZE;
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
	
	// Boot the loader
	typedef void LoaderFunction(void* kernel_elf_file);
	((LoaderFunction*)kernel_loader_region)(&kernel_loader_region[LOADER_BUFFER_SIZE]);
	
	return 0;
}
