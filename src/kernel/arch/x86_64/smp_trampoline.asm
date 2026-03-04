format ELF64

public ap_trampoline_start
public ap_trampoline_end
public ap_trampoline_mailbox
public ap_trampoline_gdt_ptr
public ap_trampoline_gdt

section '.text' executable align 16

use16

ap_trampoline_start:

	mailbox_off = ap_trampoline_mailbox - ap_trampoline_start
	gdt_ptr_off = ap_trampoline_gdt_ptr - ap_trampoline_start
	pm_entry_off = ap_protected_mode - ap_trampoline_start
	lm_entry_off = ap_long_mode - ap_trampoline_start

	db 0xFA

	db 0x8C, 0xC8
	db 0x8E, 0xD8
	db 0x8E, 0xC0
	db 0x8E, 0xD0

	mov dword [mailbox_off + 32], 1

	lgdt [gdt_ptr_off]

	mov eax, cr0
	or eax, 1
	mov cr0, eax

	db 0x66, 0xEA
	dd pm_entry_off
	dw 0x08

use32
ap_protected_mode:
	mov dword [mailbox_off + 32], 2
	mov ax, 0x10
	mov ds, ax
	mov es, ax
	mov ss, ax

	mov esp, 0x1000 - 16

	mov eax, [mailbox_off + 40]
	add eax, idt32_off
	mov [idt32_ptr + 2], eax

	mov edi, [mailbox_off + 40]
	add edi, idt32_off
	mov ecx, 32
	mov edx, [mailbox_off + 40]
	add edx, ap_fault32_off
.idt32_fill:
	mov word [edi + 0], dx
	mov eax, edx
	shr eax, 16
	mov word [edi + 6], ax
	add edi, 8
	loop .idt32_fill

	lidt [idt32_ptr]

	mov eax, cr4
	or eax, 0x20
	mov cr4, eax

	mov dword [mailbox_off + 32], 3
	mov eax, [mailbox_off + 0]
	mov cr3, eax

	mov ecx, 0xC0000080
	rdmsr
	or eax, 0x00000100
	wrmsr

	mov eax, cr0
	or eax, 0x80000000
	mov cr0, eax

	mov eax, [mailbox_off + 40]

	mov ebx, eax
	add ebx, mailbox_off

	add eax, lm_entry_off
	
	mov [mailbox_off + 48], eax
	mov word [mailbox_off + 52], 0x18

	jmp far [mailbox_off + 48]

use64
ap_long_mode:
	mov rax, [rbx + 40]
	add rax, 0x1000
	sub rax, 16
	mov rsp, rax

	mov rax, [rbx + 40]
	lea rdi, [rax + idt64_off]
	mov [idt64_ptr + 2], rdi

	mov rcx, 32
	lea rdx, [rax + ap_fault64_off]
	lea rsi, [rdi]
.idt64_fill:
	mov word [rsi + 0], dx
	mov r8, rdx
	shr r8, 16
	mov word [rsi + 6], r8w
	shr r8, 16
	mov dword [rsi + 8], r8d
	add rsi, 16
	loop .idt64_fill

	lidt [idt64_ptr]

	mov ax, 0x20
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov fs, ax
	mov gs, ax

	mov rsp, [rbx + 8]

	mov dword [rbx + 32], 5
	mov rcx, [rbx + 24]
	mov dword [rcx], 1

	mov rdi, rbx

	mov rax, [rbx + 16]
	jmp rax

align 8

use32
idt32_ptr:
	dw idt32_end - idt32 - 1
	dd 0

align 8
idt32:
	repeat 32
		dw 0
		dw 0x08
		db 0
		db 0x8E
		dw 0
	end repeat
idt32_end:

align 16

use64
idt64_ptr:
	dw idt64_end - idt64 - 1
	dq 0

align 16
idt64:
	repeat 32
		dw 0
		dw 0x18
		db 0
		db 0x8E
		dw 0
		dd 0
		dd 0
	end repeat
idt64_end:

align 16

use32
ap_fault32:
	cli
.halt32:
	hlt
	jmp .halt32

align 16

use64
ap_fault64:
	cli
.halt64:
	hlt
	jmp .halt64

align 8

idt32_off = idt32 - ap_trampoline_start
idt64_off = idt64 - ap_trampoline_start
ap_fault32_off = ap_fault32 - ap_trampoline_start
ap_fault64_off = ap_fault64 - ap_trampoline_start

ap_trampoline_gdt:
	dq 0
	dq 0x00CF9A000000FFFF
	dq 0x00CF92000000FFFF
	dq 0x00AF9A000000FFFF
	dq 0x00AF92000000FFFF

ap_trampoline_gdt_ptr:
	dw gdt_end - ap_trampoline_gdt - 1
	dd 0

gdt_end:

align 8
ap_trampoline_mailbox:
	dq 0
	dq 0
	dq 0
	dq 0
	dd 0
	dd 0
	dq 0
	dq 0
	dq 0

ap_trampoline_end:
