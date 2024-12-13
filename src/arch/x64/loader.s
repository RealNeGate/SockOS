extern kmain, kernel_tss, gdt64, gdt64_pointer, gdt64.tss
global _start, kernel_idle

; We got ourselves boot info in RCX
section .text
align 4096
_start:
    ; switch to new kernel stack
    mov rsp, rdx
    cli

    ; write TSS address (R9:R8)
    mov qword [rel gdt64.tss], r8
    mov qword [rel gdt64.tss + 8], r9

    ; switch page tables
    mov rax, [rcx + 0]
    mov rdx, [rcx + 8]
    mov r8,  [rcx + 16]
    mov cr3, rax

    ; jump to the higher-half form of kmain
    lea rax, [_start.transition - _start]
    add rax, rdx
    jmp rax
_start.transition:
    ; move pointers up
    add rcx, r8
    add rsp, r8

    ; setup GDT descriptor
    lea rax, qword [rel gdt64]
    mov [rel gdt64_pointer + 2], rax

    lgdt [rel gdt64_pointer]

    ; set TSS
    mov ax, 0x28
    ltr ax

    lea rax, [rel _start.reload_cs]
    mov qword [rel far_jumper], rax
    jmp far qword [rel far_jumper]
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

section .data
far_jumper:
    dq 0 ; filled in at runtime
    dw 0x08
