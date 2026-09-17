# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
# Builds hal.dll, a Windows NT 4.0 PowerPC HAL for the Apple Network Server, with clang + lld and
# tools/elf2pe.py.  No Microsoft tools are needed.  Requires clang-18 / lld-18 (any recent LLVM works).
CLANG ?= clang-18
LLD   ?= ld.lld-18
OBJDUMP ?= llvm-objdump-18
TOOLS  = tools
TARGET = --target=powerpcle-unknown-linux-gnu -mcpu=604
# -combiner-store-merging=false: the 604 runs little-endian here, where a misaligned access
# traps instead of being fixed up in hardware.  LLVM merges adjacent byte stores into word
# stores whenever it can prove they are adjacent — it cannot prove they are *aligned*, and for
# PowerPC it assumes it may — so a plain string copy into a buffer at an odd offset becomes an
# alignment exception.  (`+strict-align` is not plumbed through the PowerPC backend for this.)
# The matching load-side fold, four byte loads turned back into one `lwz`, is blocked locally
# with volatile where it matters; see HalpGetUlong in src/disk.c.
CFLAGS = $(TARGET) -O2 -Wall -Wextra -Wno-unused-parameter -ffreestanding -fno-builtin -fno-pic -fno-pie \
         -msoft-float -fno-asynchronous-unwind-tables -fno-stack-protector -fno-jump-tables \
         -mllvm -combiner-store-merging=false -Iinclude
ASFLAGS = $(TARGET) -x assembler-with-cpp -c
BUILD = build
SRCS_C = $(wildcard src/*.c)
SRCS_S = $(wildcard src/*.S)
OBJS = $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS_C)) $(patsubst src/%.S,$(BUILD)/%.o,$(SRCS_S)) \
       $(BUILD)/imports.o $(BUILD)/exports.o

all: $(BUILD)/hal.dll $(BUILD)/adbport.sys

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/imports.S $(BUILD)/exports.S: hal.imports hal.exports $(TOOLS)/mkstubs.py | $(BUILD)
	python3 $(TOOLS)/mkstubs.py hal.imports hal.exports $(BUILD)/imports.S $(BUILD)/exports.S

$(BUILD)/%.o: src/%.c include/*.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.S | $(BUILD)
	$(CLANG) $(ASFLAGS) $< -o $@

$(BUILD)/%.o: $(BUILD)/%.S | $(BUILD)
	$(CLANG) $(ASFLAGS) $< -o $@

$(BUILD)/hal.elf: $(OBJS) hal.ld
	$(LLD) -T hal.ld --emit-relocs -nostdlib -static -o $@ $(OBJS)

$(BUILD)/hal.dll: $(BUILD)/hal.elf hal.exports $(TOOLS)/elf2pe.py
	python3 $(TOOLS)/elf2pe.py $< $@ --exports hal.exports --dllname HAL.dll

# ---- adbport.sys: the ADB keyboard/mouse port driver and OEM-disk ramdisk (drivers/adbport) ----
# The same toolchain and the same thunks as the HAL; a second .imports file with two DLL groups.
ADB      = drivers/adbport
ADB_SRCS = $(wildcard $(ADB)/*.c)
ADB_OBJS = $(patsubst $(ADB)/%.c,$(BUILD)/adbport/%.o,$(ADB_SRCS)) \
           $(BUILD)/adbport/thunk.o $(BUILD)/adbport/imports.o $(BUILD)/adbport/exports.o

$(BUILD)/adbport:
	mkdir -p $(BUILD)/adbport

$(BUILD)/adbport/imports.S $(BUILD)/adbport/exports.S: $(ADB)/adbport.imports $(ADB)/adbport.exports $(TOOLS)/mkstubs.py | $(BUILD)/adbport
	python3 $(TOOLS)/mkstubs.py $(ADB)/adbport.imports $(ADB)/adbport.exports $(BUILD)/adbport/imports.S $(BUILD)/adbport/exports.S

$(BUILD)/adbport/%.o: $(ADB)/%.c $(ADB)/adbport.h include/*.h | $(BUILD)/adbport
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/adbport/%.o: $(ADB)/%.S | $(BUILD)/adbport
	$(CLANG) $(ASFLAGS) $< -o $@

$(BUILD)/adbport/%.o: $(BUILD)/adbport/%.S | $(BUILD)/adbport
	$(CLANG) $(ASFLAGS) $< -o $@

$(BUILD)/adbport.elf: $(ADB_OBJS) $(ADB)/adbport.ld
	$(LLD) -T $(ADB)/adbport.ld --emit-relocs -nostdlib -static -o $@ $(ADB_OBJS)

$(BUILD)/adbport.sys: $(BUILD)/adbport.elf $(ADB)/adbport.exports $(TOOLS)/elf2pe.py
	python3 $(TOOLS)/elf2pe.py $< $@ --exports $(ADB)/adbport.exports --dllname adbport.sys \
	        --entry desc_DriverEntry --import-dll ntoskrnl.exe --import-dll HAL.dll

disasm: $(BUILD)/hal.elf
	$(OBJDUMP) -d -r $< > $(BUILD)/hal.lst

# The boot floppy, from your own Windows NT 4.0 PowerPC CD:
#
#     make floppy ISO=/path/to/nt4-ppc.iso
#
# Builds the HAL and the ADB driver first, then takes the four files it cannot
# ship -- the veneer, SETUPLDR and the Cirrus driver pair -- off the image you
# name, patches the veneer twice, and lays out build/boot-floppy.img.
#
# The result CANNOT BE REDISTRIBUTED: five of its eleven files are Microsoft's
# and three of those are modified.  See PROVENANCE.md.
floppy: all
	@test -n "$(ISO)" || { \
	  echo 'make floppy needs your CD image:'; \
	  echo '    make floppy ISO=/path/to/nt4-ppc.iso'; \
	  exit 1; }
	python3 $(TOOLS)/mkfloppy.py --iso "$(ISO)"

clean:
	rm -rf $(BUILD)

.PHONY: all clean disasm floppy
