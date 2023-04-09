.set GDT_SIZE, 0x38
gdt64:
    .quad 0 // zero entry
gdt64.kernel_code_segment:
    .int 0xFFFF
    .byte 0
    .word 0xAF9A
    .byte 0
gdt64.kernel_data_segment:
    .int 0xFFFF
    .byte 0
    .word 0xCF92
    .byte 0
gdt64.user_data_segment:
    .int 0xFFFF
    .byte 0
    .word 0xCFF2
    .byte 0
gdt64.user_code_segment:
    .int 0xFFFF
    .byte 0
    .word 0xAFFA
    .byte 0
gdt64.tss:
    .quad 0x00000000
    .quad 0x00000000
gdt64.pointer:
    .word GDT_SIZE - 1 // length
    .quad 0 // a.intress
