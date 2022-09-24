-- build efi stub
local efi_cflags = "-target x86_64-pc-win32-coff -fno-stack-protector -nostdlib -fshort-wchar -mno-red-zone"
local efi = build.chain(
    build.foreach_chain(
        "src/efi_stub.c", "clang %f "..efi_cflags.." -c -o %o", "bin/%F.o"
    ),
    "lld-link -subsystem:efi_application -nodefaultlib -dll -entry:efi_main %i -out:%o",
    "bin/boot.efi"
)

-- build kernel
local loader = build.chain("src/loader.s", "nasm -f bin -o %o %i", "bin/loader.bin")

local kernel_cflags = "-target x86_64-pc-linux-gnu -fpic -fno-stack-protector -nodefaultlibs -mno-red-zone -nostdlib -ffreestanding"
local kernel = build.chain(
    build.foreach_chain(
        { "src/kernel.c" }, "clang %f "..kernel_cflags.." -c -o %o", "bin/%F.o"
    ), "ld.lld %i --nostdlib -e kmain -o %o", "bin/kernel.so"
)

if build.has_file_changed(kernel, nil) or build.has_file_changed(loader, nil) or build.has_file_changed(efi, nil) then
os.execute("wsl ./make_iso.sh")
end

build.done()
