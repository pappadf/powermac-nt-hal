# Every wall, and how it fell

*A running account of getting Windows NT 4.0 to boot on an Apple Network Server 500 — one
obstacle at a time, what each one turned out to be, and what it cost to get past. Written as a
narrative; the reference documents in [`docs/`](docs/) carry the register-level detail and
[`docs/CHARTER.md`](docs/CHARTER.md) carries the charter.*

**The machine.** An Apple Network Server 500 ("Shiner"): a Power Macintosh 9500 logic board with
server extras. PowerPC 604 at 132 MHz, two Bandit PCI host bridges, a Grand Central I/O chip,
two Symbios 53C825A SCSI controllers, a Cirrus Logic 54M30 PCI video card, an ADB keyboard
behind a Cuda microcontroller, and Apple's Open Firmware 2.26NT. Apple never supported NT on
it; Microsoft's PowerPC NT only ever ran on IBM and Motorola PReP machines. Everything happens
inside the Granny Smith emulator, which models this board.

**The goal.** Get NT 4.0's text-mode Setup to run. That needs a HAL — `HAL.DLL` — written from
scratch, because none of the seven HALs on the CD knows this hardware.

**How to read this.** Walls are numbered in the order they bit. Each one gets the symptom as
the machine reported it, what it actually was, how that was established, the fix, and what the
fix bought. A few of the fixes are *workarounds* rather than fixes; they are marked, and
collected honestly at the end.

---

## Part 0 — The starting line

Before any HAL work, the emulator itself had to be able to run this software at all. Two things
were already done and are worth naming because everything below stands on them:

- **Little-endian mode on the 604.** NT PowerPC runs the processor little-endian, which on this
  architecture is not a byte-order switch but an *address munge*: a byte access at address `A`
  goes to `A ^ 7`, a halfword to `A ^ 6`, a word to `A ^ 4`. This is the wall that has stopped
  PowerPC NT emulation for a decade — QEMU's IBM 40p work hit exactly the same one in 2015.
- **Bandit byte-lane reversal.** The PCI bridge reverses its eight byte lanes when the
  big-endian bit is clear, which *cancels* the CPU's munge for device registers. The two
  together are why a plain load or store of natural size reaches a device correctly. Getting one
  without the other produces garbage that looks like a broken device model.

With those, Open Firmware boots, the machine reboots little-endian, Microsoft's `VENEER.EXE`
(FirmWorks, 1996 — an Open Firmware → ARC firmware shim) runs, and `SETUPLDR` loads and shows
*Windows NT Setup*.

---

## Part 1 — Getting Setup to ask for a HAL

### Wall 1 — Two `claim` failures in the veneer

**Symptom.** The veneer exits back to Open Firmware early in its startup.

**What it was.** Two memory `claim` calls fail on this ROM.

**Fix.** Two `nop`s, from the TinkerDifferent thread that first got this far on real hardware.
*(Inherited, not ours.)*

**Bought.** `Open Firmware ARC Interface Version 3.0 (Jul 12 1996)` and a boot.

### Wall 2 — The boot path names nothing

**Symptom.** The veneer tries to boot `device-tree(0)` and returns ENODEV.

**What it was.** `/chosen bootpath` has to name the CD, or the veneer's ARC path conversion has
nothing to convert.

**Fix.** Set it in Open Firmware before `go`:
`" /bandit/53c825@11/sd@0,0" encode-string " bootpath" _chosen (property)`.

**Bought.** `Booting from 'multi(0)scsi(0)cdrom(0)fdisk(0)\PPC\SETUPLDR'`.

### Wall 3 — `partition(1)` appended to every boot path

**Symptom.** SETUPLDR rejects the boot device.

**What it was.** SETUPLDR's `BlGenerateDeviceNames` refuses any component after
`cdrom(N)fdisk(N)`, and the veneer appends `partition(1)` to every path it builds.

**Fix.** Blank the `"partition(1)"` string in the veneer: one byte at image `0x5D0C0`.

### Wall 4 — Apple's `disk-label` wants an *empty* argument for raw sectors

**Symptom.** Raw sector reads of the CD come back as filesystem data.

**What it was.** Apple's Open Firmware `disk-label` package gives raw sector access only when
the OF path has no argument after the colon. The veneer's `VrOpen` appends `:0`.

**Fix.** Blank the `":0"` string: one byte at `0x5E168`.

**Bought.** SETUPLDR mounts the CD with its own CDFS, parses `TXTSETUP.SIF`, loads
`NTKRNLMP.EXE`, and puts up **"Setup could not determine the type of computer you have"** — the
HAL menu. That screen is one further than the community had reached on real hardware, and it is
where a HAL project starts.

### Wall 5 — Every `OFClose` leaks an Open Firmware instance

**Symptom.** Choose *MOTOROLA PowerStack*; the HAL, configuration data, font and NLS files load;
then Setup re-opens the bare CD device to check the media and the open fails with
`OFOpen(...) IHandle: 0`.

**What it was.** Not the HAL — the veneer. Its `OFClose` has an `nret` bug (it tells Open
Firmware the wrong number of return values), so every close fails silently. Each file SETUPLDR
opens leaks an OF instance, and 2.26NT stops opening the CD after nine live instances.

**How we found it.** Reading the veneer's disassembly against its own symbol table, and counting
opens against the failure point.

**Fix.** Two words poked into the veneer at `0x52250` and `0x5225c`.

**Bought.** Setup's mass-storage screen, the video-adapter menu, and — with a shipped HAL —
SETUPLDR *entering the kernel*. The shipped PowerStack HAL then dies exactly as you would hope:
it probes PCI configuration space through the PReP window at `0x80800000`, which on this machine
is Bandit PCI **memory** space, takes a master abort → machine check, retries with a bad address,
takes an alignment exception, bugchecks `0x1E`, and its `HalDisplayString` then faults on the
VGA text buffer at `0xB8000` that this machine does not have. Two requirements measured in one
crash: **PCI config through Bandit's ports, and a console that exists.**

---

## Part 2 — A PE the NT PowerPC loader will accept

Now the HAL itself. Built with clang + lld + a home-grown `tools/elf2pe.py` — no Microsoft
tools. Five things about the NT PowerPC PE format had to be exactly right, and each announced
itself differently.

### Wall 6 — Import slots stay zero

**Symptom.** Bugcheck `0x1E` with `STATUS_BREAKPOINT` on the first call into the kernel.

**What it was.** SETUPLDR's `BlpScanImportAddressTable` walks `FirstThunk` and **never consults
`OriginalFirstThunk`**. A conventional PE leaves the IAT zeroed and the ILT populated; this
loader needs the IAT itself pre-filled with the hint/name RVAs.

**Fix.** `elf2pe.py` writes the name RVAs into the IAT.

### Wall 7 — `DEFAULT CATCH!, code=FFF00600`

**Symptom.** An alignment exception in the firmware while *Setup is loading files*.

**What it was.** The binder reads each import's hint with `lhz`, and in little-endian mode a
misaligned halfword is an alignment fault. Hint/name entries must be 2-byte aligned.

### Wall 8 — `DEFAULT CATCH!, code=FFF00300`

**Symptom.** A data-storage fault at the same instruction.

**What it was.** The IAT needs its terminating zero word *inside the section*. Without it the
loader read the emulator's RAM fill pattern as another hint address.

### Wall 9 — 256 MB of device space and only three BAT slots

**Symptom.** `KePhase0MapIo` returns NULL for the HAL's I/O window.

