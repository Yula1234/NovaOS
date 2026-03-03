OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start32)

KERNEL_VMA = 0xFFFFFFFF80000000;
KERNEL_LMA = 0x00100000;
KERNEL_LMA_BASE_FIXED = 0x00200000;

PHDRS
{
	boot   PT_LOAD FLAGS(5);
	text   PT_LOAD FLAGS(5);
	rodata PT_LOAD FLAGS(4);
	data   PT_LOAD FLAGS(6);
}

SECTIONS
{
	. = KERNEL_LMA;
	__kernel_phys_start = .;

	.boot :
	{
		*(.multiboot2)
		*(.boot .boot.*)
		*(.bootdata .bootdata.*)
	} :boot

	KERNEL_LMA_BASE = KERNEL_LMA_BASE_FIXED;

	. = KERNEL_VMA;

	.text : AT(KERNEL_LMA_BASE + (ADDR(.text) - KERNEL_VMA))
	{
		*(.text .text.*)
	} :text

	.rodata : AT(KERNEL_LMA_BASE + (ADDR(.rodata) - KERNEL_VMA))
	{
		*(.rodata .rodata.*)
	} :rodata

	.data : AT(KERNEL_LMA_BASE + (ADDR(.data) - KERNEL_VMA))
	{
		*(.data .data.*)
	} :data

	.bss : AT(KERNEL_LMA_BASE + (ADDR(.bss) - KERNEL_VMA))
	{
		*(COMMON)
		*(.bss .bss.*)
	} :data

	__kernel_phys_end = KERNEL_LMA_BASE + (ADDR(.bss) - KERNEL_VMA) + SIZEOF(.bss);
}
