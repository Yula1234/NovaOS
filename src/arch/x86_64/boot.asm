format ELF64

public _start32
extrn kmain

KERNEL_VMA equ 0xFFFFFFFF80000000

section '.multiboot2' align 8
	MB2_MAGIC equ 0xE85250D6
	MB2_ARCH equ 0
	MB2_HEADER_LEN equ (mb2_header_end - mb2_header)
	MB2_CHECKSUM equ (0x100000000 - (MB2_MAGIC + MB2_ARCH + MB2_HEADER_LEN))

mb2_header:
	dd MB2_MAGIC
	dd MB2_ARCH
	dd MB2_HEADER_LEN
	dd MB2_CHECKSUM

	align 8
	du 0
	du 0
	dd 8

mb2_header_end:

section '.boot' executable align 16
use32

_start32:
	cli
	mov [mb2_magic], eax
	mov [mb2_info], ebx
	mov esp, stack_top32

	lgdt [gdt32_ptr]

	mov eax, cr4
	or eax, 0x20
	mov cr4, eax

	mov eax, pml4
	mov cr3, eax

	mov ecx, 0xC0000080
	rdmsr
	or eax, 0x00000100
	wrmsr

	mov eax, cr0
	or eax, 0x80000001
	mov cr0, eax

	jmp 0x08:long_mode_entry

use64
long_mode_entry:
	mov ax, 0x10
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov fs, ax
	mov gs, ax

	lea rsp, [stack_top64]

	mov eax, [mb2_magic]
	mov ebx, [mb2_info]

	mov edi, eax
	mov esi, ebx

	mov rax, kmain_entry
	jmp rax

section '.text' executable align 16
use64

kmain_entry:
	call kmain
.hang:
	cli
	hlt
	jmp .hang

section '.bootdata' writable align 4096

align 8
mb2_magic:
	dd 0
mb2_info:
	dd 0

align 16
stack32:
	rb 4096
stack_top32:

align 16
stack64:
	rb 16384
stack_top64:

align 4096
pml4:
	dq pdpt_low + 0x003
	repeat 510
		dq 0
	end repeat
	dq pdpt_low + 0x003

align 4096
pdpt_low:
	dq pd_low0 + 0x003
	dq pd_low1 + 0x003
	dq pd_low2 + 0x003
	dq pd_low3 + 0x003

	repeat 506
		dq 0
	end repeat

	dq pd_high + 0x003
	dq 0

align 4096
pd_low0:
addr = 0
	repeat 512
		dq addr + 0x083
		addr = addr + 0x200000
	end repeat

align 4096
pd_low1:
addr1 = 0x40000000
	repeat 512
		dq addr1 + 0x083
		addr1 = addr1 + 0x200000
	end repeat

align 4096
pd_low2:
addr2 = 0x80000000
	repeat 512
		dq addr2 + 0x083
		addr2 = addr2 + 0x200000
	end repeat

align 4096
pd_low3:
addr3 = 0xC0000000
	repeat 512
		dq addr3 + 0x083
		addr3 = addr3 + 0x200000
	end repeat

align 4096
pd_high:
addr_high = 0x00200000
	repeat 512
		dq addr_high + 0x083
		addr_high = addr_high + 0x200000
	end repeat

align 8
gdt32:
	dq 0
	dq 0x00AF9A000000FFFF
	dq 0x00AF92000000FFFF

gdt32_ptr:
	dw gdt32_end - gdt32 - 1
	dd gdt32

gdt32_end:
