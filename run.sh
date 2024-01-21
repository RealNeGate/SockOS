#!/bin/sh

qemu-system-x86_64 \
    -serial stdio \
    -bios OVMF.fd \
    -device piix3-ide,id=ide \
    -drive format=raw,file=fat:rw:bin \
    -device ide-hd,drive=disk,bus=ide.0 \
    -m 256M \
    -smp cores=4,threads=1,sockets=1 \
    -no-reboot -s \
    -d int \
    -S \
    -D qemu.log
