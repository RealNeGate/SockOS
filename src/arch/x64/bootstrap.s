.intel_syntax noprefix
.globl bootstrap_start, bootstrap_entry, bootstrap_end, premain
.align 4096

.org 0
.code16
bootstrap_start:
bootstrap_pml4:
    .quad 0
smp_main:
    .quad 0
cores:
    .quad 0
sizeof_core:
    .quad 0
counter:
    .quad 0
far_jumper_smp:
    .quad 0 // filled in at runtime
    .word 0x08
.align 8
.include "src/arch/x64/gdt.s"
bootstrap_data_end:

// layout stuff
.set BOOTSTRAP_DATA, (0x1000 - (bootstrap_data_end - bootstrap_start))
.set BOOTSTRAP_PML4,      BOOTSTRAP_DATA + 0
.set BOOTSTRAP_SMP,       BOOTSTRAP_DATA + 8
.set BOOTSTRAP_CORES,     BOOTSTRAP_DATA + 16
.set BOOTSTRAP_SIZE_CORE, BOOTSTRAP_DATA + 24
.set BOOTSTRAP_COUNTER,   BOOTSTRAP_DATA + 32
.set BOOTSTRAP_FARJMP,    BOOTSTRAP_DATA + 40
.set BOOTSTRAP_GDT64,     BOOTSTRAP_DATA + 56
.set BOOTSTRAP_GDT64PTR,  BOOTSTRAP_DATA + 56 + 38

bootstrap_entry:
    cli

    // Enable PAE
    mov eax, 0xA0
    mov cr4, eax

    // Setup PML4
    mov edx, [BOOTSTRAP_PML4] // bootstrap_pml4
    mov cr3, edx

    // Enable long mode
    mov ecx, 0xc0000080
    rdmsr
    or eax, 0x100
    wrmsr

    mov ebx, 0x80000011
    mov cr0, ebx

    // Set up GDT
    mov eax, BOOTSTRAP_GDT64
    mov dword ptr [BOOTSTRAP_GDT64PTR + 2], eax
    data32 lgdt [BOOTSTRAP_GDT64] // gdt64

    // there's a weird bug around far jumps in Intel assembly syntax
    .att_syntax
    data32 ljmpl $0x08,$0x1186
    .intel_syntax noprefix
.align 512
.code64
premain:
    mov dx, 0x3f8
    mov al, 'd'
    out dx, al

    // set up the TSS?
    mov ax, 0x10
    mov ds, ax
    mov ss, ax

    // reserve core
    //   cpu_index = atomic_add(&counter, 1)
    mov rax, [0x1020]   // counter
    mov rcx, 1
    xadd [rax], rcx
    //   cpu_index *= sizeof(PerCPU)
    mov r8, [0x1018]    // sizeof_core
    imul rcx, r8
    // get core pointer & stack
    mov rdx, [0x1010]   // cores
    add rcx, rdx

    // jump into C... finally!
    mov rsp, [rcx + 8] // kernel_stack
    call smp_main
bootstrap_end:
