format ELF64

public isr_stub_table

extrn isr_dispatch

section '.text' executable align 16

macro PUSH_GPRS
{
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
}

macro POP_GPRS
{
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
}

macro CALL_DISPATCH
{
	local .aligned, .call

	mov rdi, rsp

	mov rbx, rsp
	and rbx, 0xF
	cmp rbx, 8
	je .aligned

	sub rsp, 8
	mov rbx, 8
	jmp .call

.aligned:
	xor rbx, rbx

.call:
	call isr_dispatch
	add rsp, rbx
}

macro GEN_ISR vec
{
	local .kernel_entry, .kernel_exit

	align 16
	isr_stub_#vec:
	cld

	if vec = 8 | vec = 10 | vec = 11 | vec = 12 | vec = 13 | vec = 14 | vec = 17 | vec = 21 | vec = 29 | vec = 30
		mov r11, [rsp]
		add rsp, 8
		push r11
	else
		push 0
	end if

	push vec

	mov r11, [rsp + 24]
	and r11, 0x3
	cmp r11, 0x3
	jne .kernel_entry

	swapgs

.kernel_entry:
	PUSH_GPRS
	CALL_DISPATCH
	POP_GPRS

	mov r11, [rsp + 144]
	and r11, 0x3
	cmp r11, 0x3
	jne .kernel_exit

	swapgs

.kernel_exit:
	add rsp, 16
	iretq
}

macro GEN_PTR vec
{
	dq isr_stub_#vec
}

rept 256 i:0
{
	GEN_ISR i
}

section '.rodata' align 16

align 16
isr_stub_table:

rept 256 i:0
{
	GEN_PTR i
}
