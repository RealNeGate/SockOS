.intel_syntax noprefix

.extern kmain, kernel_tss
.globl _start, kernel_idle

// We got ourselves boot info in RCX
.text
_start:
    // switch to new kernel stack
    mov rsp, rdx
    cli

    // write TSS address (R9:R8)
    mov qword ptr [rip + gdt64.tss], r8
    mov qword ptr [rip + gdt64.tss + 8], r9

    // setup GDT descriptor
    lea rax, qword ptr [rip + gdt64]
    mov [rip + gdt64.pointer + 2], rax

    lgdt [rip + gdt64.pointer]

    // set TSS
    mov ax, 0x28
    ltr ax

    lea rax, [rip + _start.reload_cs]
    mov qword ptr [rip + far_jumper], rax
    jmp qword ptr [rip + far_jumper]
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

.data
far_jumper:
    .quad 0 // filled in at runtime
    .word 0x08

.include "src/arch/x64/gdt.s"
