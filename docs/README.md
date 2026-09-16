# Documents

Start with [`../STORY.md`](../STORY.md) — the narrative, wall by wall. Everything here is either
a standing document (how to run this, and the project's charter) or a session write-up with the
register-level detail behind one part of that story.

| | |
|---|---|
| [`EMULATOR.md`](EMULATOR.md) | **Start here to run anything.** Step-by-step setup of the emulator this HAL is developed against — including the fact that the support it needs is an *open pull request*, not Granny Smith's mainline, and how to check you have the right tree |
| [`CHARTER.md`](CHARTER.md) | The standing document: the goal, the machine as a HAL sees it, what NT expects from a HAL and how Setup chooses one, the plan, the open questions, the conventions, and the licensing reasoning. Written to be read cold |

## Session write-ups, in order

| | |
|---|---|
| [`2026-09-05-phase0-probes.md`](2026-09-05-phase0-probes.md) | Before a line of HAL existed: what Setup's computer-type menu accepts, what `HALEAGLE.DLL` and the other shipped HALs need, and the veneer's `OFClose` instance leak — the first firmware bug that had to be patched to get past the media check |
| [`2026-09-06-first-boot.md`](2026-09-06-first-boot.md) | The HAL boots the kernel. `SETUPLDR` binds `NTKRNLMP.EXE` to a HAL built here, and NT prints its banner — first on ttya, then on the machine's own Cirrus 54M30 monitor through a HAL framebuffer console |
| [`2026-09-06-boot-device-and-video.md`](2026-09-06-boot-device-and-video.md) | SCSI end to end and the boot device: the adapter-channel stub that made every request fail, the hidden-pointer ABI rule for eight-byte return values, the veneer's `VrOpen` raw-open bug behind `STOP 0x7B`, and the four Cirrus registers `cirrus.sys` identifies the part by |
| [`2026-09-06-adb-keyboard.md`](2026-09-06-adb-keyboard.md) | The ADB keyboard: Cuda over Grand Central's VIA, the three `HalPxi*` entry points a keyboard driver imports from the HAL, interrupt ownership and DPC delivery — and why "could not load KBDUS.DLL" turned out to be about drive letters |
| [`2026-09-07-partitions-and-arc-environment.md`](2026-09-07-partitions-and-arc-environment.md) | The partition table and the ARC environment: the three MBR exports NT puts on the HAL side, the undocumented layout convention `IoWritePartitionTable` has to know (four entries per on-disk table, Setup's preference for a logical drive, and `HiddenSectors` arriving zero), why Setup asks the firmware for `SYSTEMPARTITION` rather than looking for one, and the drive-letter bug it all ends on |
| [`2026-09-14-the-toc-slot.md`](2026-09-14-the-toc-slot.md) | Wall 49's `0x50`: NT PowerPC keeps the caller's saved TOC at `4(r1)` and the SVR4 ABI we compile for puts the LR save slot there, so every HAL export that saved LR destroyed it. The conditional breakpoint that picked one fault out of thousands, and the export thunks that fixed it |
| [`2026-09-15-the-boot-floppy.md`](2026-09-15-the-boot-floppy.md) | **A stock CD, not one byte written.** Everything this project adds moves onto a floppy: the firmware reads the veneer off it as raw blocks, and Setup meets it again as an ordinary device support disk. Ledger rows 8 and 10 retired. The step-by-step boot, seventeen measurements, the two things that were in the way — an Open Firmware `open` that needs the drive touched once, and six bytes that give the drive the ARC name SETUPLDR spells floppies with — and why `pe-loader` being the only NT-ROM-specific package decides how this extends to a 7500/8500 |
| [`2026-09-16-handover-the-bitblt-engine.md`](2026-09-16-handover-the-bitblt-engine.md) | **Work in progress, not a result.** GUI Setup draws nothing because `cirrus.dll` reaches the GD5430's BitBLT engine through memory-mapped registers in the legacy VGA window, which the emulated 54M30 does not have. A real blit captured out of video memory and decoded field by field, the register map and ROP table to implement against, where it goes in the emulator, the one-line HAL change beside it, and the byte-lane question the first run has to settle |

## The Setup walk, in screenshots

[`../traces/`](../traces/) carries one screenshot per Setup screen reached, in order:
`2026-09-06-setup-01-welcome.png` through `2026-09-07-setup-21-copying-win32k.png` — the
hardware list, the partition table, the format, `\WINNT`, and Windows NT copying itself onto the
disk.

## Conventions

Every claim names its source: a thread post id, an Apple document and section, a chip manual, or
a file in [`../traces/`](../traces/). Dead ends are recorded alongside results — several of the
theories in these documents were wrong within hours, and saying so is the point.
