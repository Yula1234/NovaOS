#pragma once

/* Kernel entry point ABI. Bootloader passes multiboot2 magic and MBI physical address. */
extern "C" void kmain(unsigned multiboot_magic, unsigned multiboot_info_addr);
