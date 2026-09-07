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

## Conventions

Every claim names its source: a thread post id, an Apple document and section, a chip manual, or
a file in [`../traces/`](../traces/). Dead ends are recorded alongside results — several of the
theories in these documents were wrong within hours, and saying so is the point.
