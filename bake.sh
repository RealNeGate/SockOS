# dd if=/dev/zero of=part.img bs=512 count=91669

mformat -i part.img -h 32 -t 32 -n 64 -c 1
mcopy -i part.img bin/* ::
mmd -i part.img ::/efi/boot
mcopy -i part.img bin/efi/boot/* ::/efi/boot/
dd if=part.img of=disk.img bs=512 count=91669 seek=2048 conv=notrunc
