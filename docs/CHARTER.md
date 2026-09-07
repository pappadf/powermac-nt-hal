# Windows NT 4.0 HAL for the Apple Network Server (and TNT-class Power Macintosh) — project charter and onboarding

*The project's standing document, started 2026-09-05 and kept current. Written to be read
cold, by a person or an agent with none of the history, and to link to everything they need.
For the narrative — every wall and how it fell — read [`../STORY.md`](../STORY.md) first; for
what the project is and how to build it, [`../README.md`](../README.md).*

---

## 0. Charter

**Goal.** Write and test a Windows NT 4.0 PowerPC Hardware Abstraction Layer (`HAL.DLL`) for
the Apple Network Server 500/700 ("Shiner"), so that NT 4.0's text-mode Setup, which already
runs on the emulated machine and stops at *"Setup could not determine the type of computer
you have"*, can load a kernel and continue. The Network Server is a TNT-class machine (the
Power Macintosh 7500/8500/9500 family: Hammerhead memory controller, Bandit PCI bridges,
Grand Central I/O); a HAL that boots it is most of a HAL for those Power Macs too, and the
project should keep that door open.

**Why now.** On 2026-09-05 the Granny Smith emulator took NT 4.0 Setup on an emulated ANS 500
from the veneer to the HAL question, i.e. one screen past where the TinkerDifferent community
got on a real ANS 700 (§3). Every wall on the way was understood, and none was an emulator
defect. The HAL is the first piece that has to be *written* rather than *fixed*.

**Definition of done for phase 1.** A HAL built from source in this repository, delivered as a
file Setup will accept (§7), that lets `SETUPLDR` bind `NTKRNLMP.EXE`/`NTOSKRNL.EXE` to it and
start the kernel on the emulated ANS 500, reaching the kernel's own first console output or
its first documented stop. Phases beyond that (keyboard, SCSI, video drivers; install to
disk; real hardware) are listed in §9.

