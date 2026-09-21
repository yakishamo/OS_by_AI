SHELL := /bin/sh
.DELETE_ON_ERROR:

CLANG ?= clang
LLD ?= lld
PYTHON ?= python3
HOST_PMM_LDFLAGS :=
ifeq ($(shell uname -s),Darwin)
HOST_SDK ?= /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk
HOST_PMM_LDFLAGS := -Wl,-syslibroot,$(HOST_SDK) -lSystem
endif

BUILD := build
EFI := $(BUILD)/esp/EFI/BOOT/BOOTX64.EFI
KERNEL := $(BUILD)/esp/kernel.elf
BOOT_OBJECTS := $(BUILD)/boot/main.obj $(BUILD)/boot/load.obj
KERNEL_OBJECTS := $(BUILD)/kernel/entry.o $(BUILD)/kernel/main.o $(BUILD)/kernel/serial.o \
                  $(BUILD)/kernel/tables.o $(BUILD)/kernel/interrupts.o \
                  $(BUILD)/kernel/pmm.o $(BUILD)/kernel/pmm_check.o $(BUILD)/kernel/paging.o \
                  $(BUILD)/kernel/paging_check.o $(BUILD)/kernel/console.o $(BUILD)/kernel/timer.o \
                  $(BUILD)/kernel/irq.o $(BUILD)/kernel/rx_buffer.o
OBJECTS := $(BOOT_OBJECTS) $(KERNEL_OBJECTS)
COMMON_CFLAGS := -std=c17 -ffreestanding -fno-builtin -fno-stack-protector \
                 -mno-red-zone -mgeneral-regs-only -Wall -Wextra -Werror -O2 -g -MMD -MP
KERNEL_CFLAGS := --no-default-config --target=x86_64-unknown-none-elf \
                 $(COMMON_CFLAGS) -fno-pic -fno-pie -fno-asynchronous-unwind-tables
CFLAGS := --no-default-config --target=x86_64-pc-windows-msvc \
          $(COMMON_CFLAGS) -mno-stack-arg-probe

.PHONY: all build run test debug doctor clean help
all: build
build: $(EFI) $(KERNEL)

.PHONY: loader kernel
loader: $(EFI)
kernel: $(KERNEL)

$(BUILD)/%.obj: %.c Makefile
	@mkdir -p $(@D)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel/%.o: kernel/%.c Makefile
	@mkdir -p $(@D)
	$(CLANG) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/%.o: kernel/%.S Makefile
	@mkdir -p $(@D)
	$(CLANG) --no-default-config --target=x86_64-unknown-none-elf -g -MMD -MP -c $< -o $@

$(KERNEL): $(KERNEL_OBJECTS) kernel/linker.ld Makefile
	@mkdir -p $(@D)
	$(LLD) -flavor gnu -m elf_x86_64 -static -T kernel/linker.ld -o $@ $(KERNEL_OBJECTS)

$(EFI): $(BOOT_OBJECTS) Makefile
	@mkdir -p $(@D)
	$(LLD) -flavor link /machine:x64 /subsystem:efi_application \
		/entry:efi_main /nodefaultlib /debug:dwarf /out:$@ $(BOOT_OBJECTS)

run: build
	$(PYTHON) scripts/qemu.py run

$(BUILD)/pmm-test.so: kernel/pmm.c kernel/pmm.h include/boot_info.h Makefile
	@mkdir -p $(@D)
	$(CLANG) --no-default-config -std=c17 -ffreestanding -fno-builtin \
		-fno-stack-protector -Wall -Wextra -Werror -O2 -shared -nostdlib \
		-fuse-ld=lld kernel/pmm.c $(HOST_PMM_LDFLAGS) -o $@

$(BUILD)/rx-test.so: kernel/rx_buffer.c kernel/rx_buffer.h Makefile
	@mkdir -p $(@D)
	$(CLANG) --no-default-config -std=c17 -ffreestanding -fno-builtin \
		-fno-stack-protector -Wall -Wextra -Werror -O2 -shared -nostdlib \
		-fuse-ld=lld kernel/rx_buffer.c $(HOST_PMM_LDFLAGS) -o $@

$(BUILD)/tests/rx_stalled.o: scripts/fixtures/rx_stalled.c kernel/rx_buffer.h Makefile
	@mkdir -p $(@D)
	$(CLANG) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/tests/rx-overflow.elf: $(KERNEL_OBJECTS) $(BUILD)/tests/rx_stalled.o kernel/linker.ld Makefile
	$(LLD) -flavor gnu -m elf_x86_64 -static -T kernel/linker.ld \
		--wrap=rx_read -o $@ $(KERNEL_OBJECTS) $(BUILD)/tests/rx_stalled.o

test: build $(BUILD)/pmm-test.so $(BUILD)/rx-test.so $(BUILD)/tests/rx-overflow.elf
	$(PYTHON) scripts/check_build.py
	$(PYTHON) scripts/test_pmm.py
	$(PYTHON) scripts/test_rx_buffer.py
	$(PYTHON) scripts/qemu.py test

debug: build
	@$(PYTHON) scripts/qemu.py debug

doctor:
	$(CLANG) --version
	$(LLD) -flavor gnu --version
	$(PYTHON) scripts/qemu.py doctor

clean:
	rm -rf $(BUILD)

help:
	@echo 'make build   Build the UEFI loader and minimal C kernel'
	@echo 'make run     Load kernel.elf and boot the kernel'
	@echo 'make test    Verify artifacts, kernel halt and loader error cases'
	@echo 'make debug   GDB stdio transport; see README before using'
	@echo 'make doctor  Check tools and firmware paths'
	@echo 'make clean   Remove generated files'

-include $(BOOT_OBJECTS:.obj=.d) $(KERNEL_OBJECTS:.o=.d)
