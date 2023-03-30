global io_in8
global io_in16
global io_in32
global io_out8
global io_out16
global io_out32
global io_wait
global __writemsr

global irq_enable
global irq_disable
global IDT_Descriptor

extern irq_int_handler

section .text
__writemsr:
	mov rax, rsi
	and eax, -1
	mov rdx, rsi
	shr edx, 32
	mov rcx, rdi
	wrmsr
	ret
irq_enable:
    lidt [rdi]
    sti
    int 3
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
    mov rdx, rdi
    xor rax, rax
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
    mov rax, rsi
    mov rdx, rdi
    out dx, eax
    ret
io_wait:
    jmp .a
.a: jmp .b
.b: ret

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

    ; fxsave needs to be aligned to 16bytes
    ; mov	rbx,rsp
    ; and	rsp,~0xF
    ; fxsave	[rsp - 512]
    ; mov	rsp,rbx
    ; sub	rsp,512 + 16

    mov rdi, rsp
    call irq_int_handler

    ; fxrstor also needs to be aligned to 16bytes
    ; add	rsp, 512 + 16
    ; mov	rbx,rsp
    ; and	rbx,~0xF
    ; fxrstor	[rbx - 512]
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
