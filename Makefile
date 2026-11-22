CROSS ?= aarch64-linux-gnu-
CC := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
BUILD_TAG ?= local
CFLAGS := -O2 -Wall -Wextra -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns \
          -mstrict-align -mgeneral-regs-only -mcpu=cortex-a53 -fno-pic -fno-pie \
          -fno-stack-protector -fno-asynchronous-unwind-tables -nostdlib \
          -DBUILD_TAG='"$(BUILD_TAG)"'
LDFLAGS := -T link.ld -Wl,--build-id=none -no-pie -static

all: out/boot_c_safe.img out/boot_c_full.img out/boot_c_log.img out/boot_c_mmu.img

build/safe.elf: start.S main.c font.h link.ld
	mkdir -p build
	$(CC) $(CFLAGS) -DRISKY=0 $(LDFLAGS) start.S main.c -o $@ -lgcc

build/full.elf: start.S main.c font.h link.ld
	mkdir -p build
	$(CC) $(CFLAGS) -DRISKY=1 $(LDFLAGS) start.S main.c -o $@ -lgcc

build/log.elf: start.S main.c font.h link.ld
	mkdir -p build
	$(CC) $(CFLAGS) -DRISKY=3 $(LDFLAGS) start.S main.c -o $@ -lgcc

build/mmu.elf: start.S main.c font.h link.ld
	mkdir -p build
	$(CC) $(CFLAGS) -DRISKY=4 $(LDFLAGS) start.S main.c -o $@ -lgcc

build/%.bin: build/%.elf
	$(OBJCOPY) -O binary $< $@

out/boot_c_%.img: build/%.bin mkboot.py header.bin dtb.bin vbmeta.bin footer.bin
	mkdir -p out
	python3 mkboot.py $< $@

clean:
	rm -rf build out
