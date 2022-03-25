[bits 64]
[section .text]

; We got ourselves boot info in RCX
start:
	; Setup GDT descriptor
	; lea rax, [gdt64]
	; mov [gdt64.pointer + 2], rax

	; set new page table crap
	; mov rax, [rcx + 8]
	; mov cr3, rax

	; Put some pixels
	mov rax, [rcx + 48]
	mov rdx, 40000
.loop:
	mov dword [rax + rdx * 4], 0xFF00FF00
	sub rdx, 1
	jnz .loop
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
