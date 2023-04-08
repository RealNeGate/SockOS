.intel_syntax noprefix

.extern kmain, kernel_tss
.globl _start

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

.data
far_jumper:
    .quad 0 // filled in at runtime
    .word 0x08

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
    .word gdt64.pointer - gdt64 - 1 // length
    .quad 0 // a.intress