**What it was.** The kernel hands out at most three 8 MB BAT slots. This machine's devices span
`0xF2000000`–`0xF5FFFFFF`.

**Fix.** The HAL programs DBAT3 itself: 256 MB at `0xB0000000` → `0xF0000000`, cache-inhibited
and guarded. A BAT never faults, so device access works at any IRQL — which matters later.

### Wall 10 — `mfspr 268` is illegal on a 604

**Symptom.** An illegal-instruction trap in the HAL's timebase read.

**What it was.** LLVM emits `mfspr 268` for the timebase. The 604 wants the `mftb` encoding
(opcode 31, XO 371).

**Fix.** Hand-encoded `.long` instructions in `thunk.S`.

---

## Part 3 — The ABI the kernel actually speaks

### Wall 11 — Bugcheck `0x1E` inside `HalTranslateBusAddress`

**Symptom.** The "AddressSpace pointer" argument was `0x00000004`.

**What it was.** `LARGE_INTEGER` / `PHYSICAL_ADDRESS` are **passed by value in a register
pair**, low word first, aligned to an odd register. The SVR4 ABI our compiler follows passes a
*union* by pointer, so every argument after the address was shifted by one.

**Fix.** Declare them as 64-bit scalars in `nt.h`, with the reasoning written next to the
typedef.

### Wall 12 — The same types come *back* the other way round

**Symptom.** Much later: bugcheck `0x0A` at `IoMapTransfer+4`, reading address `0x14` — a null
MDL.

**What it was.** The opposite rule for *returns*. Microsoft's PowerPC compiler returns every
struct, eight bytes included, through a **hidden first argument in `r3`**; the real arguments
start at `r4`. The caller in `SCSIPORT.SYS` proves it — it passes a stack slot in `r3` and reads
the result out of it afterwards — and `MmGetPhysicalAddress` proves it from the other side: its
first instruction stores its `VirtualAddress` argument from `r4`.

**Fix.** `IoMapTransfer` and `KeQueryPerformanceCounter` take the out-pointer first and return
it; the call to `MmGetPhysicalAddress` passes one. `KeQueryPerformanceCounter` had been silently
returning the *frequency* as the counter for the whole project's life.

**Lesson.** Two opposite conventions for the same type, one for arguments and one for returns.
Both rules now sit in one comment in `nt.h`, because either alone is a trap.

### Wall 13 — Frames the kernel scribbles on

**What it was.** An NT callee may write the caller's frame at offsets 4, 8 and 24–55 (glue save
slots and the parameter home area). Our SVR4-compiled code never reserves those.

**Fix.** Every call into the kernel goes through `hal_import_call` in `thunk.S`, which builds a
64-byte frame the caller never looks at.

**Bought (walls 6–13).** Build 7 of `halshinr` — about 900 lines — is loaded by SETUPLDR, bound
to `ntoskrnl.exe`, and **the kernel runs on it**:

```
HAL: halshinr build 7 for the Apple Network Server (phase 0)
Microsoft (R) Windows NT (TM) Version 4.0 (Build 1381).
1 System Processor [64 MB Memory] MultiProcessor Kernel
```

Those 900 lines are not 900 lines of our insight. The contract between the NT kernel and a
PowerPC HAL is barely documented — the DDK names the functions and says almost nothing about
the order, the ownership or the invariants — and the reason walls 6 to 13 took days rather than
months is **Wack0's [`entii-for-workcubes`](https://github.com/Wack0/entii-for-workcubes)**,
whose `halartx` is a HAL that works. Reading it is where the shape of everything above came
from: that the HAL owns the IRQL and keeps it in the PCR; what phase 0 must do before phase 1
exists; that external interrupts are dispatched through the PCR's own routine table; and —
worth as much as any of it — *which routines may be stubs*, which is the difference between a
HAL that boots and a HAL that stops with no diagnostic at all. Walls 20 and 22, later on, are
what it looks like to guess at a contract with no such guide. Every line here was written
fresh, and nine of them came out identical to theirs anyway, because there is only one correct
way to check a PRCB version or seed a time increment.

The decrementer clock, IRQL masking, the descriptor thunks and the ttya console all worked on
the first run that got this far.

---

## Part 4 — Surviving driver initialisation

### Wall 14 — A PCMCIA driver probes ISA port `0x3E0` and kills the machine

**Symptom.** `pc = 0x200`, MSR `0x10001` — an unrecoverable machine check.

**What it was.** `pcmcia.sys` asked `HalTranslateBusAddress(Isa, 0x3E0)`. Translating that into
Bandit's PCI I/O window makes a master abort, which on this hardware raises TEA → machine check.
There is no ISA bus here.

**Fix, in two parts.** The HAL refuses to translate legacy I/O ports (except VGA's
`0x3B0`–`0x3DF`, which the Cirrus really does decode) and legacy memory below `0x80000000`.
And a **machine-check handler** completes an aborted load with all-ones and steps over it, so a
probe that slips through behaves the way it would on PReP hardware. Only volatile target
registers can be fixed up; anything else still bugchecks, loudly and on purpose.

### Wall 15 — `i8042prt.sys` dereferences NULL

**Symptom.** Bugcheck `0x1E` at `i8042prt.sys+0x1D28`.

**What it was.** The veneer publishes an i8042 `KeyboardController` in the ARC tree (it maps ADB
onto the PC keyboard controller it expects NT to want). The driver loads, cannot translate its
ports — correctly, per wall 14 — and dereferences NULL+0xF8.

**Workaround at the time.** Keep it out of the load list with a same-length edit to
`TXTSETUP.SIF` (`i8042prt.sys,4` → `kbdclass.sys,4`). *(Superseded by wall 27, which puts a real
driver in that slot.)*

### Wall 16 — The 53C825As are `UNKNOWN SCSI`

**Symptom.** Setup's mass-storage screen shows `<none>`, so no SCSI driver loads at all.

**What it was.** `SYMC810.SYS` — the PowerPC name for the NCR/Symbios 53C8xx family miniport —
imports no PCI access at all and takes its registers from the ARC `ScsiAdapter` configuration
data. Setup picks it by ARC *Identifier* (`[Map.SCSI] symc810 = *NCRC8`). The veneer's
`convert_SCSI_device` maps `NCR,53C810` to `NCRC810`; ours reports `NCR,825A` and falls through
to `UNKNOWN SCSI`.

**Workaround.** Eleven bytes poked into the veneer's model string at `0x5F420`. Setup then
announces *Symbios Logic C810 PCI SCSI Host Adapter*.

### Wall 17 — The ARC tree's resource lists are empty

**What it was.** The veneer's `ScsiAdapter` nodes carry a port range of `0x00000000+0x100` and
IRQ level 1 — placeholders. A miniport that takes its registers from there gets nothing.

**Fix.** `HalpFixConfigTree` rebuilds the `ScsiAdapter` resource lists in phase 0 from real PCI
configuration space: `ScsiAdapter 0 → mem f3100000, io 0400, irq 22`.

---

## Part 5 — Storage: four bugs between NT and its own disk

`STOP 0x0000007B INACCESSIBLE_BOOT_DEVICE`, status `0xC0000034` (*object name not found*). This
one code stayed on the screen through four completely different faults.

### Wall 18 — `HalAssignSlotResources` was a stub

**Symptom.** `ScsiPortInitialize` returns `STATUS_DEVICE_DOES_NOT_EXIST` and never calls the
miniport's `HwFindAdapter` — a breakpoint on it, resolved from the `HW_INITIALIZATION_DATA`
descriptor, never fires.

