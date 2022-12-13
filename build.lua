-- https://github.com/RealNeGate/Truct
build.mkdir("bin/EFI")
build.mkdir("bin/EFI/BOOT")

local efi_cc = "clang -target x86_64-pc-win32-coff"
local kernel_cc = "clang -target x86_64-pc-linux-gnu -fpic "

-- build efi stub
local efi_cflags = " -fno-stack-protector -nostdlib -fshort-wchar -mno-red-zone"
local efi = build.chain(
    build.foreach_chain(
        "src/efi_stub.c", efi_cc.." %f "..efi_cflags.." -c -o %o", "bin/%F.o"
    ),
    "lld-link -subsystem:efi_application -nodefaultlib -dll -entry:efi_main %i -out:%o",
    "bin/EFI/BOOT/bootx64.efi"
)

-- build kernel
local loader = build.chain("src/loader.s", "nasm -f bin -o %o %i", "bin/loader.bin")

-- x64 specific
local x64 = build.chain("src/arch/x64/irq.asm", "nasm -f elf64 -o %o %i", "bin/irq.o")

local kernel_cflags = "--nocrt"
local kernel_cflags = "-fno-stack-protector -nodefaultlibs -mno-red-zone -nostdlib -ffreestanding"
local objs = build.foreach_chain({ "src/kernel.c" }, kernel_cc.." %f "..kernel_cflags.." -c -o %o", "bin/%F.o")
table.insert(objs, x64)

build.chain(objs, "ld.lld %i --nostdlib -e kmain -o %o", "bin/kernel.so")
build.done()
