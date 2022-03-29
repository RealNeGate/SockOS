#include "compile.h"

#if !defined(__clang__)
#error "Must be compiling with clang"
#endif

#define EFI_CFLAGS \
"-target x86_64-pc-win32-coff " \
"-fno-stack-protector " \
"-nostdlib " \
"-fshort-wchar " \
"-mno-red-zone "

#define KERNEL_CFLAGS \
"-target x86_64-pc-none-elf " \
"-fno-stack-protector " \
"-fpic " \
"-ffreestanding " \
"-nostdlib " \
"-march=x86-64 " \
"-mno-red-zone "

#define LLD_FLAGS \
"-subsystem:efi_application " \
"-nodefaultlib " \
"-dll " \
"-entry:efi_main "

int main() {
	// don't wanna buffer stdout
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("Compiling with Clang %d.%d.%d...\n", __clang_major__, __clang_minor__, __clang_patchlevel__);
	
	create_dir_if_not_exists("bin/");
	
	// Compile EFI STUB
	cmd_append("clang -c src/efi_stub.c -o bin/uefi.obj ");
	cmd_append(EFI_CFLAGS);
	if (cmd_dump(cmd_run())) return 1;
	
	// Compile Kernel
	cmd_append("clang -o bin/kernel.o -c src/kernel.c ");
	cmd_append(KERNEL_CFLAGS);
	if (cmd_dump(cmd_run())) return 1;
	
	cmd_append("nasm -f bin -o bin/loader.bin src/loader.s");
	if (cmd_dump(cmd_run())) return 1;
	
	cmd_append("lld-link bin/uefi.obj -out:bin/boot.efi ");
	cmd_append(LLD_FLAGS);
	if (cmd_dump(cmd_run())) return 1;
	
#if __linux__
	cmd_append("make_iso.sh");
	if (cmd_dump(cmd_run())) return 1;
#else
	cmd_append("wsl ./make_iso.sh");
	if (cmd_dump(cmd_run())) return 1;
#endif
	
	return 0;
}
