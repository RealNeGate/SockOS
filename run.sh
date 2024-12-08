#!/bin/sh
qemu-system-x86_64 -serial stdio -bios OVMF.fd -drive format=raw,file=fat:rw:bin \
-no-reboot -d int -D qemu.log \
-m 256M -smp cores=4,threads=1,sockets=1
