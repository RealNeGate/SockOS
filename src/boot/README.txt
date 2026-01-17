
This is the UEFI code which is responsible for actually booting into the
kernel. It loads an ELF file (kernel.so), preps our own page tables and GDT, and brings
over the linear framebuffer.
