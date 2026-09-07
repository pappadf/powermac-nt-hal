# The first HAL boots — what the loader and the kernel demanded (2026-09-06)

*Emulator Granny Smith `8ebed27`, ANS 500 profile, 64 MB, ROM 2.26NT, NT 4.0 Workstation OEM
CD 000-48303 with `\PPC\HALEAGLE.DLL` replaced by this project's `hal.dll` (build 7 onward).
Console = ttya. All facts from the shipped `SETUPLDR` (its own symbol table) and from watching
the kernel run; no Microsoft source was consulted.*

## Result

Build 7 of `halshinr` (about 900 lines of C and assembly, built with clang + lld +
`tools/elf2pe.py`, no Microsoft tool) was loaded by SETUPLDR, bound to `ntoskrnl.exe`, and
the kernel ran on it:

```
HAL: halshinr build 7 (HALSHINR-BUILD7-MARKER) for the Apple Network Server (phase 0)
HAL: I/O base b0000000, PVR 00040103, MSR 00013031, PCR 8000e000, loader block 807f0000
HAL: GC events 00001000 mask 00100000 levels 00000000
HAL: boot device multi(0)scsi(0)cdrom(0)fdisk(0)
HAL: hal path (null), options /NODEBUG
HAL: 52 memory descriptors, 16384 pages; PCR irql 1, kseg0 top 80800000
HAL: phase 0 done
Microsoft (R) Windows NT (TM) Version 4.0 (Build 1381).
HAL: phase 1
1 System Processor [64 MB Memory] MultiProcessor Kernel
```

That banner is the kernel's own first console output — the charter's phase 1 definition of
done. The decrementer clock, IRQL raising and lowering, the descriptor thunks and the ttya
console all worked on the first run that got this far. What follows is the list of things
that had to be exactly right before that, in the order they bit.

## The PE the NT PowerPC boot loader accepts

| # | Requirement | How it showed |
|---|---|---|
| 1 | The **IAT must be pre-filled with the hint/name RVAs** (a copy of the ILT). SETUPLDR's `BlpScanImportAddressTable` walks `FirstThunk` and stops at the first zero word; `OriginalFirstThunk` is never consulted. | Import slots stayed zero, the first kernel call jumped through NULL, bugcheck `0x1E` with `STATUS_BREAKPOINT` (`DbgBreakPointWithStatus`). |
| 2 | Every **hint/name entry must be 2-byte aligned**: the binder reads the hint with `lhz`, and in little-endian mode a misaligned halfword is an alignment exception. | Firmware `DEFAULT CATCH!, code=FFF00600 at SRR0: 8060B9D0` while *Setup is loading files (MOTOROLA PowerStack)*. |
| 3 | The **IAT needs its terminating zero word** inside the section; the word after the last slot was the emulator's RAM fill pattern and became a "hint address". | `DEFAULT CATCH!, code=FFF00300` at the same instruction. |
| 4 | Exports point at **function descriptors** `{entry, toc}`; the DLL entry point is the `HalInitSystem` descriptor; names are sorted for the loader's binary search. | (as designed; the kernel's IAT for HAL bound to our descriptors on the first try) |
| 5 | Base relocations: ELF `ADDR32`→HIGHLOW, `ADDR16_LO`→LOW, `ADDR16_HA`→HIGHADJ with the low half as the extra word, `ADDR16_HI`→HIGH. The loader relocated the image from `0x80010000` to `0x80647000` correctly. | code and data verified against the file after loading |
| 6 | The PE checksum is verified; compute it. | (never failed, computed from the start) |

## The ABI the kernel speaks

