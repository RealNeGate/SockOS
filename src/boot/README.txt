
This is the UEFI stub which is responsible for actually booting into the
kernel. It creates page tables for the ELF binary and then uses the machine
code in loader.bin to jump out of the UEFI's GDT and page tables while
bringing over the linear framebuffer since it'll come in hand for graphics.
