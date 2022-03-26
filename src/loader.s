[bits 64]
[section .text]
[default rel]

; We got ourselves boot info in RCX
start:
	; switch to new kernel stack
	mov rsp, rdx


	; setup GDT descriptor
	lea rax, [gdt64]
	mov [gdt64.pointer + 2], rax

	; set new page table crap
	mov rax, [rcx + 8]
	mov cr3, rax


	mov rdi, rcx
	call [rcx]
	hlt


	lgdt [gdt64.pointer]

reload_segments:
	mov rax, 0x8
	push rax

	lea rax, [.reload_cs]
	push rax
	retf
.reload_cs:
	mov ax, 0x10
	mov ds, ax
	mov es, ax
	mov fs, ax
	mov gs, ax
	mov ss, ax

doodle:

	; Put some pixels
	mov rax, [rcx + 48]
	mov rdx, 40000
.loop2:
	mov dword [rax + rdx * 4], 0xFFFF00FF
	sub rdx, 1
	jnz .loop2
	hlt

gdt64:
	dq 0 ; zero entry
.kernel_code_segment: equ $ - gdt64
	dd 0xFFFF
	db 0
	dw 0xAF9A
	db 0
.kernel_data_segment: equ $ - gdt64
	dd 0xFFFF
	db 0
	dw 0xAF92
	db 0
.user_code_segment: equ $ - gdt64
	dd 0xFFFF
	db 0
	dw 0xAFFA
	db 0
.user_data_segment: equ $ - gdt64
	dd 0xFFFF
	db 0
	dw 0xAFF2
	db 0
.pointer:
	dw $ - gdt64 - 1 ; length
	dq 0 ; address