| # | Requirement | How it showed |
|---|---|---|
| 7 | Calls into the kernel go through a **64-byte frame**: an NT callee may write the caller's frame at offsets 4, 8 and 24..55 (glue slots and parameter home area). Our SVR4 code never sees that frame. | (as designed) |
| 8 | `LARGE_INTEGER`/`PHYSICAL_ADDRESS` are passed and returned **by value in a register pair**, low word first, aligned to an odd register (`MmAllocateContiguousMemory(size, addr)` has the address in r5:r6). SVR4 passes a *union* by pointer, so these types are 64-bit scalars in `nt.h`. | Bugcheck `0x1E` in `HalTranslateBusAddress`: the "AddressSpace pointer" was the high word of the bus address (`0x00000004`). |
| 9 | The kernel's vector routines (`PCR->InterruptRoutine[...]`) are descriptors called with the trap frame as third argument; a connected interrupt object's routine lives at `+0x3C` of the `KINTERRUPT`, its context at `+0x10`. | (from the stock HAL and halartx; decrementer clock ran) |
| 10 | The kernel's `KePhase0MapIo` hands out at most three 8 MB BAT slots; this HAL programs DBAT3 itself: 256 MB at `0xB0000000` → `0xF0000000`, cache-inhibited, guarded. | first build got NULL back from a 256 MB request |

## The machine after the banner

- **Legacy ISA probes.** The first boot driver to run, `pcmcia.sys`, asked
  `HalTranslateBusAddress(Isa, port 0x3E0)`; translated into Bandit 1's PCI I/O window that is a
  master abort → TEA → machine check the kernel does not survive (`pc = 0x200`, MSR `0x10001`).
  Build 9 refuses to translate legacy I/O ports except VGA's `0x3B0..0x3DF`, and legacy memory
  below `0x80000000`. Build 10 adds a machine-check handler that completes an aborted load
  with all-ones and steps over it (volatile targets only), so a probe that slips through
  behaves as on PReP hardware.
- **`i8042prt.sys`** (the veneer publishes an i8042 `KeyboardController`) dereferences NULL+0xF8
  when the port cannot be translated: bugcheck `0x1E` at `i8042prt.sys+0x1D28`. The Network
  Server has an ADB keyboard behind Cuda; the driver must not load. Setup reads `[files.i8042]`
  from `TXTSETUP.SIF` before the computer-type menu, so the test CD carries a same-length edit
  (`i8042prt.sys,4` → `kbdclass.sys,4`) and a new menu checkpoint was needed.

## Emulator facts that cost time

- A consolidated `checkpoint.save` **snapshots every attached image**; `checkpoint.load` fills
  the read-only CD's scratch delta with all blocks flagged, so the guest reads the CD as it was
  at save time whatever the ISO file now holds. `tools/run-hal.py --delta-patch` writes the
  new HAL (and the edited `TXTSETUP.SIF`) into the restored delta after loading. Anything
  SETUPLDR had already read (the INF text) is in RAM and needs a fresh cold boot.
- The daemon's `machine.scsi` object is shadowed by a same-named static helper: the primary
  bus's device objects (eject/insert) are unreachable, so the medium cannot be swapped live.
- Console keys: one byte per `machine.scc.a.receive`.
- A full disk (eight 725 MB checkpoints) makes the shell client exit silently with code 120.

## Build 12: through driver initialisation to the boot device

With the i8042 driver out of the load list (both `TXTSETUP.SIF` references renamed), the
machine-check handler in place, and legacy port translation refused, the kernel initialises
every boot driver Setup loaded — video, floppy, SCSI class drivers, keyboard class, FAT, CDFS,
`setupdd` — and stops at

```
*** STOP: 0x0000007B (0xE6E84A70,0xC0000034,0x00000000,0x00000000)
INACCESSIBLE_BOOT_DEVICE
```

`0xC0000034` is *object name not found*: there is no device object for the boot CD, because
Setup detected no mass-storage adapter (the veneer calls the 53C825As `UNKNOWN SCSI`). The next
checkpoint renames the veneer's model string so Setup loads `symc810`, which exercises the
HAL's PCI config access, bus-address translation, adapter object and Grand Central interrupt
dispatch for the first time.

## Builds 12-18: through driver init to the boot device, and the SCSI wall

With the machine-check handler (completes an aborted PCI probe with all-ones), legacy-port
translation refused, the i8042 keyboard driver kept out of the load list, and the veneer's
SCSI model string renamed so Setup loads `symc810`, the kernel initialises every boot driver
and stops at:

```
*** STOP: 0x0000007B (0xE6E84A70,0xC0000034,0x00000000,0x00000000)
INACCESSIBLE_BOOT_DEVICE
```

`0xC0000034` is *object name not found*: no device object exists for the boot CD, because the
SCSI miniport never attached. Breakpoints in `symc810.sys` and `scsiport.sys` (addresses from
their export tables plus the loaded module list the HAL prints in phase 1) show the mechanism
exactly:

- `symc810`'s `DriverEntry` runs and calls `ScsiPortInitialize` three times, once per NCR chip
  variant it supports.
- Each call walks the ARC configuration tree (`IoQueryDeviceDescription`) — the HAL's repaired
  `ScsiAdapter` resource lists are read — and **returns `0xC00000C0`
  (`STATUS_DEVICE_DOES_NOT_EXIST`) without ever calling the miniport's `HwFindAdapter`**
  (breakpoint at its entry, resolved from its `HW_INITIALIZATION_DATA` descriptor, never fires).
- Reporting the 53C810's PCI device id `0x0001` for the two 53C825As (device `0x0003`) from the
  HAL's config-space reads did **not** change this, so the gate is not the PCI device-id match;
  `ScsiPortInitialize` rejects the adapter earlier, in its registry/boot-config path.

**What the HAL proved along the way.** By this point the HAL's PCI configuration access reads
every device on both Bandit bridges correctly (vendor `0x1000` SCSI at IDSEL 17/18, the Cirrus
at 15, Grand Central at 16, both Bandits, the second bus), `HalGetInterruptVector` and
`HalTranslateBusAddress` answer for the internal bus, `HalGetAdapter` hands out master adapters,
the machine-check path absorbs stray probes, and the ARC-tree repair gives the miniport a
usable resource list. The remaining wall is NT's binding of the shipped 810 miniport to the
825A — the charter's Phase 2 (a config/registry change to `TXTSETUP`'s adapter detection, or a
purpose-built 825A miniport), not a HAL requirement.

## Standing HAL requirements met (phase 1 complete)

`HalInitSystem` (both phases), `HalInitializeProcessor`, `HalDisplayString` on ttya,
`KeStallExecutionProcessor` and the decrementer clock, `KeRaiseIrql`/`KeLowerIrql` with Grand
Central masking, the external-interrupt dispatcher, the machine-check handler, `HalGetBusData*`
and `HalSetBusData*` through Bandit config ports, `HalTranslateBusAddress`,
`HalGetInterruptVector`, `HalGetAdapter` and the DMA/common-buffer helpers, `HalQueryRealTimeClock`
(fixed value pending Cuda), cache sweeps, `KeQueryPerformanceCounter`, `HalReturnToFirmware`.
Console output survives a bugcheck because it is serial, not ISA VGA. The kernel reaches its
own banner and driver-init on this HAL.

## The Cirrus 54M30 as the console (Goal A, done 2026-09-06)

`HalDisplayString` — the path text-mode Setup and the bugcheck screen both use — now renders on
the on-board Cirrus 54M30, not only ttya. Screenshot:
[`../traces/2026-09-06-cirrus-console-bugcheck.png`](../traces/2026-09-06-cirrus-console-bugcheck.png)
shows the kernel banner and the INACCESSIBLE_BOOT_DEVICE screen white-on-blue on the monitor.

How it works ([`../src/vga.c`](../src/vga.c)):

- The firmware leaves the console on ttya and never programs the chip, so the HAL does a
  from-scratch mode-set into **640x480 8bpp linear** through the legacy VGA registers: unlock the
  Cirrus extensions (SR06), 8 dots/clock (SR01), packed 8bpp (SR07), chain-4 (SR04), 256-colour
  shift (GR05), and the CRTC timing the display scans from (CR01 width, CR07/CR12 height, CR13
  stride, CR0C/0D/1B start). Only the registers the emulator's model derives the mode from need
  be exact, which made the values computable rather than guessed.
