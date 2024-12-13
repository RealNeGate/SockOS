global bootstrap_start, bootstrap_transition, bootstrap_data_start, bootstrap_end, premain
extern gdt64.pointer

align 4096
bootstrap_data_end:
%define data_start (0x1000 + (bootstrap_data_start - bootstrap_start))

bits 16
bootstrap_start:
    cli

    ; Enable PAE
    mov eax, 0xA0
    mov cr4, eax

    ; Setup PML4
    mov edx, [data_start]
    mov cr3, edx

    ; Enable long mode
    mov ecx, 0xc0000080
    rdmsr
    or eax, 0x100
    wrmsr

    mov ebx, 0x80000011
    mov cr0, ebx

bits 64
    ; jump to the higher-half form of kmain
    jmp [data_start + 32]

bootstrap_transition:
    ; use GDT
    lgdt [rel gdt64.pointer]

    ; set TSS
    mov ax, 0x28
    ltr ax

    ; there's a weird bug around far jumps in Intel assembly syntax
    lea rax, [rel premain]
    mov qword [rel far_jumper], rax
    jmp far qword [rel far_jumper]

bits 64
align 8
bootstrap_data_start:
    dq 0 ; [0]  pml4
    dq 0 ; [8]  smp_main
    dq 0 ; [16] cores
    dq 0 ; [24] sizeof_core
    dq 0 ; [32] bootstrap_transition

far_jumper:
    dq 0 ; filled in at runtime
    dw 0x08

align 512
bits 64
premain:
    ; set up the TSS?
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; get core number
    mov eax, 1
    cpuid

    ; reserve core
    ;   cpu_index = ebx * sizeof(PerCPU)
    mov rdi, rbx
    shr rdi, 24
    imul rdi, [data_start + 24]
    ; get core pointer & stack
    add rdi, [data_start + 16]

    ; jump into C... finally!
    mov rsp, [rdi + 16] ; kernel_stack_top
    call [data_start + 8]
bootstrap_end:
