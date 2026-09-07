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

all: $(BUILD)/hal.dll

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

disasm: $(BUILD)/hal.elf
	$(OBJDUMP) -d -r $< > $(BUILD)/hal.lst

clean:
	rm -rf $(BUILD)

.PHONY: all clean disasm
