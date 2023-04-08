.intel_syntax noprefix
.globl boostrap_start, bootstrap_end, bootstrap_pml4
.extern smp_main, smp_stack_base

.code16

bootstrap_start:
    cli

    // Enable PAE
    mov eax, 0xA0
    mov cr4, eax

    // Setup PML4
    mov edx, bootstrap_pml4
    mov cr3, edx

    // Enable long mode
    mov ecx, 0xc0000080
    rdmsr
    or eax, 0x100
    wrmsr

    mov ebx, 0x80000011
    mov cr0, ebx

    // Set up GDT
    // lgdt <something>

    // Jump to long mode
    // jmp 0x8:premain

.code64
premain:
    // set up the TSS?
    mov ax, 0x10
    mov ds, ax
    mov ss, ax
    mov ax, 0x2b
    ltr ax

    mov rsp, smp_stack_base
    call smp_main

bootstrap_end:
