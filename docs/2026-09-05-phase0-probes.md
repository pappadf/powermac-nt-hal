# Phase 0 probes — what SETUPLDR does after the HAL menu (2026-09-05)

*Emulator: Granny Smith `8ebed27` (`ans500`, 64 MB, ROM 2.26NT, NT 4.0 Workstation OEM CD
000-48303 attached at `/bandit/53c825@11/sd@0,0`). Every fact below comes from the shipped
`SETUPLDR` / `VENEER.EXE` binaries' own symbol tables and from watching them run; no Microsoft
source was consulted. Console captures are in [`../traces/`](../traces/).*

## 0. Results in one table

| Question (charter §9) | Answer |
|---|---|
| 1. Can keys reach SETUPLDR's menus through ttya? | **Yes.** `SlGetChar` reads one byte per ARC `Read` on `ConsoleIn`; arrows are 8-bit CSI sequences (`0x9B 'A'` = Up). The firmware delivers only the **first byte of any burst**, so keys go one byte per `machine.scc.a.receive` call with a run gap between (§1). |
| 4. Where does a shipped HAL fault? | **It never runs.** After *MOTOROLA PowerStack*, SETUPLDR loads `HALEAGLE.DLL`, the configuration data, the setup font and three NLS files, then re-opens the bare CD device to check the media, and that open fails: `OFOpen(...) IHandle: 0`. Root cause is the veneer, not the HAL (§3). |
| — What does *Other* ask for? | A "Manufacturer-supplied hardware support disk" in **Drive A:**. SETUPLDR finds A: by searching the ARC tree for a `FloppyDiskPeripheral` (class 5, type 0x1A); the veneer exposes SWIM3 as `other`, so the path is uninitialised and the open fails with ENODEV. Dead end without a veneer change (§2). |
| 2. Does `symc810` drive a 53C825A? | Undecided, but the *matching* question is answered: `SYMC810.SYS` imports no `ScsiPortGetBusData` and carries no PCI ID strings; it takes its registers from the ARC `ScsiAdapter` configuration data that the firmware supplies. Text-mode Setup picks it by the ARC Identifier (`[Map.SCSI] symc810 = *NCRC8`). Ours is `UNKNOWN SCSI` (§5). |
| — Delivery route without a floppy or a `TXTSETUP.SIF` edit | SETUPLDR reads `winnt.sif` from its own directory (`\PPC\winnt.sif`); `[unattended] OemPreinstall = yes` plus `computertype = "<name>", OEM` makes it take the HAL from `$OEM$\TEXTMODE\txtsetup.oem`. An **additive** ISO is enough (§4). |

The wall after the menu is a firmware-resource leak in the veneer: every `OFClose` fails, so
every file SETUPLDR opens leaks an Open Firmware instance, and 2.26NT stops opening the CD
after nine live instances. A two-instruction patch to the veneer's `OFClose` is under test (§3.3).

## 1. Driving SETUPLDR's menus from the serial console

`SlGetChar` (`SETUPLDR` image `0x80603F20`, symbol table in the file) calls the ARC `Read`
vector on file id 0 for one byte at a time and decodes:

| bytes read | code returned | meaning in `SlDisplayMenu` |
|---|---|---|
| `0x0D` or `0x0A` | `0x0D` | select |
| `0x1B` | `0x1B` | cancel |
| `0x9B 'A'` / `'B'` | `0x10000` / `0x20000` | up / down |
| `0x9B 'H'` / `'K'` | `0x30000` / `0x40000` | home / end |
| `0x9B '?'` / `'/'` | `0x50000` / `0x60000` | page up / down |
| `0x9B 'O' 'P'` / `'w'` / `'t'` / `'u'` | `0x1000000` / `0x3000000` / `0x5000000` / `0x6000000` | F1 / F3 / F5 / F6 |

Only the 8-bit CSI (`0x9B`) introducer is recognised; `ESC [ A` is read as a cancel followed
by two ordinary characters. The veneer's `VrRead` on the console (`VENEER.EXE` `0x549D0`)
passes Open Firmware `read` bytes through unchanged, with a one-byte read-ahead used by
`VrGetReadStatus`.

**Gotcha, measured:** `machine.scc.a.receive("\x9bA")` delivered only the `0x9B`; the `A` was
lost, and the menu moved only when a later byte arrived. Send one byte per call and run
~30 M instructions between bytes. A breakpoint at `SlGetChar`'s common exit, `0x806040D8`,
shows the decoded code in `r3` (`tools/gen-probe.py` does this for every key it sends).

