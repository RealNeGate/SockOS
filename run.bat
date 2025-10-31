qemu-system-x86_64 -bios OVMF.fd -drive format=raw,file=fat:rw:bin -no-reboot -d int -D qemu.log -m 1024M -smp cores=6,threads=1,sockets=1 -serial stdio -serial file:perf.spall
dd if="perf.spall" of="perf2.spall" bs=1 skip=367


