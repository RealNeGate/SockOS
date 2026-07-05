ENTRY(_start)
SECTIONS
{
	. = 0x100000;
	.text : ALIGN(4096) {
		*(.text)
	}
	.rodata : ALIGN(4096) {
		*(.rodata)
		*(.rodata.*)
	}
	.data : ALIGN(4096) {
		 *(.data)
	}
	.bss : ALIGN(4096) {
		*(.bss)
		*(COMMON)
	}
}