Trace: [`2026-09-05-halmenu-probe-keys.txt`](../traces/2026-09-05-halmenu-probe-keys.txt).

## 2. *Other* — the OEM hardware-support disk

Enter on *Other* → `SlPromptOemHal` → `SlpOemDiskette`:

```
Please insert the disk labeled
  Manufacturer-supplied hardware support disk
into Drive A:
*  Press ENTER when ready.
```

`SlpOemDiskette` (`0x80605408`) calls `SlpFindFloppy` (`0x80605DC4`), which walks the ARC
configuration tree with `BlSearchConfigTree(root, class 5, type 0x1A, FoundFloppyDiskCallback)`
— PeripheralClass / FloppyDiskPeripheral — and takes the first match's pathname. The veneer's
tree has no such component (SWIM3 is converted to `other`), so `FloppyDiskPath` is never
written and Enter opens garbage: `VrOpen: Entry - Path: T\xD1a\x80` → `ENODEV` → the prompt
returns. With a disk found, SETUPLDR would read `A:\txtsetup.oem`, show its `[Defaults]`
computer entry in a menu, and `BlLoadImage` the named HAL.

Consequence: the floppy route needs the veneer to emit a `FloppyDiskPeripheral` for SWIM3
(the emulator has SWIM3; a real ANS has the drive). Not pursued in phase 0.

Traces: [`…probe-other.txt`](../traces/2026-09-05-halmenu-probe-other.txt),
[`…probe-other-enter.txt`](../traces/2026-09-05-halmenu-probe-other-enter.txt) (with `VrDebug`).

## 3. *MOTOROLA PowerStack* — a shipped HAL is accepted and loaded

Five Ups and Enter (`tools/gen-probe.py 5 powerstack 40`):

```
Setup is loading files (MOTOROLA PowerStack)...             ← HALEAGLE.DLL, per [Hal.Load]
Setup is loading files (Windows NT Configuration Data)...
Setup is loading files (Setup Font)...                      ← vgaoem.fon
Setup is loading files (Locale-Specific Data)...  ×3        ← c_1252.nls, c_437.nls, l_intl.nls
Please insert the disk labeled
  Windows NT Workstation CD-ROM
into Drive A:
```