**What it was.** For a PCI miniport, `scsiport` asks the HAL for the controller's resources.
Ours returned `STATUS_NOT_SUPPORTED`.

**Fix.** Implement it: size the BARs from config space, add the routed interrupt, return a
`CM_RESOURCE_LIST`.

### Wall 19 — `CM_PARTIAL_RESOURCE_DESCRIPTOR` was 24 bytes instead of 16

**Symptom.** Bugcheck `0x50 PAGE_FAULT_IN_NONPAGED_AREA`.

**What it was.** NT packs the CM structures to 4 bytes. An 8-byte `PHYSICAL_ADDRESS` member made
the compiler pad the descriptor to 24, so `scsiport` walked the list at the wrong stride and
dereferenced garbage.

**Fix.** `#pragma pack(4)` and a `_Static_assert` on the 16-byte size, so it can never drift
again.

**Bought.** `scsiport` calls `HwFindAdapter`; the miniport requests a bus-master scatter-gather
DMA adapter, gets its interrupt vector (GC bit 22 → vector 54), connects and enables it. No
bugcheck. And then… nothing. The machine idles.

### Wall 20 — "The interrupt that never arrives"

**Symptom.** The SCSI completion interrupt never fires. `HalpExternalInterrupt` is never
entered in a whole run.

**What it was — and this is the instructive one — *not an interrupt problem at all*.** Tracing
the IRP showed `ScsiPortStartIo` running exactly once and the miniport's `HwStartIo` never being
called. Disassembling `ScsiPortStartIo` explained why: a bus master that has a DMA adapter
object does not start I/O directly, it goes through `IoAllocateAdapterChannel` and is started
from the adapter-control callback. `IoAllocateAdapterChannel` in the kernel is a five-line
wrapper that forwards straight to the HAL's `HalAllocateAdapterChannel` — which was a stub
returning `STATUS_INSUFFICIENT_RESOURCES`. Every request failed before the chip was ever asked
to do anything, so of course it never interrupted. There was no interrupt to deliver.

**Fix.** Implement it. A PCI bus master on this machine addresses memory directly, so there is
nothing to hand out and nothing that can be contended for: every request is granted immediately
in the caller's context, calling the driver's execution routine through its descriptor. The
kernel's wrapper also handed us the exact `WAIT_CONTEXT_BLOCK` layout for free, since we could
watch which offsets it writes.

**Lesson.** "Nothing is happening" is not evidence about the thing you were looking at. The
interrupt path had been healthy the whole time.

### Wall 21 — see wall 12

The hidden-pointer return convention surfaced here, in `IoMapTransfer`, as the immediate next
failure. With walls 18–21 fixed the SCSI stack works end to end: the bus is scanned, `INQUIRY`
reaches every target and LUN, the CD-ROM answers, and `cdrom.sys` reads the ISO volume
descriptor. The bugcheck code did not change — but it was now a *different* `0x7B`.

### Wall 22 — The veneer reads the CD's root directory when asked for a raw sector

**Symptom.** Still `0x7B` / `0xC0000034`, now failing to open
`\ArcName\multi(0)scsi(0)cdrom(0)fdisk(0)`.

**What it was.** That symbolic link is created only if a check passes: for each `\Device\CdRom%d`
the kernel reads 2048 bytes at offset `0x8000` (ISO sector 16, the Primary Volume Descriptor),
sums the 512 words, and requires the sum plus the checksum *the loader recorded* to be zero. The
HAL can print the loader's side, because `LoaderBlock->ArcDiskInformation` is right there:

```
HAL: arc disk sig 00000000 sum d5d64a58 valid 0 'multi(0)scsi(0)cdrom(0)fdisk(0)'
```

The kernel computed `0x2d88048b` for that CD — exactly the sum of sector 16 of the ISO, which
incidentally proves the whole DMA path is byte-perfect. `0xd5d64a58` is not its negation, and a
brute-force scan showed **no** 2048-byte window anywhere in the 606 MB image negates to it. So
SETUPLDR checksummed something else.

**How we found it.** Breaking on `BlReadSignature` live:

| | requested | `ArcRead` returned | first bytes |
|---|---|---|---|
| the staging disk | 512 @ 0 | **512** | `"GS NT VENEER STAGING DISK v1…"` |
| the CD | 2048 @ 0x8000 | **428** | an ISO directory record for `HP35036.PC_` |

428 is `0x1AC`, and the ISO's root directory is extent 29, **size 428**. Seeking `0x8000` into a
thing based at LBA 29 lands on LBA 45, where those bytes live. The veneer had opened the CD's
*root directory as a file* instead of the raw device — the same `disk-label` fault as wall 4,
but through `VrOpen`'s **second** path, the one taken when an ARC `partition(N)` component is
present, which builds `":" + digits` of its own.

**Workaround.** One word: turn the branch at veneer image `0x54748` into a `nop`, so
`partition(N)` with no file path goes down the raw route and `partition(N)\path\file` is left
alone.

**Bought.** The checksum matches, `\ArcName\…` is created, and **INACCESSIBLE_BOOT_DEVICE is
gone.** NT has its boot device.

---

## Part 6 — Video

### Wall 23 — There is no console

Before any of the above could be *seen* on the monitor, `HalDisplayString` had to draw
somewhere. The firmware leaves the console on ttya and never programs the video chip.

**Fix.** A HAL framebuffer console: a from-scratch mode-set into 640×480 8bpp linear through the
legacy VGA and Cirrus extension registers, a two-entry palette (blue paper, white ink), and
glyphs blitted from the OEM font SETUPLDR hands over in `LOADER_PARAMETER_BLOCK.OemFontFile`.
The VGA ports are at Bandit 1's fixed PCI-I/O region; the framebuffer is BAR0, mapped in phase 1
because `KePhase0MapIo` hangs on that address.

**Bought.** The kernel banner and every bugcheck appear on the monitor, ~130 lines of code.

### Wall 24 — `cirrus.sys` cannot find a Cirrus

**Symptom.** *"Setup has encountered a fatal error while initializing your computer's video
(0, 0xc0000034)"* — `setupdd` could not open `\Device\Video0`, because `videoprt.sys` never
created it, because the miniport's `HwFindAdapter` failed.

**What it was.** Four registers on the emulated card that are not memory on a real one, found by
breaking on each of `HwFindAdapter`'s return sites in turn:

| | behaviour required | why the driver cares |
|---|---|---|
| `SR06` | write `$12` unlocks and reads back `$12`; anything else locks and reads back `$0F` | the round trip *is* the presence test |
| `CR27` | read-only `$A0` — CL-GD5430, agreeing with the `$00A0` PCI device ID | the chip ID; must fall in `[$0B,$2F]` |
| `SR15` bits 3:0 | `2` = 1 MB of fitted DRAM | the driver *sizes its mode list from this* and rejects everything larger — zero meant zero modes |
| `$3CC` | reads back what `$3C2` was given | bit 0 says whether the CRTC is at `$3D4/$3D5` or `$3B4/$3B5` |

Open Firmware and AIX arrive already knowing what the chip is, so a flat byte array per register
block had carried the emulator's model this far. A driver that has to *find* the part needs
more.

**Fix.** An emulator change, committed as `54m30: the four registers a driver identifies the
part by`. The monochrome CRTC pair is also folded onto the colour one, so software that never
writes `$3C2` still works. All seven ANS/TNT integration tests still pass — the goldens are
unchanged, because everything that already worked wrote `$3C2` and `SR06` the way real VGA code
does.

