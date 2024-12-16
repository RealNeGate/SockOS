extern kmain, kernel_tss
global _start, kernel_idle

; We got ourselves boot info in RCX
section .text
align 4096
_start:
    ; switch to new kernel stack
    mov rsp, rdx
    cli

    ; switch page tables
    mov rax, [rcx + 0]
    mov rdx, [rcx + 8]
    mov r9,  [rcx + 16]
    mov cr3, rax

    ; jump to the higher-half form of kmain
    lea rax, [_start.transition - _start]
    add rax, rdx
    jmp rax
_start.transition:
    ; move pointers up
    add rcx, r9
    add rsp, r9

    ; setup GDT descriptor
    lgdt [r8]

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
    xor ebp, ebp
    call kmain
    hlt

kernel_idle:
    hlt
    jmp kernel_idle

section .data
far_jumper:
    dq 0 ; filled in at runtime
    dw 0x08
