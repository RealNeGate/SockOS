global gdt64, gdt64_pointer, gdt64.tss

%define GDT_SIZE 0x38

section .data
align 4096
gdt64:
    dq 0 ; zero entry
gdt64.kernel_code_segment:
    dd 0xFFFF
    db 0
    dw 0xAF9A
    db 0
gdt64.kernel_data_segment:
    dd 0xFFFF
    db 0
    dw 0xCF92
    db 0
gdt64.user_data_segment:
    dd 0xFFFF
    db 0
    dw 0xCFF2
    db 0
gdt64.user_code_segment:
    dd 0xFFFF
    db 0
    dw 0xAFFA
    db 0
gdt64.tss:
    dq 0x00000000
    dq 0x00000000
gdt64_pointer:
    dw GDT_SIZE - 1 ; length
    dq 0            ; base