**Constraints.**
- Work on the emulator first; it has breakpoints, logpoints on device registers, checkpoints
  and full tracing. Real hardware (the thread's ANS 700) is for confirmation.
- Publish only what can be published: HAL and driver *source*, build scripts, notes, traces.
  Never commit Microsoft binaries (`VENEER.EXE`, `SETUPLDR`, `HAL*.DLL`, the CD images), Apple
  ROMs, or Apple/Symbios/Motorola PDFs. Cite them by name and origin instead.
- Nothing here may reference private research material, and nothing here may depend on a
  particular checkout of anything else. Facts move between projects; files do not (§12).
- Two projects, worked in parallel, one direction of dependency: the emulator repository may
  gain whatever the HAL work needs (tests, tooling, fixes), but nothing in *this* project may
  end up depending on the emulator checkout. It *uses* the emulator as a test rig, the way it
  would use a real machine, and must work against an unmodified upstream release.
- Every line taken from another project is logged with its origin and license before it is
  committed, and the final license follows from that log (§13). Today every candidate upstream
  is GPL-2.0.

---

## 1. Where everything is

Everything in the left column below is in this repository; paths are relative to the repository
root. The right column names outside material by title, because none of it can be republished
here: Apple's, Symbios's, Motorola's, Zilog's and Cirrus's documents are proprietary, and the
NT 4.0 media is commercial. This document was written while the project lived next to a private
research tree, and the research citations it used to link to are given by name for the same
reason.

| What | Where |
|---|---|
| **The story so far, wall by wall** — the narrative version of this document, and the best entry point if you want the *why* | [`STORY.md`](../STORY.md) |
| Session write-ups, in order | [`docs/`](.) — see [`docs/README.md`](../README.md) |
| The HAL itself | [`src/`](../src/), [`include/`](../include/), [`Makefile`](../Makefile), [`hal.exports`](../hal.exports), [`hal.imports`](../hal.imports), [`hal.ld`](../hal.ld) |
| Build and test tooling, and how the loop works | [`tools/`](../tools/), [`tools/README.md`](../tools/README.md) |
| Console output and screenshots backing every claim | [`traces/`](../traces/) |
| What is borrowed, from where, under which license | [`PROVENANCE.md`](../PROVENANCE.md) |
| The emulator this is developed against | [Granny Smith](https://github.com/pappadf/granny-smith) — [`AGENTS.md`](https://github.com/pappadf/granny-smith/blob/main/AGENTS.md), [TNT machine notes](https://github.com/pappadf/granny-smith/blob/main/docs/machines/tnt/tnt.md), [PowerPC core](https://github.com/pappadf/granny-smith/blob/main/docs/core/cpu/ppc.md), [PCI](https://github.com/pappadf/granny-smith/blob/main/docs/core/peripherals/pci.md), [53C8xx](https://github.com/pappadf/granny-smith/blob/main/docs/core/peripherals/scripts53c8xx.md) |
| Its debugging surface (breakpoints, logpoints, checkpoints, the object-model shell) | [headless-debug skill](https://github.com/pappadf/granny-smith/blob/main/.agents/skills/headless-debug/SKILL.md), [shell reference](https://github.com/pappadf/granny-smith/blob/main/docs/core/shell/shell.md) |
| How the emulated ANS was brought to Setup's HAL question in the first place | [Granny Smith's write-up](https://github.com/pappadf/granny-smith/blob/main/docs/notes/2026-09-05-ans-windows-nt-setupldr.md), [the screen](https://github.com/pappadf/granny-smith/blob/main/docs/assets/ans-nt-setup-hal-menu.png) |

Outside material, by name:

| What | How to get it |
|---|---|
| Apple, *Network Server 500/700 Hardware Developer Notes* — the address map (§4), IDSEL assignment (§4.6.2), interrupt table | Apple's developer documentation; mirrored in several retrocomputing archives |
| Apple, *Power Macintosh 7500/8500/9500 Developer Notes* — Hammerhead, Bandit, Grand Central, DBDMA, Cuda/VIA, PRAM/RTC. Apple's own framing: TNT documentation applies to the ANS unless the ANS notes say otherwise | same |
| Motorola *MPC604 User's Manual*, *PowerPC Embedded Application Binary Interface*, *60x bus* | Motorola/NXP archives |
| Symbios Logic *53C825A* data manual; Cirrus Logic *GD543x*; Zilog *Z8530 SCC*; the 6522 VIA | vendor archives |
| The TinkerDifferent thread *"Apple Network Server: MacOS-based ROMs found"* (252 posts) — the ANS 700 owner's real-hardware results, the ROM dumps, the veneer patches | <https://tinkerdifferent.com/threads/apple-network-server-macos-based-roms-found.4756/> |
| Windows NT 4.0 Workstation for PowerPC, OEM 000-48303 (Oct 1996) — `VENEER.EXE`, `SETUPLDR`, `TXTSETUP.SIF`, the seven shipped HALs | archive.org item `windowsnt40workstationoem_00048303_alt`. ISO MD5 `ab37556d72818ed082c1d01c2d7f1898` |
| Open Firmware 2.26NT for the ANS | ROM dumps in the thread above; the one used here has MD5 `ad405e01c663340c479668c70f741f1b` |
| Prior art: `entii-for-workcubes` (the HAL template), `maciNTosh` (the Cuda/ADB code and the `HalPxi*` contract), `maciNTosh-bandit` (its Bandit port, i.e. this machine's Cuda address) | §5, and [`PROVENANCE.md`](../PROVENANCE.md) |

---

## 2. The machine, as a HAL sees it

The ANS 500/700 is a Power Macintosh 9500 logic board with server changes. Apple's own
framing (Hardware Developer Notes §1): the TNT documentation applies unless the ANS notes
say otherwise. The complete delta is one table,
Apple, *Network Server Hardware Developer Notes* §1 (the ANS-vs-TNT delta); the parts a
HAL cares about:

| Block | Chip | Doc | HAL duty |
|---|---|---|---|
| CPU | PowerPC 604 (500: 132 MHz), 604e (700: 150/200 MHz); one card, MP possible | Apple ANS developer notes (CPU card, MP); Motorola *MPC604 User's Manual*, Motorola *MPC604 User's Manual* | decrementer clock, timebase, BATs, cache ops, **little-endian mode** (NT runs the 604 in LE with address munging: MSR ILE/LE, `ppc.md` "Little-endian mode") |
| Memory controller | Hammerhead `@F8000000` | Apple TNT developer notes (Hammerhead); Hammerhead ERS, Apple ANS developer notes (memory, parity, cache), Hammerhead ERS in Hammerhead ERS, from the thread attachments | nothing at boot (firmware sized it); memory map from the ARC descriptors |
| PCI bridges | two Bandits, `@F2000000` (slots 1–2, both 53C825As, GC, 54M30) and `@F4000000` (slots 3–6) | Apple TNT developer notes (Bandit, Chaos, PCI), Apple ANS developer notes §4.6.2 (IDSEL and slots), [`docs/core/peripherals/pci.md`](https://github.com/pappadf/granny-smith/blob/main/docs/core/peripherals/pci.md) | config space access (address port `base+800000`, IDSEL-encoded), `HalGetBusData`/`HalTranslateBusAddress`, the **endian bit** in mode-select (reg `0x50`, bit 24) that reverses byte lanes for a LE CPU |
| I/O | Grand Central at Bandit 1 IDSEL 16 (`gc@10`) | Apple TNT developer notes (Grand Central), Apple TNT developer notes (DBDMA) | **the interrupt controller** (mask/events/clear registers), DBDMA for its devices |
| Interrupts | GC internal sources unchanged from the 9500; **the eleven external lines re-purposed**: EXT2/EXT6 = the two 53C825As, EXT1 = both Bandits' error interrupt, EXT3–9 = slots 1–6 | Apple's own interrupt table, ANS developer notes §4 (Apple's own table), Apple TNT developer notes (interrupt map) | vector table, `HalGetInterruptVector`, dispatch |
| SCSI | two Symbios 53C825A (IDSEL 17/18, `53c825@11`/`@12`), plus GC's 53C94 external bus; **no MESH** | Symbios Logic *53C825A* data manual, AIX's `pscsidd` driver, reverse-engineered (AIX's driver reverse-engineered), Symbios/LSI 53C8xx manuals | not the HAL's; the miniport's (§8) |
| Video | Cirrus 54M30 PCI (`54m30@F`), VGA-class, little-endian framebuffer window, no interrupt | Cirrus Logic *GD543x* manual (the 54M30), Cirrus Logic *GD543x* manual | `HalDisplayString` (the blue-screen console) if the ttya route is not used |
| Serial | ESCC (Z8530) in GC; ttya is the firmware console in this project | Zilog *Z8530 SCC* manual | kernel debugger port; optional boot console |
| Cuda / VIA | Cuda microcontroller over VIA; ADB keyboard/mouse, RTC, PRAM, **system reset** | Apple TNT developer notes (Cuda, VIA, ADB), Apple TNT developer notes (PRAM, RTC) | `HalQueryRealTimeClock`, reboot/power-off; keyboard is a driver's job (§8) |
| Board | GBUS: keyswitch, LCD (device 3), board registers `0x1A000`/`0x1E000`, MP doorbell | Apple ANS developer notes (GBUS board registers) | optional: LCD progress codes; MP later |
| Floppy, sound, Ethernet | SWIM3, AWACS, MACE (AAUI only) in GC | TNT notes | none for phase 1 |

Two facts about the environment NT runs in on this machine, both established in the emulator:

- **The firmware and everything after it run in little-endian mode.** `MSR = 0x1B071` at the
  Open Firmware prompt once `little-endian? true` is set. The 604 munges addresses (byte at
  A ↔ physical A^7, word at A ↔ physical A^4), and the Bandit's endian bit makes PCI DMA and
  MMIO agree with that view. A HAL author must think in this model for every MMIO access;
  the emulator's `debug.disasm` compensates for code, `machine.memory.peek/poke` do not.
- **The ARC environment is Microsoft's `VENEER.EXE`** (FirmWorks, 1996), which converts the
  Open Firmware device tree into an ARC configuration tree and implements the ARC firmware
  vector over Open Firmware's client interface. What the HAL and kernel will see is exactly
  the tree in the trace
  2026-09-05-go-bootpath-cd-veneer-trace.txt
  (`dump_tree` section): root `device-tree` (Class System, Identifier `device-tree`),
  `PowerPC-604` + caches, `memory`, `multi(0)` = PCI with `scsi(0)`→`cdrom(0)`→`fdisk(0)`,
  `scsi(1)`→`disk(0)`→`rdisk(0)`, `scsi(0)` (53C94), `net(0)`, `serial(0/1)`, `other`
  (SWIM3), `key`, `point`, `video`; `multi(1..3)` for the second Bandit and the two
  `pci106b-1` nodes. Memory descriptors, environment (`OSLOADER=`, `CONSOLEIN/OUT=multi(0)serial(0)line(0)`,
  `FIRMWAREVERSION=OpenFirmware2.26`, `VENEERVERSION=FirmWorks,ENG,3.0,…`) are in the same trace.

---

## 3. What has been reached, and how (the reproducible baseline)

The ladder is kept current in ONBOARDING §0.
State on 2026-09-05:

1. 2.26NT Open Firmware boots, console on ttya.
2. Little-endian configuration reboot.
3. `VENEER.EXE` read off a SCSI disk, laid out by the ROM's `pe-loader` at `0x50000`.
4. Veneer runs (the real ANS 700 faults at `0x50014` here; the emulator does not).
5. `Open Firmware ARC Interface Version 3.0 (Jul 12 1996 - 18:46:28)`.
6. Two `claim` failures worked around by two `nop`s (the thread's patches).
7. `Booting from 'multi(0)scsi(0)cdrom(0)fdisk(0)\PPC\SETUPLDR'`, SETUPLDR loaded and entered.
8. *Windows NT Setup*; SETUPLDR mounts the CD with its own CDFS over raw ARC reads.
9. `TXTSETUP.SIF` parsed; *Setup is loading files (Windows NT Executive)…*; `NTKRNLMP.EXE`
   read in full (in memory, not executing).
10. **"Setup could not determine the type of computer you have"** — the HAL menu.
11. *(2026-09-05 evening, [docs/2026-09-05-phase0-probes.md](2026-09-05-phase0-probes.md))*
    Keys reach the menu through ttya; *MOTOROLA PowerStack* is accepted and `HALEAGLE.DLL`,
    the configuration data, font and NLS files load; the media check then fails because the
    veneer's `OFClose` leaks Open Firmware instances (nret bug, two-word patch).
12. With the patch: `setupdd.sys`, PCMCIA and `scsiport.sys` load; **Setup's mass-storage
    screen** (`<none>` detected — the 825As are `UNKNOWN SCSI`). Enter → `atapi.sys`,
    `ntfs.sys`; **Setup's video-adapter menu** (has *Motorola Power Stack (cirrus 54xx)*).
13. Pick it → `cirrus.sys`, floppy, SCSI class drivers, keyboard, FAT, CDFS load and **SETUPLDR
    enters `NTKRNLMP.EXE`** (relocated to `0x8007B000`; `HALEAGLE.DLL` at `0x80647000`). The
    kernel initialises for ~3 G instructions, then the PowerStack HAL probes PCI
    configuration space through the PReP window at `0x80800000`, which on this machine is
    Bandit PCI memory space: master abort → machine check → the retried load has a bad
    address → alignment exception → **bugcheck `0x1E`**, whose `HalDisplayString` then faults on
    the absent VGA text buffer at `0xB8000`. First measured ANS HAL requirements
    (docs note, addendum 4): PCI config via Bandit's port, a console that exists.
14. *(2026-09-06, [docs/2026-09-06-first-boot.md](2026-09-06-first-boot.md))* **This
    project's HAL boots the kernel.** `halshinr` build 7, built with clang/lld/`elf2pe.py`,
    is loaded and bound by SETUPLDR; the kernel prints *Microsoft (R) Windows NT (TM) Version
    4.0 (Build 1381)* and *1 System Processor [64 MB Memory] MultiProcessor Kernel* — and, as of
    the same day, on the on-board **Cirrus 54M30 monitor** as well as ttya (a HAL framebuffer
    console, [docs/2026-09-06-first-boot.md](2026-09-06-first-boot.md); screenshot in
    traces/). Phase 1's definition of done.
    phase 1's definition of done. Builds 10-18 then carry it through driver initialisation:
    a machine-check handler for stray PCI probes, no legacy-port translation, the i8042 driver
    kept out of the load list, the veneer's SCSI identifier renamed, and the ARC tree's
    ScsiAdapter resource lists rebuilt from real config space. NT then stopped at
    **INACCESSIBLE_BOOT_DEVICE**.
15. *(2026-09-06 evening, [docs/2026-09-06-boot-device-and-video.md](2026-09-06-boot-device-and-video.md))*
    **Setup runs on the monitor and asks for a keyboard.** Builds 19-28. Four walls:
    `HalAllocateAdapterChannel` was a stub, so `ScsiPortStartIo` completed every request with
    an error and the miniport was never asked to do anything (that, not the interrupt path,
    was why no interrupt ever arrived); an eight-byte return value comes back through a hidden
    pointer in `r3` on this ABI, which `IoMapTransfer` and `KeQueryPerformanceCounter` had
    wrong; the veneer's `VrOpen` opens `partition(0)` on the CD as the ISO **root directory**,
    so SETUPLDR's ARC disk checksum never matched and `\ArcName\…` was never created (a third
    veneer patch, one word); and the emulated Cirrus lacked `CR27`, `SR06` and `SR15`, the
    three registers `cirrus.sys` identifies and sizes the part with. Now: the SCSI bus is
    scanned, the CD is the boot device, `cirrus.sys` initialises, and Setup's next screen is
    *"Setup did not find a keyboard connected to your computer"* — the Network Server's
    keyboard is ADB and NT 4.0 ships no ADB port driver.
16. *(2026-09-06 later, [docs/2026-09-06-adb-keyboard.md](2026-09-06-adb-keyboard.md))*
    **The keyboard works.** `source/cuda.c` adds a Cuda transport over Grand Central's VIA and
    the three ADB entry points a keyboard driver written to maciNTosh's contract imports from
    the HAL (`HalPxiCommandAdb`, `HalPxiAdbSetCallback`, `HalPxiAdbAutopoll`); the HAL takes
    Grand Central bit 18 for itself so auto-polled packets have an owner. maciNTosh's
    `usbadb.sys`, written over `\PPC\I8042PRT.SYS`, scans the ADB bus through it (keyboard at
    address 2, mouse at 3) and a real keystroke reaches its callback. Setup gets past the
    keyboard check and launches `usetup.exe`, which then cannot load its keyboard **layout**
    DLL.
17. *(2026-09-06 night, [STORY.md](../STORY.md) walls 31-32)* **Setup runs.** `usetup.exe`'s
    *"could not load the keyboard layout file KBDUS.DLL"* was not a file problem: it loads the
    layout itself with `LdrLoadDll` and a **rooted** DOS path, which resolves against the
    current drive — and there were no drives, because `IoAssignDriveLetters` (a HAL export) was
    a stub. Implemented in `source/misc.c`: floppies get `A:`/`B:`, hard-disk partition 1s get
    `C:` upward, then the CD-ROMs, and `NtSystemPathString` is rewritten with the boot drive's
    letter (`\Device\CdRom0` -> `D:`, system path `D:\PPC\`). Setup then reaches **"Welcome
    to Setup"**, the mass-storage list, the licence agreement and the hardware-confirmation
    screen (screenshots in [`traces/`](../traces/), `2026-09-06-setup-01-welcome.png` onward).
18. *(2026-09-06 night, [STORY.md](../STORY.md) walls 32-35)* **Setup reaches its partition
    screen.** Four more walls behind *"No valid system partitions are defined on this
    computer"*. `IoReadPartitionTable`, `IoWritePartitionTable` and `IoSetPartitionInformation`
    are HAL exports too, and were stubs, so every disk read back with no partitions —
    `source/disk.c` implements all three over `IoBuildSynchronousFsdRequest`/`IoCallDriver`.
    Twice the compiler put back the misaligned access the code was written to avoid: clang folds
    four byte loads into one `lwz` (fixed with `volatile`) and merges byte stores into `stw`
    (fixed globally with `-mllvm -combiner-store-merging=false`), each one an alignment
    exception on a little-endian 604. And Setup does not *look* for a system partition, it asks
    the firmware for `SYSTEMPARTITION` through `HalGetEnvironmentVariable` — a fourth stub; the
    HAL now keeps a real ARC environment and seeds that variable from the loader's own ARC disk
    list. The disk itself is prepared by `tools/mkarcdisk.py`, standing in for the `ARCINST.EXE`
    an ARC machine's firmware vendor would ship. Setup now lists
    `C: FAT 6 MB (5 MB free)` and offers to install; the only thing left is that the emulated
    disk is 8 MB and NT wants 158.
19. *(2026-09-07, [`../STORY.md`](../STORY.md) walls 36-40, [`docs/2026-09-07-partitions-and-arc-environment.md`](2026-09-07-partitions-and-arc-environment.md))* **Setup partitions, formats and
    starts installing.** On a 512 MB disk Setup lists the partitions, creates one in the free
    space, formats it, takes `\WINNT` as the install directory, skips the surface scan and
    reaches *"Creating directory \WINNT…"*. `IoWritePartitionTable` had to learn the whole
    layout convention: `disk.sys` hands down four entries per on-disk table, Setup makes the
    new partition a **logical drive inside an extended partition**, and `HiddenSectors` arrives
    zero, so every relative-sector field has to be computed from the absolute offsets — a
    partition entry counting from the table it sits in, a chain link from the outermost
    extended partition. Then `setupdd.sys` crashed, and pre-creating the partition instead
    showed why: `IoAssignDriveLetters` gives a letter to only the *first* partition of each
    disk, so the volume Setup installs to has no `\DosDevices\` entry and the letter it wants
    went to the CD. ← here.

Three things had to be understood to get from 7 to 10; a HAL project inherits all of them:

- `/chosen bootpath` must name the CD, or the veneer boots from `device-tree(0)` (ENODEV).
- SETUPLDR's `BlGenerateDeviceNames` rejects any component after `cdrom(N)fdisk(N)`; the
  veneer appends `partition(1)` to every boot path. One byte in the veneer fixes it.
- Apple's `disk-label` gives raw sector access only for an *empty* Open Firmware argument;
  the veneer's `VrOpen` appends `:0`. One more byte.
- The same fault has a second half, which only the kernel notices (2026-09-06): `VrOpen` has a
  separate path for an ARC `partition(N)` component that builds `":" + digits` of its own, so
  SETUPLDR's `BlReadSignature` — which opens `<boot path>partition(0)` — gets a filesystem open
  and checksums the CD's root directory instead of its volume descriptor. `IoInitSystem` then
  cannot create `\ArcName\<boot device>` and bugchecks `0x7B`. One word in the veneer
  (`machine.memory.poke.l 0x5474c 0x60000000`) sends `partition(N)` with no file path down the
  raw route.

### 3.1 Replicating it

**What you have to supply.** None of it can ship here.

| | |
|---|---|
| The emulator | [Granny Smith](https://github.com/pappadf/granny-smith) at branch **`ppc-le-mode-and-bandit-lane-reversal`** ([PR #135](https://github.com/pappadf/granny-smith/pull/135)), built with `make headless` (~1 min). **Not `main`** — little-endian mode, Bandit lane reversal and the Cirrus 54M30 id registers are all on that branch and unmerged as of 2026-09-07, and without them the firmware's little-endian reboot fails. Step by step: [`EMULATOR.md`](EMULATOR.md) |
| Open Firmware 2.26NT for the ANS | a ROM dump from the TinkerDifferent thread; the one used throughout has MD5 `ad405e01c663340c479668c70f741f1b` |
| Windows NT 4.0 Workstation for PowerPC | the OEM 000-48303 (Oct 1996) CD image, MD5 `ab37556d72818ed082c1d01c2d7f1898`, archive.org item `windowsnt40workstationoem_00048303_alt` |
| A staging disk | a raw disk image with `VENEER.EXE` (from the CD's `\PPC`) written at block `0x800`, which is where the ROM's `pe-loader` is told to read it from |
| A hard disk for NT | any raw image; [`tools/mkarcdisk.py`](../tools/mkarcdisk.py) gives it the FAT16 system partition that an ARC machine's `ARCINST.EXE` would create |
| `clang-18`, `lld-18`, `python3` | to build the HAL. No Microsoft toolchain, no DDK |

**Getting to Setup's computer-type menu.** This part is the emulator side of the work rather
than the HAL's, and the turnkey script for it lives with the emulator project. Set the emulator
up first — [`EMULATOR.md`](EMULATOR.md) — because every step below assumes a build that has
little-endian mode. What it does, in
order, is worth stating because a HAL author inherits the result:

1. Set `little-endian? true` and reboot the firmware into little-endian mode (`MSR = 0x1B071`).
2. Read `VENEER.EXE` off the staging disk with the ROM's `pe-loader` package (blocks `0x800`
   onward, `0x27800` bytes) and lay it out at `0x50000`.
3. Two `nop`s over the veneer's `claim` failure paths (the thread's patches, post 49785).
4. Point `/chosen` `bootpath` at the CD: `" /bandit/53c825@11/sd@0,0" encode-string " bootpath" _chosen (property)`. Without this the veneer boots `device-tree(0)` and gets ENODEV.
5. Two one-byte pokes so the ARC boot path is one SETUPLDR accepts:
   `00 5D0C0 c!` blanks the `partition(1)` the veneer appends to every path
   (`BlGenerateDeviceNames` rejects anything after `cdrom(N)fdisk(N)`), and `00 5E168 c!`
   blanks the `:0` it appends to the Open Firmware argument (Apple's `disk-label` gives raw
   sector access only for an *empty* argument).
6. `go`.

**The four veneer patches this project adds**, applied as memory pokes after the veneer is
loaded. Each one is a bug in Microsoft's ARC shim that only shows up on this firmware; each is
one or two words, and each is explained in the write-up named beside it:

```
machine.memory.poke.l 0x52250 0x39200000   # OFClose returns nret=1 -> 0 ...
machine.memory.poke.l 0x5225c 0x91230008   #   ... without these every close fails, twelve Open Firmware
                                           #   instances leak and the CD stops opening (docs/2026-09-05-phase0-probes.md §3)
machine.memory.poke.l 0x5474c 0x60000000   # VrOpen: partition(N) with no file path is a RAW open, not a
                                           #   disk-label filesystem open.  Without it SETUPLDR checksums the
                                           #   CD's root directory instead of its volume descriptor, IoInitSystem
                                           #   cannot create \ArcName\<boot device> and the kernel bugchecks 0x7B
                                           #   (docs/2026-09-06-boot-device-and-video.md §3)
machine.memory.poke.l 0x60C0C 0x220        # optional: VrDebug (0x20 main, 0x200 I/O, 0x100 tree, 0x400 dump)
```

Eleven further bytes rename the veneer's SCSI identifier from `NCR,53C810` to `NCR,825A` so
Setup's mass-storage detection picks `symc810.sys` for the 53C825As; see `STORY.md` wall 16.
These are all load-time pokes into a proprietary binary, they are all in the workaround ledger
at the end of `STORY.md`, and a published project needs a better answer than "type these into
Open Firmware" — see §9.

**The interactive loop.** Take one checkpoint at the computer-type menu, then iterate against
it: [`tools/run-hal.py`](../tools/run-hal.py) restores that checkpoint, writes the freshly built
`hal.dll` into the CD image's `HALEAGLE.DLL` extent, answers the three Setup menus, runs the
kernel and captures ttya plus screenshots. Seconds to minutes per try instead of a ten-minute
cold boot. [`tools/README.md`](../tools/README.md) documents every option;
[`tools/gsh.py`](../tools/gsh.py) is the emulator shell client the rest of them use.

Gotchas that cost time, all still true:

- Send scripts to the daemon as `include "file"`; a script over ~2 KB down the socket resets it.
- A loop inside an include prints no heartbeats, so `GS_IDLE` must exceed the longest run chunk.
- `property` at the Open Firmware prompt does not set `bootpath`; `(property)` with `_chosen` does.
- Type Open Firmware lines in four-character chunks — the firmware drops characters from bursts.
- Host-side pokes of little-endian words go to `A ^ 4`, bytes to `A ^ 7`. The emulator's
  `debug.disasm` compensates for code; `machine.memory.peek`/`poke` do not.
- A checkpoint embeds the emulator's build id and is refused by any other build; after
  rebuilding the emulator, [`tools/restamp-ckpt.py`](../tools/restamp-ckpt.py) rewrites it,
  which is safe only when the rebuild changed no checkpointed structure.

**Breakpoints in SETUPLDR** work on its virtual addresses (image base `0x80600000`; SETUPLDR
runs with translation on, stack in low physical memory). Example that caught the EINVAL:
`debug.breakpoints.add 0x80604344` (`SlFriendlyError`), then read `r3` (status), `r4`
(name), `r5` (line), `r6` (source file) and `lr`. The symbol table gives every address
(setupldr-symbols.txt).

---

## 4. What NT expects from a HAL, and how Setup will pick ours

**Selection.** Text-mode Setup keys the HAL on the ARC root component's `Identifier`
through `TXTSETUP.SIF`'s `[Map.Computer]` (from the NT 4.0 PowerPC CD):

```
[Computer]                                            [Map.Computer]
sandal_up      = "IBM Power Series 6015",files.none    sandal_up      = IBM-6015
wood_up        = "IBM Power Series 6020,40,42",…       wood_up        = IBM-6020 / IBM-6040 / IBM-6042
carolina_up    = "…6050,6070 and RS/6000 Model 7248"   carolina_up    = IBM-6070
victory_up/mp  = "IBM RS/6000 Model E20/E30/F30", "45M/H45"   IBM-VICT, IBM-7042 / IBM-7043, IBM-7442
powerstack_up  = "MOTOROLA PowerStack"                 powerstack_up  = PowerStack
powerstack2_up = "MOTOROLA PowerStack2"                powerstack2_up = PowerStack2
bigbend_up     = "MOTOROLA Big Bend"                   bigbend_up     = "MOTOROLA-Big Bend"
powerized_up/mp = "Powerized ES, MX, LX, TX …"          Powerized_ES / _MX / _LX / _TX
```

The veneer reports `device-tree` (the Open Firmware root's `name`), which matches nothing, hence
the menu. Two ways in:

- **Patched CD** (the emulator can attach any ISO): add `shiner_up = "Apple Network Server 500/700",files.none`
  to `[Computer]`, `shiner_up = AAPL,ShinerESB` (or `device-tree`) to `[Map.Computer]`, a
  `[Files.hal…]`-style entry pointing at our `HALSHINR.DLL` in `\PPC`, and optionally patch the
  veneer's `convert_name` so the root identifier becomes `AAPL,ShinerESB` (the Open Firmware
  root's `compatible`). Setup then never asks.
- **Hardware-support disk via "Other"** — a `TXTSETUP.OEM` on a floppy. On this machine Setup
  asks for Drive A: and cannot open it: the veneer's tree has no `FloppyDiskPeripheral`
  (docs note §2).
- **Preinstall** — `\PPC\winnt.sif` with `[unattended] OemPreinstall = yes` and
  `computertype = "<name>", OEM` makes SETUPLDR load the HAL named by
  `\$OEM$\TEXTMODE\txtsetup.oem` from the boot CD. Additive to the ISO, no floppy
  (docs note §4). **Preferred.**

**The interface.** The HAL is a PE DLL exporting the `Hal*` set the kernel and drivers import
(`HalInitSystem`, `HalInitializeProcessor`, interrupt enable/disable/vector, IRQL
raise/lower, `HalDisplayString`, `HalQueryRealTimeClock`, `HalGetBusData`,
`HalTranslateBusAddress`, `HalReturnToFirmware`, cache and DMA helpers, `KeStallExecutionProcessor`,
the profile/clock interrupt path, …). Two authoritative lists are on disk: the export table of
the shipped HALs (`HALPPC.DLL`, `HALPS.DLL`, … in the extracted `PPC/` directory), and
`halartx/source/hal.def` in entii-for-workcubes (§5). The shipped HALs are PE DLLs with MZ
stubs; `coff-dis.py` expects bare COFF, so extend it or read them with any PE tool.

**The loader block.** The kernel is entered by `OSLOADER`/`SETUPLDR` with a `LOADER_PARAMETER_BLOCK`
whose ARC-side content is what the veneer built: memory descriptors (`VrCreateMemoryDescriptors`
in the trace: `MemoryFirmwarePermanent 0–9`, `MemoryFree`, `MemoryLoadedProgram 0x600 +0x47`,
`MemoryFirmwareTemporary 0x800 +0x34ff` …), the configuration tree, the environment. A HAL
reads the configuration tree for its own devices (`HalpInitializeInterrupts` etc.), so the
tree above is the contract.

---

## 5. Prior art (verified 2026-09-05)

| Project | What it is | Source? | Use to us |
|---|---|---|---|
| [Wack0/entii-for-workcubes](https://github.com/Wack0/entii-for-workcubes) | NT 3.51/4.0 on GameCube/Wii; own ARC firmware and a **from-scratch HAL** (`halartx/`, ~50 files: `init.c`, `ints.c`, `irql.c`, `clock.c`, `display.c`, `kd.c`, `rtc.c`, `cache.s`, `hal.def`, `nthal.h` …) and drivers | **yes** | **The template.** The only public NT 4 PowerPC HAL source; shows every entry point and a working build. Setup presents it as a one-entry *Other* list. |
| [Wack0/peppc](https://github.com/Wack0/peppc) | "GCC 9 (Retro68) fork — compiler targeting PowerPC Windows NT (with PASM.EXE assembler and VC4.x linker)" | yes | The compiler (§6). |
| [MCJack123/maciNTosh-bandit](https://github.com/MCJack123/maciNTosh-bandit) | NT on Old World TNT Power Macs (7300/7500/7600/8500/8600): Bandit, Hammerhead, Grand Central | **ARC firmware and loader only** (`arcbandit/`, `arcloader_bandit/`), and that source includes **Cuda/ADB for Grand Central** (`arcbandit/source/{pxi.c,adb_bus.c,adb_kbd.c}`; `main.c:741` inits PXI at `GrandCentralStart + 0x16000`). The NT HAL and drivers are **not in the repo at all** — `boot_files/boot.img` is a 256 KB HFS volume holding the ARC loader (`stage1.elf`, `stage2.elf`, `System/BootX`), and this fork publishes no releases (checked 2026-09-06). | Two things we want. The ADB transport, as source, for our exact chipset — the wrong side of the boot, but portable into a HAL. And `inc/halpxi.h`, which publishes the HAL↔driver ADB contract the (binary) HID driver uses. |
| [Wack0/maciNTosh](https://github.com/Wack0/maciNTosh) | Parent project: Gossamer/Grackle and Mac99 machines | firmware and loader only; README: "NT HAL and drivers have no source present for now" | ARC firmware design; the authors. Rairii posts in the ANS thread (post 49404). |
| The seven HALs on the NT 4.0 CD | `HALCARO` (Carolina), `HALEAGLE`, `HALFIRE` (FirePower), `HALPPC`, `HALPS` (PowerStack), `HALVICT` (Victory), `HALWOOD` (Woodfield) | binaries | Reference for the export set and for PReP conventions; picking one in the menu is the first probe (§9). |
| QEMU's IBM 40p (PReP) work, H. Poussineau 2015–17 | NT "started up to the point where it wanted to change endianness" | — | Precedent for the LE switch; nothing to reuse. |
| BetaWiki, *Windows NT on Power Macintosh* | history | — | unverified (HTTP 403 to fetches) |

Adjacent, and worth having open while writing interrupt code: Linux's `pmac_pic`/Grand Central
and `bandit` support, NetBSD/macppc, and MkLinux (the TNT dossier's `_sources/` has a driver
corpus; the MkLinux dossier at the MkLinux research notes
records how a Unix kernel was brought up on this emulator's PPC machines).

---

## 6. Toolchain

**What this project actually uses (2026-09-06): stock clang + lld and one Python script.**
`clang --target=powerpcle-unknown-linux-gnu -mcpu=604` compiles C and assembly to little-endian
PowerPC ELF objects; `ld.lld -T hal.ld --emit-relocs` links them at the image base
with the relocations kept; [`tools/elf2pe.py`](../tools/elf2pe.py) turns that ELF into an NT
PowerPC PE (machine `0x1F0`): function descriptors for the exports, an import directory
naming `ntoskrnl.exe`, and base relocations mapped one-to-one from the ELF kinds
(`ADDR32`→HIGHLOW, `ADDR16_HA`→HIGHADJ, `ADDR16_LO`→LOW). The NT PowerPC ABI (calls through
`{entry, toc}` descriptors, the caller frame slots a callee may write) is confined to
[`src/thunk.S`](../src/thunk.S) and the generated stubs of
[`tools/mkstubs.py`](../tools/mkstubs.py); the rest is plain SVR4 C. `make` at the repository root
produces `build/hal.dll`; no Microsoft tool and no DDK library is involved. The DDK
*contract* (structure layouts, IRQL and vector numbers) is restated in
[`include/nt.h`](../include/nt.h) with static asserts.

The route entii-for-workcubes documents (peppc + Microsoft's PASM/linker under Wine + the
DDK's PowerPC libraries) remains the reference for building *drivers* that need the DDK
libraries; the HAL does not:

1. **peppc** — build from <https://github.com/Wack0/peppc>; produces PowerPC little-endian
   PE objects.
2. **The Microsoft PowerPC pieces** the entii repo ships in `msvc-ppc/`: the VC6 PowerPC CE
   `cl` (used only as the assembler's preprocessor), `PASM.EXE` (Microsoft's PowerPC assembler,
   with its single-branch patch), the MSVC 4.2 linker and resource compiler, `SPLITSYM.EXE`
   from the NT 3.51 DDK.
3. **NT 4.0 DDK and SDK for PowerPC**: the `powerpc` libraries and headers, laid out as entii's
   `nt4/sdk`, `nt4/ddk`, `nt4/crt` (VC++ 4.0 CRT headers) and `nt4/hal` (headers with
   compatibility fixes). Sources: MSDN DDK CDs of 1996–97 (archive.org has them); confirm the
   exact set against entii's directories.
4. A Linux host with `make`, Python 3, and the Granny Smith headless emulator for tests.

**First milestone for the toolchain: build `halartx` unchanged.** If the GameCube HAL links,
the compiler, libraries and `.def` handling are right before a single ANS line exists.

Suggested layout for this repository once code starts (mirrors entii, which future
contributors will recognise):

```
powermac-nt-hal/
├── README.md              what the project is, where it stands, how to build it
├── STORY.md               every wall and how it fell — the narrative
├── CONTRIBUTING.md        the provenance rule, the clean-room rule, what never gets committed
├── PROVENANCE.md          every borrowed line, its origin and its licence
├── LICENSE                GPL-2.0, which follows from PROVENANCE.md
├── Makefile               `make` -> build/hal.dll, with clang + lld + tools/elf2pe.py
├── hal.exports            the 69 names a shipped NT PowerPC HAL exports
├── hal.imports            the 26 ntoskrnl routines this HAL calls
├── hal.ld                 link script: image base, section order
├── src/                   the HAL: init, ints, irql, clock, pci, dma, disk, misc, arc, cuda, display, vga, thunk.S
├── include/               nt.h (the kernel contract, layouts pinned with _Static_assert), hal.h, ans.h
├── build/                 output, gitignored
├── tools/                 elf2pe, mkstubs, patch-iso, mkarcdisk, the emulator client and the test driver
├── docs/                  this charter and the session write-ups
└── traces/                emulator captures that back the docs (console text and screenshots)
```

---

## 7. Plan

**Phase 0 — probes that need no code (all from the checkpoint).** Results in
[docs/2026-09-05-phase0-probes.md](2026-09-05-phase0-probes.md).
- ~~Keyboard input~~ done: one byte per `machine.scc.a.receive`, `0x9B 'A'` = Up, `\r` = Enter.
- ~~*MOTOROLA PowerStack*~~ done: accepted; the wall was the veneer's `OFClose` leak (fixed by
  two words); through the mass-storage and video menus the **kernel starts** and bugchecks in
  the HAL's PCI config probe (docs note, addenda 3–4).
- ~~*Other*~~ done: wants a floppy that the veneer's tree does not have (no
  `FloppyDiskPeripheral`). Dead end unless the veneer is taught SWIM3. The **preinstall route**
  (`\PPC\winnt.sif` + `$OEM$\TEXTMODE\txtsetup.oem`) needs no floppy and no `TXTSETUP.SIF`
  edit — preferred delivery.
- Patch the veneer's SCSI identifier (`convert_SCSI_device` maps `model` `NCR,53C810` to
  `NCRC810`; ours is `NCR,825A` → `UNKNOWN SCSI`) and see whether Setup loads `symc810`
  (it matches on the ARC Identifier and takes registers from the ARC configuration data; it
  does not scan PCI). **Done for the selection half**: nine byte pokes make Setup announce
  *Symbios Logic C810 PCI SCSI Host Adapter* (docs note, addendum 2). Whether the driver runs
  waits for a HAL.
- ~~Extract the maciNTosh-bandit HAL from `boot.img`~~ — there is no HAL in `boot.img`; it is the ARC loader's HFS boot volume. The NT binaries are release assets of the *parent* repo: `nt_arcfw_grackle_0.08.zip` → `drivers.img` → `halgoss.dll`, `usbadb.sys`, `atapimio.sys`, `offrmbuf.sys`, `TXTSETUP.OEM`.

**Phase 1 — the HAL.** Fork `halartx`; replace EXI/PXI/VI with: Grand Central interrupt
dispatch on the ANS EXT map; Bandit config-space access for both bridges (with the endian bit
respected); decrementer clock and stall; Cuda RTC and reset; ESCC kernel debugger; console via
ttya first (the veneer's `CONSOLEOUT` is the serial line) and 54M30 later. Test each entry with
emulator breakpoints; use `debug.logpoints` on GC/Bandit registers to see what the HAL touches.
Deliver via a patched `TXTSETUP.SIF` (§4). Done when the kernel starts and prints or stops
somewhere documented.

**Phase 2 — Setup needs input and disks.** *Storage and video done (2026-09-06 evening,
[docs/2026-09-06-boot-device-and-video.md](2026-09-06-boot-device-and-video.md)).*

- **SCSI: done.** The CD's stock `symc810` miniport drives the 53C825A. Four HAL bugs had to
  go, in this order: an unimplemented `HalAssignSlotResources`; a mis-packed
  `CM_PARTIAL_RESOURCE_DESCRIPTOR`; a stub `HalAllocateAdapterChannel`, which made
  `ScsiPortStartIo` fail every request before the miniport was ever asked to do anything (that,
  and not the interrupt path, was why no interrupt arrived — the chip had nothing to do); and
  `IoMapTransfer` declared as returning its `PHYSICAL_ADDRESS` in a register pair when this ABI
  returns eight-byte values through a hidden pointer in `r3`. The bus is now scanned, the
  CD-ROM answers, and the interrupt arrives. No purpose-built driver was needed.
- **Boot device: done**, once a third veneer patch stopped `VrOpen` turning
  `…fdisk(0)partition(0)` into a filesystem open of the CD's root directory (§3 of the doc).
- **Video: working, with one bypass.** `cirrus.sys` identifies and initialises the 54M30 and
  Setup draws on the monitor, after the emulated part gained the four registers a driver
  identifies it by (`SR06`, `CR27`, `SR15`, `$3CC`). `VideoPortVerifyAccessRanges` still
  reports a conflict for the driver's legacy `0xA0000` VGA aperture — which on this machine is
  RAM — and is currently bypassed by a diagnostic poke. That is the one open item here.
- **Keyboard: done** (2026-09-06 evening,
  [docs/2026-09-06-adb-keyboard.md](2026-09-06-adb-keyboard.md)). `source/cuda.c` gives the
  HAL a Cuda transport over Grand Central's VIA at `+0x16000` and the three ADB entry points
  maciNTosh's `usbadb.sys` imports. With that driver dropped over `\PPC\I8042PRT.SYS` (the name
  SETUPLDR loads outright), the ADB bus is scanned through the HAL — keyboard at address 2,
  mouse at 3 — auto-poll is enabled, and a real keystroke arrives at the driver's callback:
  `status 40 cmd 2c len 2 [00 ff]`, Cuda's auto-poll flag, address 2 / Talk / register 0, key
  `0x00` down. *"Setup did not find a keyboard"* is gone and Setup launches `usetup.exe`.
  Remaining: `usetup` cannot load the layout DLL (below).

  The two routes as surveyed, for the record:

  - **Their driver, our HAL.** maciNTosh's `usbadb.sys` ("PowerMac General HID & Storage",
    from the parent repo's release `drivers.img`) is a PowerPC LE PE that imports **six** names
    from `HAL.dll` — `KeRaiseIrql`, `KeLowerIrql`, `KeStallExecutionProcessor`, which we
    already export, and three private ones: `HalPxiCommandAdb`, `HalPxiAdbSetCallback`,
    `HalPxiAdbAutopoll`. It connects no interrupt and knows no chipset: all the Cuda work is on
    the HAL side, which makes the driver chipset-independent and the contract three functions
    wide. They are declared in maciNTosh-bandit's [`inc/halpxi.h`](https://github.com/MCJack123/maciNTosh-bandit/blob/main/inc/halpxi.h).
    `TXTSETUP.OEM` registers it under `[scsi]`, which is how it loads at text-setup time.
    Run locally only — see the license note in §13.
  - **All source.** Port `arcbandit/source/{pxi.c,adb_bus.c,adb_kbd.c}` (Cuda/PMU on Grand
    Central at `+0x16000`; the ADB files carry Open Hack'Ware/OpenBIOS copyright, GPL-2.0) into
    the HAL, and write the port driver from `entii-for-workcubes/fpsidrv/source/` — a complete
    NT 4.0 PowerPC keyboard+mouse port driver with source (`IOCTL_INTERNAL_KEYBOARD_CONNECT`,
    `KEYBOARD_INPUT_DATA`, the `ClassService` callback), reading GameCube controllers instead
    of ADB but otherwise exactly the scaffolding needed.

  Route one is what runs today. Route two is still the one to finish with, so that nothing
  published depends on a binary this project may not redistribute; the HAL half of it is
  already done, and only the port driver is left.
- **Setup's keyboard layout (solved).** `usetup.exe`'s *"could not load the keyboard layout
  file KBDUS.DLL"* was a rooted DOS path with no drive to root it against: `IoAssignDriveLetters`
  is a HAL export and ours was a stub, so the machine had no drive letters at all. Implementing
  it took Setup to "Welcome to Setup" and on through the licence and hardware screens.
- **The partition table and the ARC environment (solved).** `IoReadPartitionTable`,
  `IoWritePartitionTable`, `IoSetPartitionInformation` and `HalGetEnvironmentVariable` are HAL
  exports too, and all four were stubs. With them implemented — and with the FAT system
  partition `ARCINST.EXE` would have created, written by `tools/mkarcdisk.py` — Setup reaches
  its partition screen and offers to install.
- **Drive letters: the current wall.** `IoAssignDriveLetters` assigns one letter per disk
  (`\Device\Harddisk%d\Partition1`) instead of one per recognised partition, in NT's order:
  the first primary of each disk, then logical drives, then the remaining primaries, then the
  CD-ROMs. Setup numbers the volumes that way itself, so the partition it installs to has no
  symbolic link and the kernel ends up calling `_wcsicmp` on a `UNICODE_STRING`'s `Length`.
  The fix needs `IoGetDeviceObjectPointer` to open each `\Device\Harddisk%d\Partition0` and
  the partition table this HAL already reads.
- **Disk size (solved).** NT wants at least 158 MB and the first emulated disk was 8 MB. Not a
  HAL problem: a larger disk needs a new checkpoint chain, because the checkpoint is
  consolidated and its block count is fixed.

**Phase 3 — real hardware.** The thread's ANS 700 owner has serial console access. Two things
the emulator does not reproduce and the hardware will: the DSI at `0x50014` when the veneer
first touches `.data` after the LE reboot (post 49391), and Cuda/ESCC timing. The two-byte
veneer recipe of §3 has not yet been tried on hardware.

---

## 8. Facts already established that the HAL work should not re-derive

- The veneer's ARC tree, environment and memory descriptors on this machine (§2, trace).
- The veneer has `VrDebug` at image `0x60C08` and a full symbol table; SETUPLDR likewise. Both
  binaries embed their Microsoft source paths (`D:\nt\private\ntos\boot\veneer\vr*.c`,
  `D:\nt\private\ntos\boot\setup\setup.c`).
- `BlInitResources` reads 1 KB of its own image, checks machine `0x1F0`, and locates `.rsrc`
  via the optional header — SETUPLDR is a bare COFF image with ImageBase `0x80600000`,
  entry `0x80641aac`, TOC `0x8064b52c`.
- SETUPLDR's filesystem recognizers read `0x62 @ 0`, `512 @ 0x2000`, `528 @ 0`, `2048 @ 0x8000`;
  CDFS then reads the PVD, root directory and `\PPC` (LBA 196–249) and files by extent.
- The ISO-9660 "16 KB" firmware quirk (deblocker left at the absolute extent) is real and
  irrelevant: the veneer seeks before every read, SETUPLDR reads raw sectors.
- The emulator's `ans500` profile with 64 MB, 2.26NT ROM, CD on `/bandit/53c825@11/sd@0,0`
  is the reference configuration; the transcript's machine was a 700 with 64 MB.
- Full narrative and evidence: ONBOARDING §2, §13, §14.

---

## 9. Open questions (in the order they block)

1. ~~Keyboard input to SETUPLDR~~ **Works** (one byte per call; docs note §1).
2. ~~Does the `symc810` miniport on the CD drive a 53C825A?~~ **Yes.** It is chosen by ARC
   Identifier (`NCRC8*`) and gets its registers from the ARC configuration data, not from PCI
   IDs; its chip-type switch accepts the 825A. Setup names it *"Symbios Logic C810 PCI SCSI
   Host Adapter"*, the bus is scanned, the CD boots and disks read and write through it.
3. **Will the maciNTosh authors release the HAL/driver source**, or accept an ANS variant?
4. ~~Where does each shipped HAL fault~~ **PowerStack (`HALEAGLE`) measured** (docs note,
   addendum 3–4): the kernel starts and runs its phase 0 init; the HAL's PCI
   configuration-space probe at PReP `0x80800000` takes an alignment exception →
   bugcheck `0x1E`; the bugcheck's display output then faults at ISA `0xB8000`. The other
   six HALs can be measured the same way from `tmp/nt-after-massstorage.ckpt`-style
   checkpoints if useful; the requirement list already has its first two lines.
5. ~~NT 4 DDK PowerPC availability~~ **Moot.** The question was which archive.org item carries
   the `powerpc` import libraries. This HAL is built with clang, lld and `tools/elf2pe.py` and
   needs no DDK at all, which is also why the build has no proprietary dependency to document.
6. **What the veneer needs patched** for a clean root `Identifier` (`convert_name`, `0x565c8`).
7. **The real hardware's `0x50014` DSI** — retained memory/MMU state after the LE reboot; any
   HAL test on hardware will meet it first.
8. MP (the 700 can carry two 604e cards; the doorbell is a GBUS access) — phase 4 at the earliest.

---

## 10. Conventions for this repository

- Every claim names its source: a thread post id, an Apple document and page, a chip manual
  section, or a trace file in `traces/`. Keep it up.
- Record dead ends as well as results — three of the four theories held on the morning of
  2026-09-05 were wrong by that evening, and `STORY.md` says so.
- Write emulator runs to a log file before grepping anything.
- No Microsoft or Apple binaries in git; scripts that *patch* a user-supplied CD are fine.
  `CONTRIBUTING.md` has the full list and the provenance rule.

---

## 11. People and channels

- TinkerDifferent thread *Apple Network Server MacOS-based ROMs found* — the hardware owners
  (Mr. Macintosh: the ANS 700 with the serial console and the transcript, post 49785;
  ClassicHasClass: the ROM dumps and the blog write-ups; joevt: ROM analysis and the
  detokenized firmware; Rairii/Wack0: maciNTosh, entii, peppc). Side thread
  *The Granny Smith emulator* for emulator matters.
- Granny Smith: <https://github.com/pappadf/granny-smith>, PR #135 (little-endian mode; its
  description carries the full NT story and the screen).
- ClassicHasClass's write-ups on NT for PowerPC: <https://oldvcr.blogspot.com/>.

---

## 12. Working model — two projects, one direction of dependency

| | [Granny Smith](https://github.com/pappadf/granny-smith) (the emulator) | This project |
|---|---|---|
| What goes there | emulator fixes the HAL work uncovers; device-register logpoints and traces that become integration tests; the machine write-ups | the HAL source, its build, the media-patching and test tooling, the design notes, the provenance log, this charter |
| Depends on | nothing here | **nothing there.** It *uses* the emulator as a test rig, the way it would use a real machine |

Practical rules that follow:

- **Anything this project needs from the emulator is a feature request on the emulator**, not a
  copy. If a HAL test needs a new shell command, a device logpoint or a checkpoint behaviour,
  add it to Granny Smith with its own test, and describe here how the emulator is *used*, with
  the version. The `tools/` here are glue that talks to the emulator's TCP shell; they must work
  against an unmodified upstream release.
- **Facts move, files do not.** A register map from an Apple document, a trace of the veneer's
  ARC tree, an interrupt table — restate them in `docs/` with the public citation (document
  title and section, thread post id, chip manual). Never copy the documents themselves.
- **Every relative link in the repository resolves.** The check:

```bash
python3 - <<'EOF'
import re, os, urllib.parse, glob, sys
bad = 0
for f in glob.glob('**/*.md', recursive=True):
    for m in re.finditer(r'\]\(([^)]+)\)', open(f).read()):
        t = m.group(1)
        if t.startswith(('http', '#', 'mailto:')):
            continue
        p = urllib.parse.unquote(t.split('#')[0])
        if p and not os.path.exists(os.path.join(os.path.dirname(f), p)):
            print('MISSING', f, '->', p); bad = 1
sys.exit(bad)
EOF
```

---

## 13. Provenance and licensing

The final license of this project is not a choice made up front; it is **determined by what
the code contains**. Every upstream candidate is copyleft, and Microsoft's own headers are
proprietary, so the log has to be kept from the first commit.

**Upstream licenses, checked 2026-09-05** (`gh api repos/<owner>/<repo>/license`):

| Project | License | What we might take | Consequence |
|---|---|---|---|
| Wack0/entii-for-workcubes | **GPL-2.0** | `halartx/` HAL structure and code (the intended template), `msvc-ppc/` tool bits, `nt4/` headers, `pe_rules`, Makefiles | anything derived from `halartx` makes this project **GPL-2.0** (or GPL-2.0-or-later only if we never touch the GPL-2.0-only parts); source must be published with binaries |
| Wack0/peppc | **GPL-2.0** (a GCC fork) | the compiler | a *tool*; using it does not license our output (GCC runtime exception applies to what `libgcc` contributes; note if any `libgcc` for `powerpcle` is linked in) |
| MCJack123/maciNTosh-bandit, Wack0/maciNTosh | **GPL-2.0** | ARC firmware/loader **source** (including Bandit Cuda/ADB, which we do want); the NT HAL and drivers are **binaries in release assets** (`drivers.img`), with no source published | *Using* those binaries locally is unrestricted — the GPL governs distribution, not use. *Redistributing* one is what we cannot do: it is GPL-2.0 with no published source, so we could not accompany it with the corresponding source or a written offer (GPL-2.0 §3) and would be distributing in violation. Source we read or port enters the log like `halartx`. |
| Microsoft NT 4.0 DDK/SDK | proprietary (DDK EULA permits building drivers with it; redistributing headers/libs is not permitted) | headers and `powerpc` import libraries | **never commit**; the build must fetch or expect a user-supplied DDK. entii's `nt4/` directory and its 378 KB `nthal.h` are Microsoft-derived — do not copy them into this repository; point at where they come from |
| Microsoft NT 4.0 PowerPC CD | proprietary | `TXTSETUP.SIF` layout, HAL export tables (read, not copied) | scripts may *patch* a user's CD; no CD content in git |
| Apple, Symbios/LSI, Motorola documents | proprietary PDFs | register-level facts | facts and citations only, no text or figures |
| Windows NT source leaks | — | **not used and not to be used.** Everything here was derived from the shipped binaries' own symbol tables and behaviour (the veneer's and SETUPLDR's COFF symbols, `VrDebug`, breakpoints). Keep it that way; a clean-room note per source file is part of the log |

**The log: [`PROVENANCE.md`](../PROVENANCE.md).** One row per file or function that
is not original, appended *at the time of copying*, never reconstructed later:

```
| path in this repo | origin (repo, path, commit) | license | how much (verbatim / adapted / rewritten from reading) | date | who |
```

Every file carries `SPDX-License-Identifier: GPL-2.0-only` and
`Copyright (C) 2026 powermac-nt-hal contributors`. A file with a row in the log *also* carries a
header block naming the upstream project, the commit read, its license and the extent — the
upstreams keep their notices in their repository-level `COPYING` rather than per file, so
there is no upstream copyright line to carry, and naming the project is what discharges the
obligation. [`NOTICE`](../NOTICE) collects all of it in one place. A pull request that adds
code without a provenance row is not merged.

**Working assumption for planning:** the HAL will be **GPL-2.0-only**, because the template is.
If the project later wants a permissive license, the HAL must be written from the public
interface (the DDK's documented `Hal*` contract, the shipped HALs' export tables, Apple's
register documents) without deriving from `halartx`; that is a real option — the interface
is small and fully documented in the DDK — and the decision should be made **before** the
first `halartx` file is copied, because it cannot be undone afterwards.
