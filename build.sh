BUILDDIR=bin

rm -rf $BUILDDIR
mkdir $BUILDDIR

# build efi stub
clang -I efi -target x86_64-pc-win32-coff -fno-stack-protector -nostdlib -fshort-wchar -mno-red-zone -c src/efi_stub.c -o $BUILDDIR/uefi.o
lld-link -subsystem:efi_application -nodefaultlib -dll -entry:efi_main $BUILDDIR/uefi.o -out:$BUILDDIR/boot.efi

# build kernel
clang -o $BUILDDIR/kernel.o -fno-stack-protector -fpic -mno-red-zone -nostdlib -ffreestanding -c src/kernel.c
nasm -f bin -o $BUILDDIR/loader.bin src/loader.s
