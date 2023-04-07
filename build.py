import os
import sys
import platform
import shutil
import subprocess

system = platform.system()
if system == "Windows":
    ld = "ld.lld.exe"
else:
    ld = "ld.lld"

optimize = False
target = "x64"
clang_arch = "x86_64"

ldflags = "-pie -Map foo.map --nostdlib -T src/kernel/link"
cflags = f"-g -I src -target {clang_arch}-pc-linux-gnu -fpic -Wall -Wno-unused-function -fno-stack-protector -nodefaultlibs -mno-red-zone -nostdlib -ffreestanding -fno-finite-loops"

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

def ninja_cmd(inputs, str, out): ninja.write(f"build {out}: run {inputs}\n  cmd = {str}\n")
def ninja_cc(inputs, str, out):  ninja.write(f"build {out}: cc {inputs}\n  cmd = {str}\n")


# compile EFI stub
ninja_cc("src/boot/efi_main.c", f"clang src/boot/efi_main.c -c -o bin/efi.o {efi_cflags}", "bin/efi.o")
ninja_cmd("bin/efi.o", f"lld-link bin/efi.o -out:bin/boot.efi {efi_ldflags}", "bin/boot.efi")

# compile kernel
ninja_cmd(f"src/arch/{target}/irq.asm", f"nasm -f elf64 -o bin/asm_irq.o src/arch/{target}/irq.asm", "bin/asm_irq.o")
ninja_cmd(f"src/arch/{target}/loader.asm", f"nasm -f elf64 -o bin/loader.o src/arch/{target}/loader.asm", "bin/loader.o")

ninja_cc("src/kernel/kernel.c", f"clang src/kernel/kernel.c -c -o bin/kernel.o {cflags}\n", "bin/kernel.o")
ninja_cmd("bin/kernel.o bin/loader.o bin/asm_irq.o src/kernel/link", f"{ld} bin/kernel.o bin/loader.o bin/asm_irq.o -o bin/kernel.elf {ldflags}", "bin/kernel.elf")
ninja.close()

ret = subprocess.call(['ninja', '-v'])
if ret != 0:
    sys.exit(ret)

# move into proper directory structure for boot
os.makedirs("bin/os/EFI/BOOT", exist_ok = True)
shutil.copy("bin/kernel.elf", "bin/os")
shutil.copyfile("bin/boot.efi", "bin/os/EFI/BOOT/bootx64.efi")
