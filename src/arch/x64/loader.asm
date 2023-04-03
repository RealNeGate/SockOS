bits 64
default rel

extern kmain
global _start

; We got ourselves boot info in RCX
section .text
_start:
    ; switch to new kernel stack
    mov rsp, rdx
    cli

    ; setup GDT descriptor
    lea rax, [gdt64]
    mov [gdt64.pointer + 2], rax

    lgdt [gdt64.pointer]

    lea rax, [.reload_cs]
    mov [far_jumper], rax
    jmp far [far_jumper]
.reload_cs:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov rdi, rcx
    call kmain
    hlt

far_jumper:
    dq 0 ; filled in at runtime
    dw 0x08

gdt64:
    dq 0 ; zero entry
.kernel_code_segment: equ $ - gdt64
    dd 0xFFFF
    db 0
    dw 0xAF9A
    db 0
.kernel_data_segment: equ $ - gdt64
    dd 0xFFFF
    db 0
    dw 0xAF92
    db 0
.user_code_segment: equ $ - gdt64
    dd 0xFFFF
    db 0
    dw 0xAFFA
    db 0
.user_data_segment: equ $ - gdt64
    dd 0xFFFF
    db 0
    dw 0xAFF2
    db 0
.pointer:
    dw $ - gdt64 - 1 ; length
    dq 0 ; address
