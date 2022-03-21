set -x -o pipefail

dd if=/dev/zero of=build/efi.img bs=1k count=1440
mformat -i build/efi.img -f 1440 ::
mmd -i build/efi.img ::/EFI
mmd -i build/efi.img ::/EFI/BOOT
mcopy -i build/efi.img build/boot.efi ::/EFI/BOOT
mcopy -i build/efi.img startup.nsh ::/
mcopy -i build/efi.img build/kernel.o ::/
mcopy -i build/efi.img build/loader.bin ::/

rm -rf build/iso build/cdimage.iso
mkdir build/iso
cp build/efi.img build/iso
xorriso -as mkisofs -o build/cdimage.iso -iso-level 3 -V UEFI build/iso build/efi.img -e efi.img -no-emul-boot
