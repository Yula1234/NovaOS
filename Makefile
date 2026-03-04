SHELL := /bin/bash

ARCH := x86_64

TOOLCHAIN_PREFIX ?= x86_64-elf-

CC := $(or $(shell command -v $(TOOLCHAIN_PREFIX)g++ 2>/dev/null),g++)
LD := $(or $(shell command -v $(TOOLCHAIN_PREFIX)ld 2>/dev/null),ld)
OBJCOPY := $(or $(shell command -v $(TOOLCHAIN_PREFIX)objcopy 2>/dev/null),objcopy)
FASM ?= fasm

CFLAGS := -std=c++20 -O2 -g -Wall -Wextra -Wpedantic \
	-ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector -fno-pic -fno-pie \
	-mno-red-zone -mcmodel=kernel -mgeneral-regs-only -fno-threadsafe-statics \
	-I./src

LDFLAGS := -T linker.ld -nostdlib

BUILD_DIR := build
ISO_DIR := $(BUILD_DIR)/isofiles

KERNEL_ELF := $(BUILD_DIR)/kernel.elf
KERNEL_BIN := $(BUILD_DIR)/kernel.bin

SRC_ASM := $(shell find src -name '*.asm')
SRC_CPP := $(shell find src -name '*.cpp')

OBJ_ASM := $(patsubst src/%.asm,$(BUILD_DIR)/obj/%.o,$(SRC_ASM))
OBJ_CPP := $(patsubst src/%.cpp,$(BUILD_DIR)/obj/%.o,$(SRC_CPP))

OBJ := $(OBJ_ASM) $(OBJ_CPP)

.PHONY: all kernel iso clean

all: kernel

kernel: $(KERNEL_ELF) $(KERNEL_BIN)

$(BUILD_DIR)/obj/%.o: src/%.asm
	@mkdir -p $(dir $@)
	$(FASM) $< $@ > /dev/null

$(BUILD_DIR)/obj/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(KERNEL_ELF): $(OBJ) linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJ)

$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@

iso: $(KERNEL_ELF)
	@rm -rf $(ISO_DIR)
	@mkdir -p $(ISO_DIR)/boot/grub
	@cp $(KERNEL_ELF) $(ISO_DIR)/boot/kernel.elf
	@cp grub/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	@grub-mkrescue -o $(BUILD_DIR)/os.iso $(ISO_DIR) >/dev/null

clean:
	@rm -rf $(BUILD_DIR)
