<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright (C) 2026 powermac-nt-hal contributors -->

# adbport.sys

This project's own Windows NT 4.0 PowerPC driver for the Apple Network Server's Apple Desktop
Bus keyboard and mouse — and for the OEM disk image the boot floppy leaves in RAM, which it
serves to Setup as `\Device\Floppy0`.

**Status: loaded and working — text-mode Setup completes with it (16 September 2026).** SETUPLDR
loads it under `[SCSI]`, `DriverEntry` brings up all three devices (`adbport: up; keyboard,
pointer, OEM disk as \Device\Floppy0`), `kbdclass` connects (`keyboard class connected`), every
Setup screen from Welcome to *"This portion of Setup has completed successfully"* was driven from
the ADB keyboard, and Setup's OEM-file copy read the RAM floppy through `\Device\Floppy0`. The
mouse path has not been exercised (text-mode Setup does not use one). See the boot-floppy note,
§4.7 and E21–E23, for the three faults found on the way — none of them in this driver.

## Why it exists

A stock NT 4.0 CD installs on this machine with everything of ours on a floppy (the plan above),
and text-mode Setup runs through every screen — until it copies the OEM files onto the hard disk.
That copy happens under NT, through an NT floppy device, and NT has no driver for the SWIM3
behind the real drive. So `\BOOT.OF` reads the whole floppy into RAM before `go`, the HAL checks
and fences that region (`src/oemdisk.c`), and this driver serves it as drive A:. The keyboard and
mouse are in the same binary because Setup loads OEM drivers through one prompt that accepts any
kernel driver — the SCSI one — and each extra driver is another trip through it. It replaces
maciNTosh's `usbadb.sys`, which this project may run but may not redistribute (ledger row 9).

## What it does

| device | how |
|---|---|
| `\Device\KeyboardPort0` | the HAL hands us every ADB packet from its DPC through `HalPxiAdbSetCallback`; ADB address 2, Talk Register 0, becomes set-1 scancodes for `kbdclass` (`kbd.c`) |
| `\Device\PointerPort0` | ADB address 3 becomes `MOUSE_INPUT_DATA` for `mouclass` (`mouse.c`) |
| `\Device\Floppy0` | `HalAnsOemDiskQuery` says where the image is; `MmMapIoSpace` maps it; reads, writes and the disk IOCTLs `setupdd` and `fastfat` send a floppy are answered from RAM (`ramdisk.c`) |

The hand-off is `include/oemdisk.h`, shared by `tools/mkbootfloppy.py` (writes the header),
the HAL (validates it, marks the pages firmware-permanent) and this driver.

## Building

`make` at the top of the repository builds `build/adbport.sys` next to `build/hal.dll`, with the
same clang/lld/`elf2pe.py` toolchain. `adbport.imports` names two DLLs; `tools/mkstubs.py` and
`elf2pe.py` learnt import groups for it.

## What is unverified

Everything, in this order of likelihood of being wrong:

1. The NT 4.0 structure layouts in `adbport.h`. Each is what the DDK documents, pinned with a
   static assert; `DRIVER_OBJECT.DriverName` at `0x1C` is confirmed from a shipped binary, the
   rest are not. A wrong `IO_STACK_LOCATION` size shows up as garbage IOCTL codes; a wrong
   `IRP` shows up as a bugcheck on the first completion.
2. Which DLL exports what. `KeAcquireSpinLock` from the kernel and `KeRaiseIrql` from the HAL is
   how a working driver on this platform has it; a wrong guess is an unresolved import at load.
3. Whether `kbdclass` is happy with the attributes we report, and whether Setup's key handling
   needs the E0 flags exactly as set.
4. The OEM disk's physical address (`0x03B97000`) surviving SETUPLDR's own allocations — the HAL
   validates the header and the FAT boot sector, so a collision is a trace line, not garbage.

Delivered under `[SCSI]` by `txtsetup.oem`; at the mass-storage screen, `S`, `Other`, Enter.
