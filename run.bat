qemu-system-x86_64 -bios OVMF.fd -drive format=raw,file=fat:rw:bin -no-reboot -d int -D qemu.log -m 1024M -smp cores=1,threads=1,sockets=1 -serial stdio


