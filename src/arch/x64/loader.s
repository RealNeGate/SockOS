extern kmain, kernel_tss
global _start, kernel_idle

; We got ourselves boot info in RCX
section text
_start:
    ; switch to new kernel stack
    mov rsp, rdx
    cli

    ; write TSS address (R9:R8)
    mov qword [rel gdt64.tss], r8
    mov qword [rel gdt64.tss + 8], r9

    ; setup GDT descriptor
    lea rax, qword [rel gdt64]
    mov [rel gdt64.pointer + 2], rax

    lgdt [rel gdt64.pointer]

    ; set TSS
    mov ax, 0x28
    ltr ax

    lea rax, [rel _start.reload_cs]
    mov qword [rel far_jumper], rax
    jmp qword [rel far_jumper]
_start.reload_cs:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov rdi, rcx
    call kmain
    hlt

kernel_idle:
    hlt
    jmp kernel_idle

section data
far_jumper:
    dq 0 ; filled in at runtime
    dw 0x08

%include "src/arch/x64/gdt.s"
