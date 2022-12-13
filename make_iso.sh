set -x -o pipefail

BUILDDIR=bin

dd if=/dev/zero of=$BUILDDIR/efi.img bs=1k count=1440
mformat -i $BUILDDIR/efi.img -f 1440 ::
mmd -i $BUILDDIR/efi.img ::/EFI
mmd -i $BUILDDIR/efi.img ::/EFI/BOOT
mcopy -i $BUILDDIR/efi.img $BUILDDIR/EFI/BOOT/bootx64.efi ::/EFI/BOOT
mcopy -i $BUILDDIR/efi.img startup.nsh ::/
mcopy -i $BUILDDIR/efi.img $BUILDDIR/kernel.so ::/
mcopy -i $BUILDDIR/efi.img $BUILDDIR/loader.bin ::/

rm -rf $BUILDDIR/iso $BUILDDIR/cdimage.iso
mkdir $BUILDDIR/iso
cp $BUILDDIR/efi.img $BUILDDIR/iso
xorriso -as mkisofs -o $BUILDDIR/cdimage.iso -iso-level 3 -V UEFI $BUILDDIR/iso $BUILDDIR/efi.img -e efi.img -no-emul-boot