**Bought.** `cirrus.sys` initialises, `\Device\Video0` exists, and Setup switches its own text
UI onto the monitor — the grey status bar with black text is the giveaway that this is Setup
drawing through the video driver and not the HAL's two-colour console.

### Wall 25 — The legacy VGA aperture is RAM here *(still open)*

**Symptom.** `VideoPortVerifyAccessRanges` returns `ERROR_INVALID_PARAMETER`.

**What it was.** `IoReportResourceUsage` succeeds but sets the *conflict* flag for the four
ranges `cirrus.sys` claims. Poking each range's start in turn narrowed it to one: memory
`0xA0000+0x20000`, the legacy VGA aperture. Move it to `0x03000000` — still inside the 64 MB —
and it conflicts too; move it to `0x70000000` and it does not. The collision is with the
machine's **physical memory**. A PCI VGA part does decode `0xA0000` on the *bus*, but Bandit
does not forward CPU accesses there, and the kernel's conflict scan works on the raw,
untranslated list.

**Why there is no HAL fix.** `HalReportResourceUsage` is empty here, so nothing we report can be
the cause, and `HalAdjustResourceList` — the one place a HAL gets to edit a resource list — is
called only from `IoAssignResources`, not from the report path. The claim is simply not
satisfiable on this machine.

**Workaround.** A diagnostic poke that makes the check return success. **This is the one patch
in the loop that is not defensible as a fix**, and it is flagged as such wherever it appears.

The HAL did gain one real correctness fix from the investigation: `HalTranslateBusAddress` now
refuses PCI memory below `0x80000000`, because handing a driver an identity translation of
`0xA0000` would have let it write over the kernel.

---

## Part 7 — The keyboard

Setup's next screen: *"Setup did not find a keyboard connected to your computer."* It was right.
The keyboard is ADB behind Cuda, `i8042prt` probes a port `0x60` that does not exist, and NT 4.0
ships no ADB driver anywhere.

### Wall 26 — No ADB driver exists… except it does

**What we found.** Cloning and reading the three adjacent projects settled it:

- **maciNTosh** and **maciNTosh-bandit** both say *"NT HAL and drivers have no source present for
  now."* Their trees hold ARC firmware and loader only. (Correcting our own earlier notes:
  `boot_files/boot.img` is **not** the HAL and drivers — it is a 256 KB HFS volume holding the
  ARC loader's `stage1.elf`, `stage2.elf` and `BootX`.) The driver that does ADB —
  `usbadb.sys`, *"PowerMac General HID & Storage"* — ships only as a binary in a release asset.
- But **ADB source does exist for our exact chipset**, in the Bandit fork's *ARC firmware*:
  `arcbandit/source/{pxi.c,adb_bus.c,adb_kbd.c}`, with `main.c` initialising Cuda at
  `GrandCentralStart + 0x16000` — which is precisely this machine.
- And the **HAL↔driver contract is published in source** even though both binaries are not:
  `inc/halpxi.h`.
- And **entii-for-workcubes** contains a complete NT 4.0 PowerPC keyboard+mouse *port driver*
  with source — the template if we write our own.

`usbadb.sys` turns out to import exactly **six** names from `HAL.dll`, of which only three are
private: `HalPxiCommandAdb`, `HalPxiAdbSetCallback`, `HalPxiAdbAutopoll`. It connects no
interrupt and knows nothing about the chipset — all the Cuda knowledge belongs to the HAL. So
the contract is three functions wide, and the driver is chipset-independent.

The header gave the names; the *semantics* came out of disassembling maciNTosh's own
`halgoss.dll`, whose four-line delivery routine settles the callback's arguments exactly:
`cb(Status = reply[1], Command = reply[2], Data = &reply[3], Length = n-3)` — Cuda's flag byte,
the echoed ADB command byte, the payload.

**Fix.** `source/cuda.c`: a Cuda transport over Grand Central's VIA (TIP/ByteAck/TREQ over the
shift register, every wait bounded by the timebase because a HAL may not hang the machine on a
microcontroller that stops answering), plus the three exports, plus HAL ownership of Grand
Central interrupt bit 18 so that auto-polled packets have somewhere to arrive.

### Wall 27 — "The file i8042prt.sys is corrupted"

**Symptom.** Exactly that, from SETUPLDR.

**What it was.** SETUPLDR loads the keyboard driver **by file name, outright** — it does not
consult `[files.i8042]` at all — so the delivery is to write `usbadb.sys` over
`\PPC\I8042PRT.SYS`. But SETUPLDR verifies the PE checksum over **the length the ISO directory
record gives**, not the length in the image. A 21,648-byte driver zero-padded into a
38,928-byte slot fails it.

**Fix.** Recompute the checksum over the padded image (`0xc866` → `0x10be6`). The HAL slot never
showed this because `elf2pe.py` writes the checksum for whatever length it emits.

### Wall 28 — A fresh checkpoint quietly lost the SCSI adapters

**Symptom.** After rebuilding the boot checkpoint (needed because `TXTSETUP.SIF` is parsed long
before it), SCSI vanished and `INACCESSIBLE_BOOT_DEVICE` came back.

**What it was.** The recipe I derived the new cold boot from did not carry wall 16's eleven
veneer pokes. Self-inflicted, and worth recording because it looked exactly like a regression in
the HAL.

### Wall 29 — `STOP 0x00000009 IRQL_NOT_GREATER_OR_EQUAL (0x2, 0x15, 0, …)`

**Symptom.** A bugcheck inside `usbadb.sys`, asking to raise to IRQL 2 while at IRQL `0x15` = 21.

**What it was.** 21 is the VIA line's device IRQL. I was calling the driver's callback straight
from the interrupt; the callback takes a spin lock, and `KeAcquireSpinLock` *raises* to
DISPATCH_LEVEL — which from 21 is a lowering. maciNTosh's HAL says the same thing in its own way:
it does `KeLowerIrql(2)` immediately before invoking the callback.

**Fix.** The interrupt now only parks the packet in an eight-deep ring and queues a DPC; the DPC
delivers at DISPATCH_LEVEL.

### Wall 30 — `STOP 0x1E`, `STATUS_DATATYPE_MISALIGNMENT`

**Symptom.** A fault with a data address ending in `1`.

**What it was.** The `KDPC` was declared as a byte array and got byte alignment; the kernel's DPC
list operations need it aligned.

**Fix.** `ULONG[16]`.

**Bought (walls 26–30).** The ADB bus is scanned through the HAL, and the trace reads like the
hardware manual:

```
HAL: adb status 00 cmd 2f len 2 [02 01]    <- address 2, Talk r3: the KEYBOARD
HAL: adb status 00 cmd 3f len 2 [03 01]    <- address 3: the mouse
HAL: adb status 02 cmd 4f len 0            <- 4..12: nothing (timeout flag)
HAL: ADB autopoll on
HAL: adb status 40 cmd 2c len 2 [00 ff]    <- auto-polled: 'a' down
HAL: adb status 40 cmd 2c len 2 [80 ff]    <- 'a' up
```

`status 0x40` is Cuda's auto-poll flag, `cmd 0x2C` is address 2 / Talk / register 0, `00 ff` is
"key `0x00` pressed, second slot empty". Those keystrokes travelled Cuda → VIA → Grand Central
interrupt → the HAL's ring → a DPC → the driver's callback, with the driver stable underneath.
**The keyboard works**, and Setup's "no keyboard" screen is gone.

