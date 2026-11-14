# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
# Builds hal.dll, a Windows NT 4.0 PowerPC HAL for the Apple Network Server, with clang + lld and
# tools/elf2pe.py.  No Microsoft tools are needed.  Requires clang-18 / lld-18 (any recent LLVM works).
CLANG ?= clang-18
LLD   ?= ld.lld-18
OBJDUMP ?= llvm-objdump-18
OBJCOPY ?= llvm-objcopy-18
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

all: $(BUILD)/hal.dll $(BUILD)/adbport.sys $(BUILD)/mesh.sys $(BUILD)/pmctrl.sys $(BUILD)/arcfw.xcf

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

# ---- mesh.sys: the SCSI miniport for a Power Macintosh 7500/8500's internal MESH bus (drivers/mesh) ----
MESHD     = drivers/mesh
MESH_SRCS = $(wildcard $(MESHD)/*.c)
MESH_OBJS = $(patsubst $(MESHD)/%.c,$(BUILD)/mesh/%.o,$(MESH_SRCS)) \
            $(BUILD)/mesh/thunk.o $(BUILD)/mesh/imports.o $(BUILD)/mesh/exports.o

$(BUILD)/mesh:
	mkdir -p $(BUILD)/mesh

$(BUILD)/mesh/imports.S $(BUILD)/mesh/exports.S: $(MESHD)/mesh.imports $(MESHD)/mesh.exports $(TOOLS)/mkstubs.py | $(BUILD)/mesh
	python3 $(TOOLS)/mkstubs.py $(MESHD)/mesh.imports $(MESHD)/mesh.exports $(BUILD)/mesh/imports.S $(BUILD)/mesh/exports.S

$(BUILD)/mesh/%.o: $(MESHD)/%.c include/*.h | $(BUILD)/mesh
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/mesh/%.o: $(MESHD)/%.S | $(BUILD)/mesh
	$(CLANG) $(ASFLAGS) $< -o $@

$(BUILD)/mesh/%.o: $(BUILD)/mesh/%.S | $(BUILD)/mesh
	$(CLANG) $(ASFLAGS) $< -o $@

$(BUILD)/mesh.elf: $(MESH_OBJS) $(MESHD)/mesh.ld
	$(LLD) -T $(MESHD)/mesh.ld --emit-relocs -nostdlib -static -o $@ $(MESH_OBJS)

$(BUILD)/mesh.sys: $(BUILD)/mesh.elf $(MESHD)/mesh.exports $(TOOLS)/elf2pe.py
	python3 $(TOOLS)/elf2pe.py $< $@ --exports $(MESHD)/mesh.exports --dllname mesh.sys \
	        --entry desc_DriverEntry --import-dll SCSIPORT.SYS --import-dll HAL.dll

# ---- pmctrl.sys: the video miniport for a Power Macintosh 7500/8500's Control display (drivers/pmctrl) ----
CTLD     = drivers/pmctrl
CTL_SRCS = $(wildcard $(CTLD)/*.c)
CTL_OBJS = $(patsubst $(CTLD)/%.c,$(BUILD)/pmctrl/%.o,$(CTL_SRCS)) \
            $(BUILD)/pmctrl/thunk.o $(BUILD)/pmctrl/imports.o $(BUILD)/pmctrl/exports.o

$(BUILD)/pmctrl:
	mkdir -p $(BUILD)/pmctrl

$(BUILD)/pmctrl/imports.S $(BUILD)/pmctrl/exports.S: $(CTLD)/pmctrl.imports $(CTLD)/pmctrl.exports $(TOOLS)/mkstubs.py | $(BUILD)/pmctrl
	python3 $(TOOLS)/mkstubs.py $(CTLD)/pmctrl.imports $(CTLD)/pmctrl.exports $(BUILD)/pmctrl/imports.S $(BUILD)/pmctrl/exports.S

$(BUILD)/pmctrl/%.o: $(CTLD)/%.c include/*.h | $(BUILD)/pmctrl
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/pmctrl/%.o: $(CTLD)/%.S | $(BUILD)/pmctrl
	$(CLANG) $(ASFLAGS) $< -o $@

$(BUILD)/pmctrl/%.o: $(BUILD)/pmctrl/%.S | $(BUILD)/pmctrl
	$(CLANG) $(ASFLAGS) $< -o $@

$(BUILD)/pmctrl.elf: $(CTL_OBJS) $(CTLD)/pmctrl.ld
	$(LLD) -T $(CTLD)/pmctrl.ld --emit-relocs -nostdlib -static -o $@ $(CTL_OBJS)

$(BUILD)/pmctrl.sys: $(BUILD)/pmctrl.elf $(CTLD)/pmctrl.exports $(TOOLS)/elf2pe.py
	python3 $(TOOLS)/elf2pe.py $< $@ --exports $(CTLD)/pmctrl.exports --dllname pmctrl.sys \
	        --entry desc_DriverEntry --import-dll VIDEOPRT.SYS --import-dll ntoskrnl.exe --import-dll HAL.dll

disasm: $(BUILD)/hal.elf
	$(OBJDUMP) -d -r $< > $(BUILD)/hal.lst

# ---- arcfw.xcf: the ARC firmware (src/arcfw) inside its big-endian front end (src/stage0) ----
# The firmware is little-endian plain clang code at a fixed address, 0x50000: no imports, no
# exports, no TOC.  stage0 carries it as a payload: Open Firmware's own xcoff-loader loads stage0
# in the firmware's factory big-endian mode, and stage0 switches the machine to little-endian and
# starts the firmware (no NVRAM setting involved).  arcfw.xcf is what the floppy's ARCFW.XCF is.
ARCFW      = src/arcfw
ARCFW_SRCS = $(wildcard $(ARCFW)/*.c)
ARCFW_OBJS = $(patsubst $(ARCFW)/%.c,$(BUILD)/arcfw/%.o,$(ARCFW_SRCS)) $(BUILD)/arcfw/crt0.o $(BUILD)/arcfw/thunks.o $(BUILD)/arcfw/cache.o

$(BUILD)/arcfw:
	mkdir -p $(BUILD)/arcfw

$(BUILD)/arcfw/%.o: $(ARCFW)/%.c $(ARCFW)/arcfw.h $(ARCFW)/of.h $(ARCFW)/dtsnap.h | $(BUILD)/arcfw
	$(CLANG) $(CFLAGS) -I$(ARCFW) -c $< -o $@

$(BUILD)/arcfw/%.o: $(ARCFW)/%.S | $(BUILD)/arcfw
	$(CLANG) $(ASFLAGS) $< -o $@

$(BUILD)/arcfw.elf: $(ARCFW_OBJS) $(ARCFW)/arcfw.ld
	$(LLD) -T $(ARCFW)/arcfw.ld --emit-relocs -nostdlib -static -o $@ $(ARCFW_OBJS)

$(BUILD)/arcfw.bin: $(BUILD)/arcfw.elf
	$(OBJCOPY) -O binary $< $@

# stage0: big-endian, an ordinary Open Firmware client program at 0x600000.
STAGE0    = src/stage0
TARGET_BE = --target=powerpc-unknown-linux-gnu -mcpu=604
CFLAGS_BE = $(subst $(TARGET),$(TARGET_BE),$(CFLAGS))

$(BUILD)/stage0:
	mkdir -p $(BUILD)/stage0

$(BUILD)/stage0/stage0.o: $(STAGE0)/stage0.c $(ARCFW)/dtsnap.h include/oemdisk.h | $(BUILD)/stage0
	$(CLANG) $(CFLAGS_BE) -c $< -o $@

$(BUILD)/stage0/stage0s.o: $(STAGE0)/stage0.S $(BUILD)/arcfw.bin | $(BUILD)/stage0
	$(CLANG) $(TARGET_BE) -x assembler-with-cpp -c -DPAYLOAD=\"$(BUILD)/arcfw.bin\" $< -o $@

$(BUILD)/stage0.elf: $(BUILD)/stage0/stage0s.o $(BUILD)/stage0/stage0.o $(STAGE0)/stage0.ld
	$(LLD) -T $(STAGE0)/stage0.ld -nostdlib -static -o $@ $(BUILD)/stage0/stage0s.o $(BUILD)/stage0/stage0.o

$(BUILD)/stage0.bin: $(BUILD)/stage0.elf
	$(OBJCOPY) -O binary -j .text $< $@

# .bss's place and size from the ELF: mkxcoff.py checks the place is right after the .data page
$(BUILD)/arcfw.xcf: $(BUILD)/stage0.bin $(BUILD)/stage0.elf $(TOOLS)/mkxcoff.py
	python3 $(TOOLS)/mkxcoff.py $< $@ 0x00600000 $(BUILD)/stage0.elf

# The boot floppy:
#
#     make floppy                    # -> build/boot-floppy.img
#
# Builds the HAL, the ADB driver and the ARC firmware, and lays them out with
# a txtsetup.oem and the three Open Firmware scripts.  Nothing on it is
# Microsoft's: the firmware is this project's (src/arcfw), SETUPLDR and the
# Cirrus display pair stay on the CD, where the firmware and Setup load them.
# The image may be redistributed under GPL-2.0-only.  (ISO= is ignored.)
floppy: all
	python3 $(TOOLS)/mkfloppy.py $(if $(ISO),--iso "$(ISO)")

# An empty disk for NT to install onto:
#
#     make disk                      # -> build/nt-disk.img, 512 MB
#     make disk DISK=my.img SIZE=800
#
# It holds no files -- only a partition table with an ARC system partition,
# which Setup requires before it will start ("System partitions are created and
# managed by a manufacturer-supplied configuration program").  On an IBM or
# Motorola ARC machine that program is ARCINST.EXE; this machine has no ARC
# firmware of its own, so mkarcdisk.py does that job here.
#
# 32 MB at LBA 4096 for the system partition, the rest for NT.  Install to the
# SECOND one; the first exists to hold OSLOADER.EXE and HAL.DLL where the
# firmware can read them.
DISK ?= $(BUILD)/nt-disk.img
SIZE ?= 512
disk: | $(BUILD)
	@test ! -e "$(DISK)" || { echo "$(DISK) exists -- remove it, or set DISK="; exit 1; }
	python3 $(TOOLS)/mkarcdisk.py --size-mb $(SIZE) --part 4096:65536 --part 69632:0 "$(DISK)"

clean:
	rm -rf $(BUILD)

.PHONY: all clean disasm floppy disk
