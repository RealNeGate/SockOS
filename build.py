import os
import platform
import shutil
import subprocess

optimize = False
target = "x64"
clang_arch = "x86_64"

ldflags = "--nostdlib -e kmain"
cflags = f"-g -I src -target {clang_arch}-pc-linux-gnu -fpic -Wall -Werror -Wno-unused-function -fno-stack-protector -nodefaultlibs -mno-red-zone -nostdlib -ffreestanding"

efi_cflags = f"-I src -target {clang_arch}-pc-win32-coff -fno-stack-protector -nostdlib -fshort-wchar -mno-red-zone"
efi_ldflags = "-subsystem:efi_application -nodefaultlib -dll -entry:efi_main"

if optimize:
	cflags += " -O2 -DNDEBUG"
	efi_cflags += " -O2 -DNDEBUG"

# write some rules
ninja = open('build.ninja', 'w')
ninja.write("""
rule run
  command = $cmd

rule cc
  command = $cmd -MD -MF $out.d
  depfile = $out.d
""")

def ninja_cmd(inputs, str, out):
	ninja.write(f"build {out}: run {inputs}\n  cmd = {str}\n")

def ninja_cc(inputs, str, out):
	ninja.write(f"build {out}: cc {inputs}\n  cmd = {str}\n")

# compile EFI stub
ninja_cc("src/boot/efi_main.c", f"clang src/boot/efi_main.c -c -o bin/efi.o {efi_cflags}", "bin/efi.o")
ninja_cmd("bin/efi.o", f"lld-link bin/efi.o -out:bin/boot.efi {efi_ldflags}", "bin/boot.efi")

# compile kernel
ninja_cmd(f"src/boot/loader_{target}.asm", f"nasm -f bin -o bin/loader.bin src/boot/loader_{target}.asm", "bin/loader.bin")

ninja_cmd(f"src/arch/{target}/core.asm", f"nasm -f elf64 -o bin/asm.o src/arch/{target}/core.asm", "bin/asm.o")
ninja_cc("src/kernel/kernel.c", f"clang src/kernel/kernel.c -c -o bin/kernel.o {cflags}\n", "bin/kernel.o")
ninja_cmd("bin/kernel.o bin/asm.o", f"ld.lld.exe bin/kernel.o bin/asm.o -o bin/kernel.so {ldflags}", "bin/kernel.so")
ninja.close()

subprocess.call(['ninja'])

# move into proper directory structure for boot
os.makedirs("bin/os/EFI/BOOT", exist_ok = True)
shutil.copy("bin/loader.bin", "bin/os")
shutil.copy("bin/kernel.so", "bin/os")
shutil.copyfile("bin/boot.efi", "bin/os/EFI/BOOT/bootx64.efi")
