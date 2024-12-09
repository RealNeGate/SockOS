ENTRY(_start)
SECTIONS
{
	. = 0x0;
	.text : {
		*(.text)
	}
	.rodata : {
		*(.rodata)
		*(.rodata.*)
	}
	.data : {
		 *(.data)
	}
	.driver : {
		_DRIVER_START = .;
		 *(.driver)
		_DRIVER_END   = .;
	}
	.bss : {
		*(.bss)
		*(COMMON)
	}
}

