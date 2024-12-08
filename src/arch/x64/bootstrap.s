global bootstrap_start, bootstrap_entry, bootstrap_end, premain

align 4096
bootstrap_start:
bootstrap:
.pml4:
    dq 0
.smp_main:
    dq 0
.cores:
    dq 0
.sizeof_core:
    dq 0
.far_jumper_smp:
    dq 0 ; filled in at runtime
    dw 0x08

align 8
gdt64:
    dq 0 ; zero entry
.kernel_code_segment:
    dd 0xFFFF
    db 0
    dw 0xAF9A
    db 0
.kernel_data_segment:
    dd 0xFFFF
    db 0
    dw 0xCF92
    db 0
.user_data_segment:
    dd 0xFFFF
    db 0
    dw 0xCFF2
    db 0
.user_code_segment:
    dd 0xFFFF
    db 0
    dw 0xAFFA
    db 0
.tss:
    dq 0x00000000
    dq 0x00000000
.end:

gdtr64:
    dw (gdt64.end - gdt64) - 1 ; length
    dq 0                       ; a.intress

bootstrap_data_end:
%define data_start (0x1000 - bootstrap_data_end)

bits 16
bootstrap_entry:
    cli

    ; Enable PAE
    mov eax, 0xA0
    mov cr4, eax

    ; Setup PML4
    mov edx, [data_start + bootstrap.pml4]
    mov cr3, edx

    ; Enable long mode
    mov ecx, 0xc0000080
    rdmsr
    or eax, 0x100
    wrmsr

    mov ebx, 0x80000011
    mov cr0, ebx

    ; Set up GDT
    mov eax, data_start + gdt64
    mov [data_start + gdtr64 + 2], eax
    lgdt [data_start + gdtr64]

    ; there's a weird bug around far jumps in Intel assembly syntax
    jmp 0x8:data_start + premain

align 512
bits 64
premain:
    ; set up the TSS?
    mov ax, 0x10
    mov ds, ax
    mov ss, ax

    ; get core number
    mov eax, 1
    cpuid

    ; reserve core
    ;   cpu_index = ebx * sizeof(PerCPU)
    mov rdi, rbx
    shr rdi, 24
    imul rdi, [data_start + bootstrap.sizeof_core]
    ; get core pointer & stack
    add rdi, [data_start + bootstrap.cores]

    ; jump into C... finally!
    mov rsp, [rdi + 16] ; kernel_stack_top
    call [data_start + bootstrap.smp_main]
bootstrap_end:
