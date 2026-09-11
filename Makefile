SHELL := /bin/sh
.DELETE_ON_ERROR:

CLANG ?= clang
LLD ?= lld
PYTHON ?= python3

BUILD := build
EFI := $(BUILD)/esp/EFI/BOOT/BOOTX64.EFI
OBJECTS := $(BUILD)/boot/main.obj
CFLAGS := --no-default-config --target=x86_64-pc-windows-msvc -std=c17 \
          -ffreestanding -fno-builtin -fno-stack-protector \
          -mno-stack-arg-probe -mno-red-zone -mgeneral-regs-only \
          -Wall -Wextra -Werror -O2 -g -MMD -MP

.PHONY: all build run test debug doctor clean help
all: build
build: $(EFI)

$(BUILD)/boot/%.obj: boot/%.c Makefile
	@mkdir -p $(@D)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(EFI): $(OBJECTS) Makefile
	@mkdir -p $(@D)
	$(LLD) -flavor link /machine:x64 /subsystem:efi_application \
		/entry:efi_main /nodefaultlib /debug:dwarf /out:$@ $(OBJECTS)

run: build
	$(PYTHON) scripts/qemu.py run

test: build
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
	@echo 'make build   Build the x86_64 UEFI serial probe (C)'
	@echo 'make run     Boot the UEFI Serial IO probe; quit with Ctrl-a x'
	@echo 'make test    Check UEFI Serial IO input/output (60s timeout)'
	@echo 'make debug   GDB stdio transport; see README before using'
	@echo 'make doctor  Check tools and firmware paths'
	@echo 'make clean   Remove generated files'

-include $(OBJECTS:.obj=.d)
