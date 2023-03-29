qemu-system-x86_64 -serial stdio -bios OVMF.fd -drive format=raw,file=fat:rw:bin/os -no-reboot -s %1
