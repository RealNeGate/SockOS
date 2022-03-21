#include "compile.h"

int main() {
	// don't wanna buffer stdout
	setvbuf(stdout, NULL, _IONBF, 0);
	
#if defined(__clang__)
	printf("Compiling on Clang %d.%d.%d...\n", __clang_major__, __clang_minor__, __clang_patchlevel__);
#else
#error "Must be compiling with clang"
#endif
	
	create_dir_if_not_exists("build/");
	
	// Compile EFI STUB
	cmd_append("clang -I efi -target x86_64-pc-win32-coff ");
	cmd_append("-fno-stack-protector -nostdlib -fshort-wchar -mno-red-zone ");
	cmd_append("-c src/efi_stub.c -o build/uefi.obj");
	if (cmd_dump(cmd_run())) return 1;
	
	// Compile Kernel
	cmd_append("clang -o build/kernel.o  -c src/kernel.c ");
	cmd_append("-fno-stack-protector -fpic -mno-red-zone -nostdlib ");
	cmd_append("--target=x86_64-pc-none-elf -march=x86-64");
	if (cmd_dump(cmd_run())) return 1;
	
	cmd_append("nasm -f bin -o build/loader.bin src/loader.s");
	if (cmd_dump(cmd_run())) return 1;
	
	cmd_append("lld-link -subsystem:efi_application -nodefaultlib -dll ");
	cmd_append("-entry:efi_main build/uefi.obj -out:build/boot.efi");
	if (cmd_dump(cmd_run())) return 1;
	
	cmd_append("wsl ./make_iso.sh");
	if (cmd_dump(cmd_run())) return 1;
	
	return 0;
}
