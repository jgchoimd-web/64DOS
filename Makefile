PYTHON ?= python3
NASM ?= nasm
CC ?= gcc
LD ?= ld
OBJCOPY ?= objcopy
QEMU ?= qemu-system-x86_64

BUILD := build
DIST := dist
IMAGE := $(DIST)/64dos.img

CFLAGS := -std=c11 -O2 -Wall -Wextra -ffreestanding -fno-stack-protector \
	-fno-pic -m64 -mno-red-zone -mgeneral-regs-only -Ikernel/include
LDFLAGS := -nostdlib -z max-page-size=0x1000 -T kernel/linker.ld

.PHONY: all clean check-tools check-tools-all test smoke image-test unit-test

all: $(IMAGE)

check-tools:
	$(PYTHON) scripts/check-tools.py --profile build

check-tools-all:
	$(PYTHON) scripts/check-tools.py --profile all

$(BUILD) $(DIST):
	mkdir -p $@

$(BUILD)/stage1.bin: boot/stage1.asm | $(BUILD)
	$(NASM) -f bin $< -o $@

$(BUILD)/stage2.bin: boot/stage2.asm | $(BUILD)
	$(NASM) -f bin $< -o $@

$(BUILD)/entry.o: kernel/entry.asm | $(BUILD)
	$(NASM) -f elf64 $< -o $@

$(BUILD)/kernel.o: kernel/kernel.c kernel/include/bootinfo.h kernel/include/fs_iface.h kernel/include/fs_fat12.h | $(BUILD)
	$(CC) $(CFLAGS) -c kernel/kernel.c -o $@

$(BUILD)/fs_fat12.o: kernel/fs_fat12.c kernel/include/fs_fat12.h kernel/include/fs_iface.h | $(BUILD)
	$(CC) $(CFLAGS) -c kernel/fs_fat12.c -o $@

$(BUILD)/kernel64.elf: $(BUILD)/entry.o $(BUILD)/kernel.o $(BUILD)/fs_fat12.o kernel/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(BUILD)/entry.o $(BUILD)/kernel.o $(BUILD)/fs_fat12.o

$(BUILD)/kernel64.bin: $(BUILD)/kernel64.elf
	$(OBJCOPY) -O binary $< $@

$(IMAGE): $(BUILD)/stage1.bin $(BUILD)/stage2.bin $(BUILD)/kernel64.bin scripts/mkfloppy.py image_root/AUTOEXEC.BAT image_root/README.TXT | $(DIST)
	$(PYTHON) scripts/mkfloppy.py \
		--stage1 $(BUILD)/stage1.bin \
		--stage2 $(BUILD)/stage2.bin \
		--kernel $(BUILD)/kernel64.bin \
		--root image_root \
		--output $(IMAGE)
	$(PYTHON) scripts/check-size.py $(IMAGE)

image-test: $(IMAGE)
	$(PYTHON) scripts/validate-image.py $(IMAGE)

unit-test:
	$(PYTHON) -m unittest discover -s tests

test: image-test unit-test

smoke: $(IMAGE)
	$(PYTHON) scripts/qemu-smoke.py $(IMAGE)

clean:
	rm -rf $(BUILD) $(DIST)
