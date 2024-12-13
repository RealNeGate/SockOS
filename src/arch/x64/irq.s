default rel
global irq_enable, irq_disable, asm_int_handler, syscall_handler, do_context_switch
global io_in8, io_in16, io_in32, io_out8, io_out16, io_out32
extern x86_irq_int_handler, syscall_table_count, syscall_table, boot_info

section .text
irq_enable:
    lidt [rdi]
    sti
    ret

irq_disable:
    cli
    ret
io_in8:
    mov rdx, rdi
    xor rax, rax
    in al, dx
    ret
io_in16:
    mov rdx, rdi
    xor rax, rax
    in ax, dx
    ret
io_in32:
    mov edx, edi
    in eax, dx
    ret
io_out8:
    mov rax, rsi
    mov rdx, rdi
    out dx, al
    ret
io_out16:
    mov rax, rsi
    mov rdx, rdi
    out dx, ax
    ret
io_out32:
    mov eax, esi
    mov edx, edi
    out dx, eax
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

%macro swapgs_if_necessary 0
    cmp qword [rsp + 8], 8
    je %%woah
    swapgs
%%woah:
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

    ; swap out user gs, use kernel gs now
    swapgs_if_necessary

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

    ; stack segment seems to be set to 0 on interrupts, let's
    ; just set it back to the kernel DS selector
    mov ax, 0x10
    mov ss, ax

    ; switch to kernel PML4
    mov rsi, cr3
    mov rcx, [boot_info]
    mov rax, [rcx + 0]
    sub rax, [rcx + 16]
    mov cr3, rax

    ; fxsave needs to be aligned to 16bytes
    mov rbx,rsp
    and rsp,~0xF
    fxsave  [rsp - 512]
    mov rsp,rbx
    sub rsp,512 + 16

    mov rdi, rsp
    mov rdx, gs:[0]
    call x86_irq_int_handler

    ; fxrstor also needs to be aligned to 16bytes
    add rsp, 512 + 16
    mov rbx,rsp
    and rbx,~0xF
    fxrstor [rbx - 512]

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

    ; back to user gs
    swapgs_if_necessary
    iretq

; RCX - return address
; R11 - original RFLAGS
syscall_handler:
    ; early out on >256 syscall numbers
    cmp rax, [syscall_table_count]
    jae syscall_handler.bad_syscall

    ; disable interrupts
    cli

    ; preserve old stack, use kernel stack
    swapgs

    ; save user stack, switch to kernel stack
    mov gs:[24], rsp
    mov rsp, gs:[16]

    ; ss, rsp, flags, cs, rip
    push 0x1B         ; ss
    push qword gs:[24]; user rsp
    push r11          ; rflags
    push 0x23         ; cs
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

    ; fxsave needs to be aligned to 16bytes
    mov rbx,rsp
    and rsp,~0xF
    fxsave  [rsp - 512]
    mov rsp,rbx
    sub rsp,512 + 16

    ; call into syscall table
    lea rcx, syscall_table
    mov rcx, [rcx + rax*8]
    test rcx, rcx
    jz syscall_handler.no_syscall
syscall_handler.has_syscall:
    ; switch to kernel PML4
    mov rsi, cr3
    mov r10, [boot_info]
    mov rdx, [r10 + 0]
    sub rdx, [r10 + 16]
    mov cr3, rdx

    ; run C syscall stuff & preserve old address space (in callee saved reg)
    mov rdi, rsp
    mov rdx, gs:[0]
    call rcx

    ; switch to new address space
    mov cr3, rax
syscall_handler.no_syscall:
    ; fxrstor also needs to be aligned to 16bytes
    add rsp, 512 + 16
    mov rbx,rsp
    and rbx,~0xF
    fxrstor [rbx - 512]

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
    mov rsp, gs:[24]
    swapgs

    ; we're using the 64bit sysret
    db 0x48
    sysret
syscall_handler.bad_syscall:
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

    ; fxrstor also needs to be aligned to 16bytes
    add rsp, 512 + 16
    mov rbx,rsp
    and rbx,~0xF
    fxrstor [rbx - 512]

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
