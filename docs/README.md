# Documents

| | |
|---|---|
| [`HOWTO.md`](HOWTO.md) | **Start here to run anything.** Installing Windows NT 4.0 on the emulated Network Server from a cold machine to the logon screen: the three Open Firmware lines and every Setup key |
| [`EMULATOR.md`](EMULATOR.md) | Getting and building the emulator this HAL is developed against, what you must supply yourself, and the daemon workflow for iterating on the HAL |

## Session write-ups, in order

Each is the register-level record of one part of the work, with its evidence. Several theories
in them were wrong within hours, and saying so is the point.

| | |
|---|---|
| [`2026-09-05-phase0-probes.md`](2026-09-05-phase0-probes.md) | Before a line of HAL existed: what Setup's computer-type menu accepts, what `HALEAGLE.DLL` and the other shipped HALs need, and the veneer's `OFClose` leak |
| [`2026-09-06-first-boot.md`](2026-09-06-first-boot.md) | The HAL boots the kernel. `SETUPLDR` binds `NTKRNLMP.EXE` to a HAL built here, and NT prints its banner on ttya and on the 54M30 |
| [`2026-09-06-boot-device-and-video.md`](2026-09-06-boot-device-and-video.md) | SCSI end to end and the boot device: the adapter-channel stub that made every request fail, the hidden-pointer ABI rule, and `cirrus.sys` identifying the 54M30 |
| [`2026-09-06-adb-keyboard.md`](2026-09-06-adb-keyboard.md) | The ADB keyboard: Cuda over Grand Central's VIA, the three `HalPxi*` entry points a keyboard driver imports from the HAL, interrupt ownership |
| [`2026-09-07-partitions-and-arc-environment.md`](2026-09-07-partitions-and-arc-environment.md) | The partition table and the ARC environment: the three MBR exports NT puts on the HAL side, and the system partition `ARCINST.EXE` would have made |
| [`2026-09-07-booting-the-installed-disk.md`](2026-09-07-booting-the-installed-disk.md) | Walls 45 to 48: two loader faults, two workarounds of ours applied where they no longer held, and the COFF symbol tables the veneer and OSLOADER shipped with |
| [`2026-09-14-the-toc-slot.md`](2026-09-14-the-toc-slot.md) | Wall 49: NT PowerPC keeps the caller's TOC at `4(r1)`, the SVR4 ABI puts the LR save slot there, and every HAL export clobbered its caller |
| [`2026-09-15-the-boot-floppy.md`](2026-09-15-the-boot-floppy.md) | **A stock CD, not one byte written.** Everything this project adds moves onto a floppy; the firmware reads the veneer off it as raw blocks and Setup meets it as its OEM disk |
| [`2026-09-16-handover-the-bitblt-engine.md`](2026-09-16-handover-the-bitblt-engine.md) | Work in progress at the time: GUI Setup draws nothing because `cirrus.dll` reaches for the GD5430's BitBLT engine |
| [`2026-09-17-the-bitblt-engine.md`](2026-09-17-the-bitblt-engine.md) | **GUI Setup draws.** The BitBLT engine and the legacy VGA window, modelled and mirrored; the install completes and the installed system boots to logon |
| [`2026-11-05-adbport-layouts-verified.md`](2026-11-05-adbport-layouts-verified.md) | The NT structure layouts `adbport.sys` relies on, read out of the shipped kernel and of Microsoft's own class and port drivers, instruction by instruction, and a checker that repeats the reading on any build |
| [`2026-11-05-the-vga-window.md`](2026-11-05-the-vga-window.md) | **A stock `cirrus.sys` boots.** The HAL translates the legacy VGA window the miniport asks for, so ledger row 6, the OEM display class and the two Cirrus files on the floppy are retired; the floppy is eight files |
| [`2026-11-05-the-arc-firmware.md`](2026-11-05-the-arc-firmware.md) | **Both boots go through this project's own ARC firmware; nothing of Microsoft's is on the floppy.** `src/arcfw`, an ARC firmware over Open Firmware's client interface, in place of Microsoft's veneer: how the veneer is entered, what NT's loaders ask for, the configuration tree and memory descriptors they are given, the measured defects between a first run and the logon screen, and the CD route to SETUPLDR |
| [`2026-11-05-retiring-the-veneer.md`](2026-11-05-retiring-the-veneer.md) | **A plan, carried out the same day.** The last Microsoft file on the floppy is the veneer, patched two ways; what an ARC firmware of the project's own would have to do, two shapes it could take, and the six steps with the emulator test that finishes each |
| [`2026-11-06-real-hardware-the-npfs-wall.md`](2026-11-06-real-hardware-the-npfs-wall.md) | **Real hardware, read from here.** Mr. Macintosh's ANS 700 stops at `Npfs.SYS` / `C0000185`; the retained SCSI ring decoded against `symc810.sys` shows the miniport resetting the bus itself after reselections it has no request for, Grand Central's edge-latched events and posted writes (Linux's `pic.c` as the witness) explain why the emulator never sees it, and the HAL, emulator and hardware experiments that follow |

## History

| | |
|---|---|
| [`STORY.md`](STORY.md) | Every wall and how it fell, 51 of them, from the first probe to the eve of the boot floppy. The narrative; the write-ups above are its evidence |
| [`CHARTER.md`](CHARTER.md) | The plan as written on 5 September 2026 and struck through as it was carried out: the machine as a HAL sees it, what NT expects from a HAL, how Setup picks one, prior art, and the licensing reasoning |

[`../traces/`](../traces/) carries a screenshot of every Setup screen reached, in order, and
the console transcripts the write-ups quote.

## Conventions

Every claim names its source: a thread post id, an Apple document and section, a chip manual, or
a file in `traces/`. Dead ends are recorded alongside results.
