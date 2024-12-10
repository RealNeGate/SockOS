#!/usr/bin/env python

import os
import platform
import subprocess
from pathlib import Path

NINJA_SCRIPT: str = "build.ninja"

# Note(flysand): Apparently it doesn't work if it is specified without
# .exe on windows.
LD = 'ld.lld' if platform.system() != 'Windows' else 'ld.lld.exe'

CFLAGS = [
    '-Wall',
    '-std=gnu23',
    '-nostdlib',
    '-masm=intel',
    '-mno-red-zone',
    '-fno-stack-protector',
    '-fno-finite-loops',
    '-Wno-unused',
]
cflags = ' '.join(CFLAGS)

asm_files = {
    "kernel": [
        "src/arch/x64/loader.s",
        "src/arch/x64/bootstrap.s",
        "src/arch/x64/irq.s",
    ],
}

# Ninja build script output
file = open(NINJA_SCRIPT, "w")
file.write(f'rule cc\n')
file.write(f'  depfile = $out.d\n')
file.write(f'  command = clang $in $flags -c -MD -MF $out.d -o $out\n')
file.write(f'  description = CC $out\n')
file.write(f'\n')
file.write(f'rule ld\n')
file.write(f'  command = {LD} $in $flags -o $out\n')
file.write(f'  description = LINK $out\n')
file.write(f'\n')
file.write(f'rule link\n')
file.write(f'  command = lld-link $in $flags /out:$out\n')
file.write(f'  description = LINK $out\n')
file.write(f'\n')
file.write(f'rule nasm\n')
file.write(f'  command = nasm $in -f elf64 -o $out\n')
file.write(f'  description = NASM $out\n')
file.write(f'\n')

asm_outputs: dict[str, list[str]] = {"kernel": []}
for module in asm_files:
    for asm_file in asm_files[module]:
        asm_out = os.path.splitext(os.path.basename(asm_file))[0]+".o"
        asm_out = os.path.join("objs", asm_out)
        file.write(f'build {asm_out}: nasm {asm_file}\n')
        asm_outputs[module].append(asm_out)
file.write(f'\n')

if True:
    # build desktop
    file.write(f'build objs/desktop.o: cc userland/desktop.c\n')
    file.write(f'  flags = -target x86_64-linux-gnu -ffreestanding -nostdlib -fpic\n')
    file.write(f'\n')
    file.write(f'build userland/desktop.elf: ld objs/desktop.o\n')
    file.write(f'  flags = -T userland/link.ld\n')
    file.write(f'\n')

# Compile arch-specific objects
objs = []
for path in Path("src/arch/x64/").rglob("*.c"):
    if path.name == "x64.c":
        file.write(f'build objs/{path.name}.o: cc {path} | userland/desktop.elf\n')
    else:
        file.write(f'build objs/{path.name}.o: cc {path}\n')
    file.write(f'  flags = -I src -fPIC -target x86_64-linux-gnu -ffreestanding {cflags}\n')
    file.write(f'\n')
    objs.append("objs/" + path.name + ".o")

for path in Path("src/kernel/").rglob("*.c"):
    file.write(f'build objs/{path.name}.o: cc {path}\n')
    file.write(f'  flags = -I src -fPIC -target x86_64-linux-gnu -ffreestanding {cflags}\n')
    file.write(f'\n')
    objs.append("objs/" + path.name + ".o")

# build EFI app
file.write(f'build objs/efi.o: cc src/boot/efi_main.c\n')
file.write(f'  flags = -target x86_64-pc-win32-coff -fuse-ld=lld-link -I src -fno-PIC -fshort-wchar {cflags}\n')
file.write(f'\n')

file.write(f'build bin/efi/boot/bootx64.efi: link objs/efi.o\n')
file.write(f'  flags = -subsystem:efi_application -nodefaultlib -dll -entry:efi_main\n')
file.write(f'\n')

# file.write(f'build objs/kernel.o: cc src/kernel/kernel.c | userland/desktop.elf\n')
# file.write(f'  flags = -I src -fPIC -target x86_64-linux-gnu -ffreestanding {cflags}\n')
# file.write(f'\n')

kernel_asm = " ".join(asm_outputs["kernel"])
objs_str = " ".join(objs)
file.write(f'build bin/kernel.so | output.map: ld {objs_str} {kernel_asm} | link.ld\n')
file.write(f'  flags = -T link.ld -nostdlib -Map=output.map -pie\n')
file.write(f'\n')

file.close()

os.system('ninja')