And look at what those five walls actually were: interrupt ownership, calling a callback at
IRQL 21, a `KDPC` that wanted 8-byte alignment, a PE checksum over a padded file, a veneer poke
lost in a rebuilt checkpoint. Five mistakes of our own making, and **not one of them was the
Cuda protocol** — because the protocol had already been written down for this exact silicon, in
`arcbandit/source/pxi.c`: Grand Central's VIA at `+0x16000`, TIP and ByteAck and TREQ, the
attention byte you have to throw away, "last byte" meaning TREQ went high. Reading it turned the
part of this that could have consumed a week of oscilloscope-by-printf into something that
behaved on the first try, and left us free to be wrong about the things that were genuinely ours
to get wrong.

Two names belong on that, and it is worth measuring rather than waving at.
**[`maciNTosh`](https://github.com/Wack0/maciNTosh) — Wack0 (Rairii)** — is where the code comes
from: `inc/halpxi.h`, which gave the three-function HAL↔driver contract by name and without which
there would have been nothing for a keyboard driver to talk to, is byte-identical between the two
trees, and of the 268 substantive lines in the `pxi.c` we read, 208 are `maciNTosh`'s own.
**[`maciNTosh-bandit`](https://github.com/MCJack123/maciNTosh-bandit) — MCJack123** — is the
reason it was ours to read: the fork moves Cuda to a Bandit-class machine, and that
`+0x16000` is not in the parent. One wrote it; the other pointed it at this hardware. The
callback's exact arguments came from a third place the header could not reach — maciNTosh's own
`halgoss.dll`, disassembled.

Three projects between two authors, neither of whom owes this one anything, and between them the
two hardest unknowns in the whole story were simply *already answered*. That is what publishing
your work does for the next person.

---

## Part 8 — The drive that did not exist

Setup now got far enough to launch `usetup.exe`, the user-mode text-setup program, and stopped
there:

> **Setup could not load the keyboard layout file KBDUS.DLL.**

### Wall 31 — a keyboard layout that could not be found

**Symptom.** `usetup.exe` starts and immediately dies on its layout DLL.

**What could be ruled out, and how.** `\PPC\KBDUS.DLL` is on the CD, is a valid PowerPC PE, and
its stored checksum is correct. `LoaderBlock->NtBootPathName` is `\PPC\`, so `\SystemRoot`
resolves — it is where `ntdll.dll` and `smss.exe` had just been loaded from. SETUPLDR never
preloads a keyboard layout on this architecture: its entire vocabulary of `TXTSETUP.SIF`
sections is `Scsi`, `DiskDrivers`, `ScsiClass`, `CdRomDrivers`, `Extenders`, plus files named
outright. And with the SCSI log on, **the CD is never read for it** — no access to its extent,
none to the `\PPC` directory after the kernel starts. Copying `KBDUS.DLL` into `\PPC\SYSTEM32`,
the image directory and therefore first on the loader's search path, changed nothing: the screen
came back byte-identical.

So the failure happened *before* anything touched a file system. That is the whole finding.

**Analysis.** `usetup` imports `LdrLoadDll` and loads the layout itself, passing a rooted DOS
path. `LdrLoadDll(NULL, ...)` resolves a rooted path — one that starts with a backslash and no
drive — against the process's **current drive**. On this machine there was no current drive,
because there were no drives at all: `IoAssignDriveLetters` is a HAL export, and ours was a stub
that returned without doing anything. Nothing had ever created `\DosDevices\A:`, `\DosDevices\C:`
or `\DosDevices\D:`, and nothing had rewritten `NtSystemPathString` from an NT device path into
a DOS one. A rooted path with no drive to root it against fails inside `ntdll`, never reaching
the object manager, let alone CDFS — exactly the shape of the evidence.

It is an odd place to put the function. Assigning drive letters is policy, not hardware, and on
NT 4.0 it lives in the HAL only because the *order* of the assignment is architecture-specific:
an x86 machine numbers its disks the way the BIOS did, an ARC machine the way the firmware did.
A stub is therefore silent — nothing bugchecks, nothing logs, the system simply has no drives —
until the first component that assumes a DOS namespace goes looking for one.

**Fix.** Implement it, in `source/misc.c`: `A:` and `B:` for floppies, `C:` upward for
`\Device\Harddisk%d\Partition1`, then the CD-ROMs as `\Device\CdRom%d`, each one an
`IoCreateSymbolicLink` under `\DosDevices\`. Then find the boot device by matching the
`NtDeviceName` the kernel passed in, and rewrite `NtSystemPathString` in place with that drive
letter — replacing only the letter when the incoming path already looks like `X:\…`, and
prefixing otherwise.

```
HAL: IoAssignDriveLetters: boot device '\Device\CdRom0' -> D:, 0 floppy 1 disk 1 cdrom
HAL: system path -> 'D:\PPC\'
```

**Bought.** Everything. Setup came up:

> **Welcome to Setup.**

and then, one key at a time over the ADB keyboard built in Part 7: the mass-storage list
(*Symbios Logic C810 PCI SCSI Host Adapter*), six pages of licence agreement, and the hardware
confirmation screen — computer *MOTOROLA PowerStack*, display *Motorola Power Stack (cirrus
54xx)*, keyboard *XT, AT, or Enhanced*, layout *US*, pointing device *Mouse Port Mouse*. Every
one of those lines is something earlier in this story. Screenshots are in
[`traces/`](traces/), `2026-09-06-setup-01-welcome.png` onward.

---

## Part 9 — A disk with nothing on it

### Wall 32 — no valid system partitions

**Symptom.** Past the hardware screen, Setup stops dead:

> No valid system partitions are defined on this computer, or all system partitions are full.
> Windows NT requires 750 kilobytes of free disk space on a valid system partition.
> …Setup cannot continue. Press F3 to exit.

**Analysis.** On NT 4.0 the partition table lives on the HAL side of the boundary:
`IoReadPartitionTable`, `IoWritePartitionTable` and `IoSetPartitionInformation` are all HAL
exports, and `disk.sys`, `ftdisk` and text-mode Setup reach the MBR only through them — nothing
above the HAL ever reads sector 0 itself. Ours were the same kind of stub as
`IoAssignDriveLetters`: `IoReadPartitionTable` set `*Layout = NULL` and returned
`STATUS_UNSUCCESSFUL`, so **every disk on the machine read back as having no partitions at all**.

The disk was empty in a second sense too. NT's Setup on an ARC system wants a FAT partition with
750 KB free on the boot disk, and on real hardware the firmware vendor's `ARCINST.EXE` creates
it. Open Firmware has no such tool, and the emulated disk had no MBR signature at all.

**Fix.** `source/disk.c`: all three exports for real. Reads and writes go out as
`IRP_MJ_READ`/`IRP_MJ_WRITE` built with `IoBuildSynchronousFsdRequest` and sent with
`IoCallDriver`, waiting on an event — the standard pattern, legal here because every caller of
these three is at `PASSIVE_LEVEL` in a real thread. `IoReadPartitionTable` walks the four primary
slots and then the extended chain behind them, numbering recognised partitions the way NT does.
And `tools/mkarcdisk.py` does `ARCINST.EXE`'s job: an MBR with one active primary FAT16
partition and an empty FAT laid down inside it.

One detail that is not about MBRs at all: a byte offset divided by a sector size is a 64-bit
division, and a freestanding HAL has no `__udivdi3` to call. Shift-and-subtract long division,
sixty-four iterations, keeps the module self-contained — the same reason `memset` and `memcpy`
are hand-written in `misc.c`.

### Wall 33 — the compiler puts the misaligned load back

**Symptom.** With the code above in place, the sector read back perfectly — the trace showed the
signature `44 48 54 4e` and the entry `80 01 02 04 06 04 04 10 00 10 00 00 00 30 00 00`, exactly
what `mkarcdisk.py` had written — and then:

```
*** STOP: 0x0000001E (0x80000002, 0x806495B4, ...)   KMODE_EXCEPTION_NOT_HANDLED
*** 806495B4 has base at 80647000 - hal.dll
```

`0x80000002` is `STATUS_DATATYPE_MISALIGNMENT`.

**Analysis.** The 32-bit fields in a partition entry sit at `+8` and `+12` inside a 16-byte
record that starts at `0x1BE`, so they are never 4-byte aligned. On x86 that is free; on a 604
running **little-endian** a misaligned load raises an alignment exception instead of being fixed
up in hardware. That is exactly why the code assembles them a byte at a time. Disassembling the
faulting address showed what had happened to that care:

```
800125b0:  lbzu 11, 16(6)     ; the type byte, and step to the next entry
800125b4:  lwz  8, 4(6)       ; <- the fault
```

clang recognises `p[0] | p[1]<<8 | p[2]<<16 | p[3]<<24` as a little-endian 32-bit load and folds
the four byte loads back into one `lwz` — the precise instruction the byte-at-a-time code existed
to avoid. The optimiser is not wrong in general: it cannot see that this pointer is misaligned,
and for PowerPC LLVM assumes misaligned access is allowed.

**Fix.** `const volatile UCHAR *` on the accessor. volatile forbids the fold; the same code then
emits `lbz 7, 4(6)`.

### Wall 34 — and then the misaligned store

**Symptom.** The same bugcheck, a few builds later, in brand-new code that had nothing to do with
partition tables — `HalpSeedEnvironment`, which appends the literal `"partition(1)"` to an ARC
name:

```
80015624:  lis   6, 29810      ; 0x7472  "tr"
8001562c:  ori   6, 6, 24944   ;         "pa"
80015630:  stwux 6, 5, 4       ; <- "part", as one word
8001563c:  stw   6, 4(5)       ;    "itio"
80015648:  stw   6, 8(5)       ;    "n(1)"
```

The ARC name is `multi(0)scsi(1)disk(0)rdisk(0)` — thirty characters — so the append started at
offset 30, two bytes off a word boundary.

**Analysis.** This is the mirror image of wall 33, and worse, because it is not confined to one
clever idiom: LLVM merges adjacent byte stores into wider stores whenever it can prove they are
adjacent, which makes **any** string copy in the HAL a candidate. A `volatile` here and there
would be whack-a-mole.

**Fix — global, in the Makefile.** `-mllvm -combiner-store-merging=false`. LLVM's PowerPC target
does not plumb `+strict-align` through to this transform (tested: it changes nothing), and
`-fno-builtin` does not either, but turning the store-merging combine off does — the same
function then emits nothing but `stb`. Cost: irrelevant for a HAL.

Two walls, one lesson worth stating plainly: **on this target, "I wrote it byte at a time" is not
a guarantee of anything.** The compiler is entitled to undo it in both directions, and the only
reliable answers are `volatile` at the point of access or a flag that switches the transform off.

### Wall 35 — a system partition that nobody had declared

**Symptom.** With the partition table read correctly —

```
HAL: IoReadPartitionTable: 1 partitions, signature 4e544844
HAL:  #1 type 00000006 start 00001000 len 00003000 recognized
HAL: IoAssignDriveLetters: boot device '\Device\CdRom0' -> D:, 0 floppy 1 disk 1 cdrom
```

— Setup showed the *same* "no valid system partitions" screen, byte for byte.

**Analysis.** Setup does not go looking for a system partition. It **asks the firmware for one**,
through `NtQuerySystemEnvironmentValue`, which on this architecture lands in
`HalGetEnvironmentVariable` — another HAL export, and another stub, ours returning `ENOENT` for
everything. Setup's own message says so, if you read it as a specification rather than an error:
*"System partitions are created and managed by a manufacturer-supplied configuration program."*
The variable it wants is `SYSTEMPARTITION`, and `ARCINST.EXE` is what writes it.

Two things from the boot log made the fix easy. SETUPLDR had already read the new MBR:

```
HAL: arc disk sig 20202020 sum d277fb75 valid 0 'multi(0)scsi(0)cdrom(0)fdisk(0)'
HAL: arc disk sig 4e544844 sum c1d0a3b6 valid 1 'multi(0)scsi(1)disk(0)rdisk(0)'
```

`4e544844` is the signature `mkarcdisk.py` wrote, and `valid 1` means the table parsed. So the
ARC path of the system partition was already there for the taking.

**Fix.** A real environment store in `source/misc.c` — the ARC `NAME=value\0…\0\0` shape, with
get *and* set both working, because Setup writes `OSLOADER`, `OSLOADPARTITION`, `OSLOADFILENAME`
and `LOADIDENTIFIER` back at the end of the copy phase. It is RAM-backed: the Network Server has
no ARC NVRAM (Open Firmware's own `nvram` partitions are a different format with a different
owner), and nothing here boots from the ARC environment anyway, because Open Firmware's boot
script is what starts the veneer. Then a seeding pass at phase 0 derives `SYSTEMPARTITION` from
the loader's ARC disk list: the first disk whose partition table SETUPLDR could read, plus
`partition(1)` — what an `ARCINST` run on this machine would have written.

The trace shows the whole ARC vocabulary Setup expects, and which one answered:

```
HAL: env set 'SYSTEMPARTITION' = 'multi(0)scsi(1)disk(0)rdisk(0)partition(1)'
HAL: env get 'SystemPartition'  -> 'multi(0)scsi(1)disk(0)rdisk(0)partition(1)'
HAL: env get 'LoadIdentifier'   -> ENOENT
HAL: env get 'OsLoader'         -> ENOENT
HAL: env get 'OsLoadPartition'  -> ENOENT
HAL: env get 'OsLoadFilename'   -> ENOENT
HAL: env get 'OsLoadOptions'    -> ENOENT
```

**Bought.** Setup's partition screen:

```
8 MB Disk 0 at Id 0 on bus 0 on symc810
      Unpartitioned space          2 MB
  C:  FAT                          6 MB (  5 MB free)
ENTER=Install   C=Create Partition   F1=Help   F3=Exit
```

It found the disk through `symc810`, read the table through our `IoReadPartitionTable`, mounted
the FAT16 volume and gave it `C:`. Pressing Enter gets the one remaining complaint, which is not
a defect in anything:

> The partition or unpartitioned space you chose is too small for Windows NT. Select a partition
> or unpartitioned space whose size is at least 158 megabytes.

Screenshots: [`traces/`](traces/), `2026-09-06-setup-11-partition-list.png` and `-12-…`.

## Part 10 — Installing

### Wall 36 — 158 megabytes

**Symptom.** From the partition screen, Enter gives:

> The partition or unpartitioned space you chose is too small for Windows NT. Select a
> partition or unpartitioned space whose size is at least 158 megabytes.

**What it was.** Nothing. The emulated disk was 8 MB — the veneer's staging disk, which exists
to hold `VENEER.EXE` at block `0x800`, not to install onto. Worth recording only because of
what happened next.

**The false start.** The obvious move — attach a *second*, larger disk and leave the staging
disk alone — does not work, and the reason is in the loader's own disk list:

```
HAL: arc disk sig 20202020 sum d277fb75 valid 0 'multi(0)scsi(0)cdrom(0)fdisk(0)'
HAL: arc disk sig 00000000 sum 29f28bb3 valid 0 'multi(0)scsi(1)disk(1)rdisk(0)'
HAL: arc disk sig 00000000 sum 29f28bb3 valid 0 'multi(0)scsi(1)disk(0)rdisk(0)'
```

Both hard disks report `valid 0` **with identical checksums**, so SETUPLDR read the same thing
from both — which cannot be true, since one had a fresh MBR on it and the other a text label.
The veneer's ARC path for a second SCSI target does not reach the disk. Meanwhile the kernel,
going through `symc810` a moment later, read that same MBR perfectly. Two paths to one disk,
one of them broken, and the broken one is the firmware's.

**Fix.** Do not fight it: one hard disk, as before, just bigger. A 512 MB image whose first
4096 sectors are the staging disk's verbatim — label, and `VENEER.EXE` still at `0x800` — with
the MBR and a 32 MB FAT16 system partition spliced in at run time by
`tools/run-hal.py --disk-delta`, which is what the 8 MB chain had been doing all along. That
reproduces the *timing* that worked, not just the layout: the disk boots without an MBR, and
gains one before SETUPLDR reads signatures.

```
HAL: arc disk sig 4e544844 sum 60d0b3a8 valid 1 'multi(0)scsi(1)disk(0)rdisk(0)'
HAL: env set 'SYSTEMPARTITION' = 'multi(0)scsi(1)disk(0)rdisk(0)partition(1)'
```

**Bought.** Setup's partition screen, on a disk it will accept:

```
512 MB Disk 0 at Id 0 on bus 0 on symc810
      Unpartitioned space           2 MB
  C:  FAT                          32 MB (  31 MB free)
      Unpartitioned space         478 MB
```

### Wall 37 — the arrow keys were the wrong arrow keys

**Symptom.** Down-arrow did nothing. The highlight stayed on the 2 MB gap, Enter chose it, and
Setup said "too small" again — which looked like the same wall twice.

**What it was.** Not Setup, and not our Cuda code: the *translation table* inside the keyboard
driver. `usbadb.sys` maps ADB key codes to USB HID usages, and dumping its table shows:

```
3b->50  3c->4f  3d->51  3e->52     Left, Right, Down, Up  (Apple Keyboard II codes)
7b->e5  7c->e6  7d->e4  7e->e5     Right Shift, Right Alt, Right Ctrl
```

The emulator's `"down"` resolves to `0x7D`, which is the Extended Keyboard II arrow code and
what every modern Mac uses — and this table reads `0x7D` as *right control*. Page Down had
worked all along because `0x79 -> 0x4e` is the same in both conventions, which is exactly why
the problem hid.

**Fix.** Send `0x3D`. Two keystrokes later the 478 MB space was selected.

**Lesson.** A borrowed binary carries its author's assumptions about hardware you both thought
you shared. Nothing in the header said which arrow convention; the table did.

### Wall 38 — the layout convention `IoWritePartitionTable` had to learn

**Symptom.** Setup created a partition, formatted it, took `\WINNT` as the install directory,
started *"Building list of files to be copied"*, and then:

```
*** STOP: 0x0000001E (0xC0000005, 0x806A87FC, 0x00000000, 0x00000004)
*** 806A87FC has base at 8067B000 - setupdd.sys
```

A NULL+4 read inside Microsoft's own setup driver.

**What the HAL had logged just before.** Two lines that turned out to be the whole story:

```
HAL: IoWritePartitionTable: 8 entries, only the first 4 are written
HAL: IoSetPartitionInformation: partition 2 is not a primary (have 1)
```

**Analysis.** Eight entries means **two on-disk tables**, and the first implementation wrote
only the MBR. Logging every entry showed what Setup actually wanted:

```
[0] type 06 start 4096   len 65536   hidden 0  num 1  boot      the FAT16 system partition
[1] type 05 start 69632  len 978944  hidden 0  num 0  rewrite   an extended container
[4] type 06 start 69664  len 978912  hidden 0  num 2  rewrite   a logical drive inside it
```

Three facts, none of them guessable:

1. `disk.sys` hands down **four entries per on-disk table** — group 0 is the MBR, every group
   after it is one logical drive's extended boot record.
2. Setup made the new partition a **logical drive in an extended partition**, not a second
   primary. There were two free slots; it used the chain anyway.
3. **`HiddenSectors` arrives zero.** The first implementation derived each table's own sector
   from `start - HiddenSectors`, which put the extended boot record at 69664 — the start of its
   own *data* — instead of at 69632, where the MBR's type-05 entry points. The chain then read
   back as unterminated garbage, `IoSetPartitionInformation` could not find partition 2
   (`c0000001`), and `setupdd` walked the result into a null pointer.

So every relative-sector field has to be computed from the absolute offsets, honouring the two
rules an extended chain actually uses: **a partition entry counts from the table it sits in; a
chain link counts from the outermost extended partition.**

**Fix.** `source/disk.c`, and the log says it landed:

```
HAL:  table 0 -> lba 0     (00000000)
HAL:  table 1 -> lba 69632 (00000000)
HAL: IoSetPartitionInformation: #2 type -> 00000006 (table lba 69632 slot 0, 00000000)
```

### Wall 39 — the crash that moved, and named its own cause

**Symptom.** With the chain written correctly, `setupdd.sys` crashed at the *same instruction*.
The partition table had been a real bug; it was not *this* bug.

**What it took to find.** The faulting instruction is the second load of a "return element `r4`
of the list at `r3+8`" helper, so the first assumption was a corrupt list. Chasing it cost four
probes and taught two things worth keeping:

- The breakpoint fired 18,171 times with a valid pointer every time, and the loop *exited
  normally* on the last one — because **this emulator's breakpoints report after the instruction
  executes**, so the load that faults never produces a hit at all. Every conclusion drawn from
  "the last hit looks fine" was therefore drawn from the wrong instruction.
- A `mmu.peek` of the same address disagreed with the register: the shell does not apply the
  little-endian address munge, so a CPU word at `A` is `mmu.peek(A^4)`. Calibrating that
  against a known value was the only reason the dumps meant anything.

**What actually settled it was not a probe.** Pre-creating *two* primary FAT16 partitions with
`tools/mkarcdisk.py`, so Setup never had to create one, skipped `IoWritePartitionTable`, the
extended chain and the format in a single step — and Setup sailed past the crash:

> Setup will install Windows NT on partition **D: FAT 478 MB (477 MB free)** on 512 MB Disk 0 at
> Id 0 on bus 0 on symc810

…through the filesystem choice, `\WINNT`, the surface-scan prompt, and on to
**"Creating directory \WINNT…"** — writing to the volume. Then a *different* crash, and this
one named its cause:

```
*** STOP: 0x0000001E (0xC0000005, 0x800E8730, 0x00000000, 0x00000006)
*** 800E8730 has base at 8007B000 - ntoskrnl.exe
```

`0x800E8730` disassembles to the first instruction of **`_wcsicmp`**: `lhz r10,0(r3)`, reading a
halfword at `r3 == 6`. Six is a `UNICODE_STRING`'s `Length` field — three characters, e.g.
`"D:\"` — passed where its `Buffer` belonged.

**What it is.** Ours. `IoAssignDriveLetters` creates `\DosDevices\` links only for
`\Device\Harddisk%d\Partition1` — one letter per *disk*, not one per partition. Setup numbers
the volumes itself and called the install target `D:`; we had given `D:` to the CD, and the
partition it installs to has no symbolic link at all. The real HAL assigns a letter to every
recognised partition in a specific order: the first primary of each disk, then logical drives,
then the remaining primaries, then the CD-ROMs.

**Not fixed here.** The cause is identified and the fix is understood, but there is a next wall
at every stage of this and there always will be; this is where the story stands at publication.
Screens: [`traces/`](traces/), `2026-09-07-setup-13-partition-list-512mb.png` onward.

### Wall 40 — a bug found by reading rather than by crashing

**Symptom.** None. NT never complained.

**What it was.** `HalSetEnvironmentVariable` removed an entry by shifting bytes down until it
saw a NUL *pair*, and never cleared the bytes the shift vacated. So every re-set of a variable
left stale structure behind, which the next removal walked into. Extracting the shipped
functions into a native harness showed the store scrambling under ordinary use:

```
before the fix, after eight sets and eight deletes:
   |.F=6..F=6..F=6..F=6..F=6..F=6...=7...=8...|
```

— and with a tail that happened to contain no NUL pair, the loop would have run past the end of
the array entirely. Setup writes `OSLOADER`, `OSLOADPARTITION`, `OSLOADFILENAME` and
`LOADIDENTIFIER` at the end of the copy phase, so this was sitting directly in the path of the
next thing to be attempted.

**Fix.** Compute the gap and the tail explicitly, move exactly that many bytes, clear what the
move vacated, and bound every walk by the store's own length rather than by finding a
terminator. Verified with the real functions under AddressSanitizer: the two sequences that
scrambled it, Setup's five actual writes, and 200,000 random operations with the store's
structure checked after every one.

**Lesson.** The walls in this story were all found by a machine stopping. This one was found by
reading the code with the specific question *"what happens on the second call?"* — which is a
cheaper way to find a bug than waiting for it to corrupt something at the far end of a
forty-minute run.

---

## The ledger of workarounds

Everything above that is *not* a fix, kept in one place so it is never forgotten:

| # | What | Why it is not a fix | What would be |
|---|---|---|---|
| 1 | Two `nop`s for the veneer's `claim` failures | patches a Microsoft binary in memory | inherited from the thread; needs a story for real hardware |
| 2 | `OFClose` nret, two words | same | a veneer replacement, or upstream acceptance |
| 3 | `partition(1)` and `:0` strings blanked | same | same |
| 4 | `VrOpen`'s partition branch `nop`ed (wall 22) | same | same |
| 5 | Eleven bytes renaming the veneer's SCSI model (wall 16) | same | a `TXTSETUP` `[Map.SCSI]` addition on an OEM disk would be cleaner |
| 6 | `VideoPortVerifyAccessRanges` bypass (wall 25) | **defeats a correct conflict check** | a video driver that does not claim the `0xA0000` aperture |
| 7 | `usbadb.sys`, a binary we may not redistribute | GPL-2.0 with no published source, so GPL §3 cannot be satisfied | our own port driver, from the `fpsidrv` template, on the HAL half that already exists |
| 8 | The driver written over `\PPC\I8042PRT.SYS` | abuses a name SETUPLDR hardcodes | a proper OEM driver disk (`winnt.sif` + `txtsetup.oem`) |
| 9 | `SYSTEMPARTITION` synthesised by the HAL from the loader's ARC disk list (wall 35) | on real ARC hardware it is NVRAM, written by `ARCINST.EXE`; the HAL guessing it is policy in the wrong place | an environment store backed by Open Firmware's own `nvram`, or an `ARCINST` equivalent for this machine |
| 10 | The ARC environment does not survive a reboot | it is plain memory in the HAL | the same nvram-backed store |
| 11 | Both partitions pre-created by `tools/mkarcdisk.py`, so Setup never runs its own partition-creation path (wall 39) | dodges the `setupdd.sys` crash rather than fixing it; a real install must be able to partition a blank disk from inside Setup | the drive-letter fix, after which Setup's own create-and-format path is worth retrying |
| 12 | The MBR and system partition spliced into the checkpoint's copy-on-write delta at run time (`run-hal.py --disk-delta`) | a test-rig convenience, not a property of the machine: it exists so the disk boots without an MBR and gains one before SETUPLDR reads signatures (wall 36) | a cold-boot chain that starts from an already-partitioned disk, once the veneer's ARC path for it is trusted |

Workarounds 1–5 all live in the veneer and all exist because Microsoft's ARC shim was written for
a machine whose firmware behaves slightly differently. They are load-time memory pokes, they are
scripted, and they are documented — but a published project needs a better answer than "type
these into Open Firmware".

## What the HAL actually is now

About 2,900 lines of C and assembly, built with clang, lld and `tools/elf2pe.py`, with no
Microsoft tool anywhere in the build:

- `HalInitSystem` phases 0 and 1, processor init, the 256 MB device BAT
- Grand Central interrupt dispatch, IRQL masking, `HalEnable/DisableSystemInterrupt`
- the decrementer clock, stalls and the performance counter on the timebase
- a machine-check handler that survives stray PCI probes
- Bandit configuration space for both bridges, bus-address translation, `HalAssignSlotResources`
- the MBR partition table (`IoReadPartitionTable` and friends) over synchronous IRPs
- drive-letter assignment, and an ARC firmware environment Setup can read and write
- DMA adapters, `HalAllocateAdapterChannel`, `IoMapTransfer`
- a Cirrus 54M30 framebuffer console, and `HalQueryDisplayParameters` so Setup lays out its UI
- Cuda over the VIA, and the three ADB entry points a keyboard driver needs
- ESCC console, cache sweeps, ARC-tree repair

And on the emulator side, one committed fidelity improvement: the four Cirrus registers a driver
identifies the part by.

## Eight things this taught, that generalise

1. **A missing symptom is not evidence.** Wall 20 — "the interrupt never arrives" — was a
   stubbed allocator four layers up. The interrupt path was healthy the entire time.
2. **The binary is the specification.** Every ABI rule in Part 3, every structure offset, and the
   whole ADB contract came out of disassembling shipped binaries and watching what they read.
   Twice the *caller* settled a question the header could not.
3. **Two opposite conventions can govern one type.** `LARGE_INTEGER` in a register pair going in,
   through a hidden pointer coming out. Learning one and assuming the other cost a bugcheck.
4. **Make the wrong thing impossible.** The `_Static_assert` on the 16-byte resource descriptor is
   the single highest-value line in the project; the same bug cannot come back.
5. **Write down which patches you are ashamed of.** The table above is the difference between a
   project that can be published and one that quietly depends on eight undocumented pokes.
6. **Your instrumentation lies too, and you will believe it.** This emulator's breakpoints report
   *after* the instruction executes, so a breakpoint on a faulting load never fires for the
   fault — 18,171 hits on the crashing instruction, every one healthy, and every conclusion drawn
   from the wrong instruction (wall 39). The shell's memory peek does not apply the little-endian
   address munge either. Calibrate a debugger against a value you already know before you trust
   what it tells you.
7. **A borrowed binary carries its author's assumptions about hardware you both thought you
   shared.** `usbadb.sys` reads ADB `0x3B`–`0x3E` as the arrow keys and `0x7B`–`0x7E` as the
   right-hand modifiers — the older Apple keyboard's convention. Nothing in the header said so;
   the translation table did (wall 37).
8. **Some bugs are cheaper to read than to run into.** Wall 40 was found by asking one question
   of existing code — *what happens on the second call?* — and it was sitting directly in front
   of the next thing to be attempted. Every other wall here was found by a machine stopping,
   which on a forty-minute boot costs a great deal more than reading does.
