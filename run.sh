#!/bin/sh
qemu-system-x86_64 -bios OVMF/OVMF.fd -drive format=raw,file=fat:rw:bin \
-device qemu-xhci -device usb-kbd -device usb-mouse \
-no-reboot -d int -D qemu.log \
-m 256M -smp cores=4,threads=1,sockets=1 -serial stdio
