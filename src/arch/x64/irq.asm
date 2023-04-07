
bits 64

global irq_enable
global irq_disable
global IDT_Descriptor
global asm_int_handler
global syscall_handler

extern irq_int_handler
extern syscall_table_count
extern syscall_table
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
DEFINE_INT 6
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
    cli

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
    mov rax, [boot_info]
    mov rax, [rax + 0]
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

; RCX - return address
; R11 - original RFLAGS
syscall_handler:
    ; early out on >256 syscall numbers
    cmp rax, [syscall_table_count]
    jae .bad_syscall

    ; disable interrupts
    cli

    ; preserve old stack, use kernel stack
    swapgs

    ; save user stack, switch to kernel stack
    mov gs:[32], rsp
    mov rsp, gs:[8]

    ; ss, rsp, flags, cs, rip
    push 0x23         ; ss
    push qword gs:[32]; user rsp
    push r11          ; rflags
    push 0x1B         ; cs
    push rcx          ; rip

    ; error & interrupt num
    push 0
    push 0

    ; save registers
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

    ; call into syscall table
    lea rcx, syscall_table
    mov rcx, [rcx + rax*8]
    test rcx, rcx
    jz .no_syscall
.has_syscall:
    ; switch to kernel PML4
    mov rsi, cr3
    mov rdx, [boot_info]
    mov rdx, [rdx + 0]
    mov cr3, rdx

    ; run C syscall stuff & preserve old address space (in callee saved reg)
    mov rdi, rsp
    call rcx

    ; switch to new address space
    mov cr3, rax
.no_syscall:
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

    add rsp, 56 ; pop the rest of the CPU state

    ; enable interrupts
    or r11, 0x200

    ; give the user back his GS and stack
    mov rsp, gs:[32]
    swapgs

    ; we're using the 64bit sysret
    db 0x48
    sysret
.bad_syscall:
    mov rax, -1
    ; we're using the 64bit sysret
    db 0x48
    sysret

; RDI - CPUState*
; RSI - PageTable*
do_context_switch:
    ; switch to new address space
    mov cr3, rax

    ; use CPUState as the stack
    mov rsp, rdi
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