Every file loaded (the emulator's SCSI log shows each CDFS extent being read). The prompt is
SETUPLDR's media check before the next group of files: it opens the *device* with no
partition and no file, and the veneer's trace (`VrDebug = 0xFFFFFAFF`) shows:

```
VrOpen: Entry - Path: multi(0)scsi(0)cdrom(0)fdisk(0) Mode: 0
VrOpen: Partition 'NULL' FilePath 'NULL'
OFOpen('/bandit@F2000000/53c825@11/sd@0,0@0,0')
OFOpen: IHandle: 0
```

### 3.1 It is not the path

The doubled unit address is how the veneer always spells this device; the successful opens
earlier in the boot used `/bandit@F2000000/53c825@11/sd@0,0@0,0:1,\PPC\SETUPLDR`. At the
Open Firmware prompt, `" /bandit@F2000000/53c825@11/sd@0,0@0,0" open-dev` returns a valid
ihandle. No SCSI command was issued for the failing open, so the firmware refused it before
reaching the `sd` package.

### 3.2 It is an instance leak

At the prompt, `open-dev` on the CD succeeds **nine** times and then returns nothing (the
tenth and every later open fail) — [`…probe-ofopen3.txt`](../traces/2026-09-05-halmenu-probe-ofopen3.txt).
Between `go` and the media check the veneer printed `OFClose(…) failed` **twelve** times, one
per file SETUPLDR opened, i.e. twelve instances were never released. The media check is the
first open that finds the pool exhausted.

`OFClose` (`VENEER.EXE` `0x52228`) builds the client-interface call as `{ "close", nargs = 1,
nret = 1, ihandle, 0 }`. IEEE 1275 `close` returns nothing; this firmware's client interface
rejects the call (nonzero return, printed as "failed") and leaves the instance open. `open`,
`read` and the others are built with the right counts.

### 3.3 The fix under test

Two instructions in `OFClose` swap the stores so `nret = 0`:

| image address | was | becomes |
|---|---|---|
| `0x52254` | `91430008  stw r10,8(r3)` (nret = 1) | `39200000  li r9,0` |
| `0x52258` | `39200000  li r9,0` | `91230008  stw r9,8(r3)` (nret = 0) |

Applied in the emulator before `go` as `machine.memory.poke.l 0x52250 0x39200000` and
`machine.memory.poke.l 0x5225c 0x91230008` (little-endian mode: word at A lives at A^4). In
the file the same words are at offsets `0x2454` and `0x2458` (file = image − 0x50000 + 0x200).
Result: see the addendum at the end of this note.

Traces: [`…probe-powerstack.txt`](../traces/2026-09-05-halmenu-probe-powerstack.txt),
[`…probe-ps-enter-full.txt`](../traces/2026-09-05-halmenu-probe-ps-enter-full.txt),
[`…probe-ofopen.txt`](../traces/2026-09-05-halmenu-probe-ofopen.txt) (opens with `close-dev`),
[`…probe-ofopen2.txt`](../traces/2026-09-05-halmenu-probe-ofopen2.txt) (the doubled path opens).

## 4. How SETUPLDR can be told to use our HAL — three routes

From `SETUPLDR`'s own code (`setup.c` path strings in its `.rdata`):

1. **`TXTSETUP.SIF`** (charter §4): `[Map.Computer]` maps the ARC root `Identifier` to a
   computer id; `[Hal.Load]` maps the id to the HAL file loaded during Setup, `[hal]` to the
   HAL installed. Needs an edited `TXTSETUP.SIF` on the CD and, unless we patch the veneer's
   `convert_name`, an entry for the identifier `device-tree`.
2. **Hardware-support floppy** via *Other*: needs a `FloppyDiskPeripheral` in the ARC tree (§2).
3. **Preinstall / unattended** (`SETUPLDR` `0x806006D0`–`0x80600944`): SETUPLDR appends
   `winnt.sif` to its own directory (`\PPC\`) and reads it. `[unattended]`
   `OemPreinstall = yes` sets `PreInstall`; `computertype = "<text>", OEM` sets `OemHal`
   (`ComputerType` = the text, `OemTag` compared case-insensitively with the second field);
   `massstoragedrivers` lists OEM SCSI drivers. With `OemHal` set, `SlPromptOemHal` reads
   `$OEM$\TEXTMODE\txtsetup.oem` from the boot device instead of A: and loads the HAL it
   names. **Additive only**: a rebuilt ISO with `\PPC\winnt.sif`, `\$OEM$\TEXTMODE\txtsetup.oem`
   and the HAL — nothing on the original CD is modified. Preferred delivery for the emulator
   and for a real ANS with a burned CD.

Route 3 still needs the veneer's `OFClose` fixed (§3), because the OEM files are more opens.

## 5. `SYMC810.SYS` and the 53C825A (charter §9.2, first look)

- Imports (17, all `SCSIPORT.SYS`): register read/write, `ScsiPortGetDeviceBase`,
  `GetUncachedExtension`, `Notification`, … — **no `ScsiPortGetBusData`**, and no `1000`/`0001`
  PCI ID strings anywhere in the file. The driver does not enumerate PCI. `SCSIPORT.SYS` does
  (`HalGetBusData`, `HalAssignSlotResources`, `HalGetAdapter`, `HalTranslateBusAddress`, …
  are its HAL imports); the miniport gets base address and interrupt from the
  `ScsiAdapter` component's `ConfigurationData` and from `ScsiPortInitialize`'s PCI matching
  when a vendor/device string is present — here it is not, so the ARC component is the source.
- Text-mode Setup loads it only for an ARC `ScsiAdapter` whose `Identifier` starts `NCRC8`
  (`[Map.SCSI]`). The veneer's `convert_SCSI_device` maps the Open Firmware `model`
  `NCR,53C810` to `NCRC810`; the ANS's `NCR,825A` becomes `UNKNOWN SCSI`. One veneer table
  entry changes that.
- The driver distinguishes chip variants at run time (`Ex_ChipID`, `Fast20_Support` in its
  `.text` string pool; a three-way switch on chip type at `0x11748`). Whether the 825A's
  extra registers (wide SCSI) confuse it is a phase 2 experiment: fix the identifier, let
  Setup load `symc810`, and watch the emulator's SCRIPTS engine.

The shipped HALs export **66** functions each (`HALEAGLE.DLL` table saved with the emulator
project); `SCSIPORT.SYS` imports the eight `Hal*` bus/DMA entries listed above — those plus
what `ntoskrnl` imports are the phase 1 contract.

## 6. Reproduction

```bash
# pre-go checkpoint per the charter §3.1; daemon on 6820
T=<emulator-project>/tools
python3 tools/gen-probe.py 5 powerstack 40 > tmp/probe-powerstack.gs   # 5 x Up, Enter
GS_PORT=6820 GS_IDLE=900 $T/gs 'include "tmp/probe-powerstack.gs"' > tmp/probe-powerstack.log
```

`gen-probe.py` loads the HAL-menu checkpoint (`tmp/nt-hal-menu.ckpt`, made by running the
charter's recipe from the pre-`go` checkpoint until `Select the computer type` appears, ~55 s
at turbo speed), verifies each key at `0x806040D8`, presses Enter, runs in 50 M-instruction
chunks until the firmware prompt, `EXIT called`, or silence, and saves a checkpoint of the end
state. It talks only to the emulator's TCP shell.

## Addendum — the `OFClose` fix, tested

With the two words patched before `go` (trace
[`…probe-closefix.txt`](../traces/2026-09-05-halmenu-probe-closefix.txt)):

- **Zero** `OFClose(…) failed` messages from `go` to the end of the run (twelve before).
- The media check after the NLS files passes. SETUPLDR goes on to load *Windows NT Setup*
  (`setupdd.sys`, two files), *PCMCIA Support* and *SCSI Port Driver*, and stops at Setup's
  mass-storage screen:

```
Setup could not determine the type of one or more mass storage devices installed in
your system, or you have chosen to manually specify an adapter.  Currently, Setup will
load support for the following mass storage devices(s):
    <none>
S=Specify Additional Device   ENTER=Continue   F3=Exit
```

`<none>` is expected: the veneer names both 53C825As `UNKNOWN SCSI`, which matches no
`[Map.SCSI]` entry (§5). The file offsets of the two patched words in `VENEER.EXE` are
`0x2454` and `0x2458` (bytes `08 00 43 91` → `00 00 20 39`, `00 00 20 39` → `08 00 23 91`);
together with the two CD-ROM bytes at `0xD2C0`/`0xE368` this is the complete veneer patch
set for a CD boot on this machine. The same leak would hit a real ANS 700, whose firmware is
the same 2.26NT.

Enter at the mass-storage screen (trace
[`…probe-massstorage-enter.txt`](../traces/2026-09-05-halmenu-probe-massstorage-enter.txt)):
*ESDI/IDE Hard Disk* (`atapi.sys`) and *Windows NT File System (NTFS)* load, then Setup's
**video-adapter menu**: E10 (S3 864 / 928 / 964), S10/S15/H10 (Weitek P9100), Standard VGA,
G10 (WD90C24A), *Motorola Power Stack (cirrus 54xx)*, FirePower Powerized, Other. The
54M30 is 54xx-class (charter §2), so the PowerStack entry is the one to try when the
`cirrus` miniport is next; for the phase 0 HAL probe any entry will do — the miniport is only
loaded into memory before the kernel starts.

## Addendum 2 — `symc810` is selected once the identifier fits (charter §9.2)

The veneer's `convert_name` (`VENEER.EXE` `0x565C8`, code at `0x56A1C`) knows three SCSI
models: `strcmp(model, "NCR,53C810")` → `NCRC810`, `"AMD 53C794"` → `AMD53C974`,
`strncmp(model, "ADPT,AIC-78")` → `AIC78XX`; anything else → `UNKNOWN SCSI`. The emulated
ANS's node reports (Open Firmware `.properties`, trace
[`…probe-ncrc810.txt`](../traces/2026-09-05-halmenu-probe-ncrc810.txt)):

```
/bandit/53c825@11: vendor-id 1000  device-id 3  revision-id 14  class-code 10000
  model "NCR,825A"  compatible "pci1000,3"  device_type scsi  AAPL,interrupts 16
  assigned-addresses: I/O 0x400 (0x100), MEM F3100000 (0x100), RAM F3101000 (0x1000)
```

Overwriting the 11-byte string `NCR,53C810\0` at image `0x5F420` with `NCR,825A\0` (nine
byte pokes, LE byte at A is at A^7) before `go` makes Setup's mass-storage screen read
**"Currently, Setup will load support for: Symbios Logic C810 PCI SCSI Host Adapter"**.
Everything else on the way was unchanged. So the miniport *will* be loaded and handed the
825A's ARC configuration data; whether it drives the chip is the next experiment, and it needs
a HAL that survives long enough for `scsiport` to start (Addendum 3).

## Addendum 3 — the kernel runs

Enter at the mass-storage screen, then *Motorola Power Stack (cirrus 54xx)* on the
video-adapter menu (traces [`…probe-video-cirrus.txt`](../traces/2026-09-05-halmenu-probe-video-cirrus.txt),
[`…probe-kernel-sample.txt`](../traces/2026-09-05-halmenu-probe-kernel-sample.txt)):

```
Setup is loading files (Motorola Power Stack (cirrus 54xx))...   cirrus.sys
Setup is loading files (Video Driver)...  (Floppy Disk Driver)...  (SCSI CD-ROM)... ×2
  (SCSI Disk)...  (SCSI Floppy Disk)...  (Keyboard Driver)... ×2  (FAT File System)...
  (CD-ROM File System)...
```

and then **SETUPLDR jumps into `NTKRNLMP.EXE`**. Three billion instructions later the console
is silent and the CPU is executing kernel code:

| what | value | meaning |
|---|---|---|
| `pc` | `0x800DBCE4` | `RtlCopyMemory32+0xF0` |
| `r2` | `0x80083340` | the kernel's TOC; file TOC is `0x80018340`, so the kernel was relocated by `+0x6B000` and runs at `0x8007B000` |
| `msr` | `0x1B031` / `0x13031` | LE, ILE, FP, ME, IR, DR; EE toggling — interrupts are being enabled and disabled |
| `bat0`/`dbat0` | `800000FE / 00000012` | 256 MB, `0x80000000` → physical 0: NT's KSEG0 |
| `dbat2` | `B08000FE / 8080003A` | `0xB0800000` → `0x80800000`, 256 MB, I/G: the PReP PCI configuration window |
| `dbat3` | `B10000FE / 8000003A` | `0xB1000000` → `0x80000000`: the PReP ISA / PCI I/O window |
| `sdr1` | `0x10000` | page table at physical `0x10000` |
| `srr0`, `dar`, `dsisr` | `0x800A8380`, `0x000B8007`, `0x42000000` | a kernel-mode store to `0xB8000` (the byte address munged by LE mode) took a translation fault |

Twelve samples 5 M instructions apart land in `KeBugCheckEx+0x74`, `KeBugCheckEx+0x2D4`,
`RtlLookupFunctionEntry`, `RtlVirtualUnwind`, `RtlCopyMemory32` and the kernel's exception
prologue (`mfcr r5; stw r6,-10780(0)` — state saved into the PCR at `0xFFFFD000`), with the
decrementer running. That is a bugcheck walking its own stack, repeatedly: the PowerStack HAL's
`HalDisplayString` writes the VGA text buffer at ISA `0xB8000`, an address that exists on a
PReP box behind its ISA memory window and nowhere on a Network Server, so the bugcheck's own
screen output faults and re-enters the bugcheck. This is the **first PReP-specific access
the ANS requirement list was waiting for**: the console. The bugcheck code and the HAL's
first fatal access are being captured with breakpoints on the DSI vector (`0x300`) and on
`KeBugCheckEx` (`0x800AB08C` relocated); see the next addendum.

Kernel export addresses relocate by `+0x6B000`: `KeBugCheckEx 0x800AB08C`, `KeBugCheck
0x800AAE5C`, `DbgPrint 0x800D5BA0`, `KeConnectInterrupt 0x800ABDC4`.

## Addendum 4 — the bugcheck, fully explained (traces [`…probe-bugcheck.txt`](../traces/2026-09-05-halmenu-probe-bugcheck.txt), [`…probe-align.txt`](../traces/2026-09-05-halmenu-probe-align.txt), [`…probe-slot-table.txt`](../traces/2026-09-05-halmenu-probe-slot-table.txt))

Breakpoints on the DSI vector (`0x300`), the alignment vector (`0x600`) and `KeBugCheckEx`:

1. **First DSI — benign.** `srr0 0x80139E74`, `dar 0xC0300C04`, `dsisr 0x40000000`: kernel
   INIT code (`0x801AD0E4` calls a helper with `r6 = 0xC0300C00`) reading the page-directory
   self-map entry — `0xC0300000` is NT's PDE base on PowerPC as on x86. The kernel's own DSI
   handler resolves it and execution continues. Expected on PPC NT, where TLB misses are
   software-handled.
2. **The HAL's PCI probe.** `HALEAGLE` is loaded at `0x80647000` (right after SETUPLDR's
   `MemoryLoadedProgram` range). Its routine at `.text+0x1AD44` maps `0x80800000` (the PReP
   PCI configuration window) — the mapping helper returns `0xB0800000`, via `dbat2` — then
   walks a seven-entry IDSEL table in `.data` (`0x2000, 0x4000, 0x10000, 0x20000, 0x40000,
   0x80000, 0x8000`) reading the vendor-ID halfword at `base + offset` and comparing with
   `0xFFFF`; on failure it prints *"Can't create mapping to PCI Configuration Space"*.
3. **First probe.** `lhz r11,0(r11)` at `0x80651DF8` with `r11 = 0xB0802000` — aligned. In
   the emulator that virtual address translates to physical `0x80802000`, Bandit PCI memory
   space with nothing there: the read returns `0xFFFF` **and** signals a bus error, which the
   604 model delivers as a **machine check** (`0x200`, SRR1 TEA bit) with `SRR0` = the `lhz`.
4. **NT's machine-check handler returns to SRR0**, re-executing the `lhz` — but `r11` now
   holds the `0xFFFF` the emulator already loaded, so the effective address is `0xFFFF`, odd,
   and in little-endian mode any misaligned access is an **alignment exception** (`0x600`,
   `dar 0xFFFF`, `dsisr 0x116B` = `rD 11, rA 11`). The kernel raises
   `STATUS_DATATYPE_MISALIGNMENT` for kernel mode → **bugcheck `0x1E`**
   (`KMODE_EXCEPTION_NOT_HANDLED`, p1 `0x80000002`, p2 `0x80651DF8`).
5. **The bugcheck cannot print.** `HalDisplayString` writes ISA VGA text memory at
   `0xB8000` (`dar 0xB8007`), unmapped and non-existent → DSI inside the bugcheck → the
   stack-walking loop the samples showed.

**What this says about the machine and the emulator.** On a real PowerStack the Eagle
bridge returns all-ones for an empty configuration slot without asserting TEA, so the stock
HAL never expects an exception there; on the Network Server the PReP window lands in
Bandit PCI memory space, where a master abort *does* raise a machine check (and the emulator
models that, on purpose, so firmware probes run under their fault catcher). The 604 manual
(§4.5.2, Table 4-8) makes the machine check imprecise — SRR0 is set "on a best-effort basis"
to "some instruction that was executing or about to be executing" — so the emulator's choice
of the faulting instruction is within spec; whether a real 604 also updates the load's target
register before the TEA machine check is not stated there and is left as an emulator-side
question. Nothing here needs fixing for the ANS HAL, because the ANS HAL will not do this.

**Requirement list for the ANS HAL, first entries (measured, not assumed):**

| # | Requirement | Evidence |
|---|---|---|
| R1 | PCI configuration access through **Bandit's configuration port** (`bridge + 0x800000`, IDSEL-encoded, two bridges), never by reading a memory-mapped PReP window. Empty IDSELs read all-ones with no error. | Addendum 4 steps 2–4; Bandit behaviour as modelled in the emulator's PCI layer |
| R2 | **Console output that exists**: `HalDisplayString` to ttya (the veneer's `CONSOLEOUT` is `multi(0)serial(0)`) or to the 54M30 framebuffer window, never ISA `0xB8000`. Without it every bugcheck is silent and recursive. | `dar 0xB8007` inside the bugcheck |
| R3 | The veneer's loader block is **good enough for the kernel**: memory descriptors, the ARC tree and the LE environment carried NT through its early initialisation (BAT0 KSEG0 set up, page tables at `0x10000`, PCR at `0xFFFFD000`, interrupts enabled and disabled) for ~3 G instructions before the HAL's first PReP-specific access. | Addendum 3 samples |
| R4 | The HAL must expect to be loaded **little-endian and relocated**: kernel at `0x8007B000` (file base `0x80010000`), HAL at `0x80647000`; SETUPLDR applies `.reloc`. | Addenda 3–4 |

Next probes that need no HAL code: the same run under `debug.logpoints` on the GC and Bandit
register ranges to list every device register the stock HAL touches before the probe; the
other six HALs for comparison. Next step that needs code: a HAL whose `HalInitSystem` does R1
and R2 and nothing else, to see how far the kernel gets when the console works.