- The VGA ports are a fixed PCI-I/O region at 0x3B0-0x3DF; on Bandit 1 that is CPU-physical
  `0xF2000000 + port`, reached through the device BAT the HAL already owns.
- The framebuffer is BAR0 (OF-assigned PCI memory `0x81000000`, in Bandit's 256 MB window at
  `0x80000000`), mapped with `MmMapIoSpace` in phase 1 — `KePhase0MapIo` hangs for this address,
  so the console is brought up in phase 1, which is when Setup needs it.
- A two-entry palette (index 0 blue paper, index 1 white ink) via the DAC ports; glyphs blitted
  from the OEM font `LOADER_PARAMETER_BLOCK.OemFontFile` (here 8x12, 80x40 characters), using the
  NT font's `Map[]` offsets and column-major row bytes.
- `HalDisplayString` writes ttya and the framebuffer; `HalQueryDisplayParameters` reports the
  80x40 grid so `setupdd` lays out its UI correctly.

Effort: about half a day, one new file (~130 lines). It is independent of the SCSI wall — the
kernel banner and bugcheck already prove the path, and Setup's blue text UI will render here once
the boot device is reachable.

## SCSI: the stock miniport now binds (2026-09-06, later)

Chasing why NT could not open the boot device turned up two HAL bugs, not a driver problem —
exactly as the external advice predicted ("if the HAL is correct, the stock 53C8xx driver
should just work"). The 53C825A is driven by the CD's `symc810.sys`, which is the PowerPC name
for the NCR 53C8xx family miniport (NT's `ncrc810` is not shipped on PPC media — verified against
the OEM CD and NT 4.0 SP2 for PowerPC, neither carries it; `symc810` is that driver).

`scsiport.sys` finds SCSI adapters through the ARC `ScsiAdapter` tree and, for a PCI miniport,
calls `HalAssignSlotResources` to obtain the controller's resources. Two defects blocked it:

1. **`HalAssignSlotResources` was a stub** returning `STATUS_NOT_SUPPORTED`. With no resources,
   `ScsiPortInitialize` returned `STATUS_DEVICE_DOES_NOT_EXIST` before ever calling the miniport's
   `HwFindAdapter`, and I/O init then bugchecked `0x7B INACCESSIBLE_BOOT_DEVICE`. Implemented it:
   it sizes the device's BARs from config space, adds the ANS interrupt line from the routing
   table, and returns a `CM_RESOURCE_LIST`.
2. **`CM_PARTIAL_RESOURCE_DESCRIPTOR` was mis-packed.** NT packs it to 4 bytes (16 bytes each);
   an 8-byte `PHYSICAL_ADDRESS` made the compiler pad it to 24, so `scsiport` walked the list at
   the wrong offsets and dereferenced a garbage pointer — bugcheck `0x50 PAGE_FAULT_IN_NONPAGED_AREA`.
   Packing the CM structs to 4 (with a `_Static_assert` on the 16-byte size) fixed it.

With both fixed, `scsiport` accepts the adapter and **calls `symc810`'s `HwFindAdapter`**; the
miniport then requests a bus-master scatter-gather DMA adapter (`HalGetAdapter`), gets its
interrupt vector (`HalGetInterruptVector`, GC bit 22 → vector 54) and connects and enables it
(`HalEnableSystemInterrupt`). No bugcheck. The kernel prints its banner
([`../traces/2026-09-06-cirrus-kernel-banner.png`](../traces/2026-09-06-cirrus-kernel-banner.png))
and then idles in a spinlock.

**Current wall:** `HalpExternalInterrupt` never fires in the whole run, so the SCSI completion
interrupt (GC EXT2, bit 22) never reaches the HAL and the miniport waits forever. The decrementer
interrupt works (the kernel scheduled and initialised), so the fault is specific to the external
interrupt path: Grand Central latching the 53C825A's line and asserting the 604's external input,
and the HAL unmasking bit 22 correctly. That is the next item — a HAL/GC interrupt-delivery
problem, no longer a "can NT see the disk" problem.
