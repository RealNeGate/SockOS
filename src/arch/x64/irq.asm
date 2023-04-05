
bits 64

global irq_enable
global irq_disable
global IDT_Descriptor
global asm_int_handler

extern irq_int_handler
extern boot_info

section .text

irq_enable:
    lidt [rdi]
    sti
    ret

irq_disable:
    cli
    ret

%macro DEFINE_INT 1
global isr%1
isr%1:
    push 0 ; Padded out
    push %1
    jmp asm_int_handler
%endmacro

%macro DEFINE_ERR 1
global isr%1
isr%1:
    push %1
    jmp asm_int_handler
%endmacro

DEFINE_INT 3
DEFINE_ERR 8
DEFINE_INT 9
DEFINE_ERR 13
DEFINE_ERR 14
DEFINE_INT 32
DEFINE_INT 33
DEFINE_INT 34
DEFINE_INT 35
DEFINE_INT 36
DEFINE_INT 37
DEFINE_INT 38
DEFINE_INT 39
DEFINE_INT 40
DEFINE_INT 41
DEFINE_INT 42
DEFINE_INT 43
DEFINE_INT 44
DEFINE_INT 45
DEFINE_INT 46
DEFINE_INT 47
DEFINE_INT 112
asm_int_handler:
    cld

    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; switch to kernel PML4
    mov rsi, cr3
    mov rax, [boot_info + 0]
    mov cr3, rax

    ; fxsave needs to be aligned to 16bytes
    ; mov rbx,rsp
    ; and rsp,~0xF
    ; fxsave  [rsp - 512]
    ; mov rsp,rbx
    ; sub rsp,512 + 16

    mov rdi, rsp
    call irq_int_handler

    ; fxrstor also needs to be aligned to 16bytes
    ; add rsp, 512 + 16
    ; mov rbx,rsp
    ; and rbx,~0xF
    ; fxrstor [rbx - 512]

    ; switch to whichever address space we returned
    mov cr3, rax

    cli

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax

    add rsp, 16 ; pop interrupt_num and error code
    iretq
