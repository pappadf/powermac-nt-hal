# From INACCESSIBLE_BOOT_DEVICE to Setup's keyboard check

*2026-09-06, second session. Builds 19-28 of `halshinr`.*

Four walls fell in this order, and each one was a different kind of thing: a missing HAL
export, a mis-declared ABI, a veneer bug, and three registers the emulated Cirrus did not
have. Setup now reaches its keyboard check, drawn on the monitor.

---

## 1. `HalAllocateAdapterChannel` — why the miniport was never asked to do anything

The symptom from the previous session: NT bound `symc810.sys` to the 53C825A, connected its
interrupt, and then sat there. `HwStartIo` was never called once, the chip had nothing to do,
and so it never interrupted — the "interrupt that never arrives" was an interrupt that had no
reason to exist.

Reading `ScsiPortStartIo` (SCSIPORT.SYS image VA `0x129c8`) settles where the request stops:

```
00012bd4  lbz   r11,369(r31)       ; deviceExtension->MasterWithAdapter
00012bdc  beq   cr1,0x00012ca4     ; no adapter object: start directly
...
00012c0c  lwz   r28,-32720(r2)     ; ntoskrnl!IoAllocateAdapterChannel
00012c30  blrl
00012c38  cmpwi cr1,r28,0x0
00012c40  bge   cr1,0x00012d80     ; SUCCESS: return, the callback will start the I/O
00012c58  bl    ScsiPortNotification(RequestComplete)   ; failure: complete with an error
```

A bus master that has a DMA adapter object goes through `IoAllocateAdapterChannel`, and the
miniport is started from the adapter-control callback, not from `ScsiPortStartIo`. The
kernel's `IoAllocateAdapterChannel` is a five-line wrapper (`ntoskrnl` image VA `0x368a4`):

```
800368b8  addi  r11,r4,0x34        ; &DeviceObject->Queue.Wcb
800368bc  stw   r4,28(r11)         ;   Wcb->DeviceObject = DeviceObject
800368c0  lwz   r4,20(r4)          ;   DeviceObject->CurrentIrp
800368c8  stw   r4,32(r11)         ;   Wcb->CurrentIrp
800368d0  stw   r7,20(r11)         ;   Wcb->DeviceContext = Context
800368c4  lwz   r31,[toc] -> HAL.dll!HalAllocateAdapterChannel
```

— it forwards straight to the HAL. Ours returned `STATUS_INSUFFICIENT_RESOURCES`.

That fixes the WAIT_CONTEXT_BLOCK layout for us as well, since the wrapper writes it:
`WaitQueueEntry[16]`, `DeviceRoutine` at 16, `DeviceContext` at 20, `NumberOfMapRegisters` at
24, `DeviceObject` at 28, `CurrentIrp` at 32, `BufferChainingDpc` at 36.

On this machine a PCI bus master addresses memory directly, so there is nothing to hand out
and nothing that can be contended for: `source/dma.c` grants every request immediately, in the
caller's context, and calls the driver's execution routine through the descriptor
(`HalpCallDesc4`, new in `thunk.S`).

## 2. An eight-byte return value comes back through a hidden pointer

With the adapter channel granted, the next stop was a bugcheck `0x0A` at `IoMapTransfer+4`,
reading address `0x14` — a null MDL. The caller says why (SCSIPORT image VA `0x14360`):

```
00014394  addi  r8,r1,0x60
00014398  ori   r3,r8,0x0          ; r3 = a stack slot
000143ac  li    r4,0x0             ; r4 = AdapterObject
...
000143c0  blrl                     ; IoMapTransfer
000143c4  lwz   r11,104(r1)        ; and the result is READ OUT OF THE SLOT
000143cc  lwz   r11,0(r11)
```

Microsoft's PowerPC compiler returns every struct through a hidden first argument in `r3`, and
`LARGE_INTEGER` is a union — eight bytes or not. The real arguments start at `r4`.
`MmGetPhysicalAddress` in the kernel confirms it from the other side: its first instruction is
`stw r4,-24(r1)`, storing the *VirtualAddress* it was passed in `r4`.

This is the opposite of how these types are *passed* (a register pair, as
`HalTranslateBusAddress` needed in the previous session), so `inc/nt.h` now states both rules
next to each other. Three places changed: `IoMapTransfer` and `KeQueryPerformanceCounter` take
the out-pointer first and return it; the call to `MmGetPhysicalAddress` passes one.
`KeQueryPerformanceCounter` had been silently returning the frequency as the counter.

With those two fixes the SCSI stack works end to end: the bus is scanned, `INQUIRY` reaches
every target and LUN, the CD-ROM answers, and `cdrom.sys` reads the ISO volume descriptor.
The bugcheck moved from `0x7B` to a *different* `0x7B`.

## 3. The veneer reads the CD's root directory when asked for a raw sector

`STOP 0x0000007B (…, 0xC0000034, …)` is `IoInitSystem` failing to open
`\ArcName\multi(0)scsi(0)cdrom(0)fdisk(0)`. That name is created only if a check passes
(`ntoskrnl` image VA `0x139ec0`-`0x13a0c0`): for each `\Device\CdRom%d`, read 2048 bytes at
byte offset `0x8000` (ISO sector 16, the Primary Volume Descriptor), sum the 512 words, and
require the sum plus the checksum the *loader* recorded to be zero.

The HAL can see the loader's side, because `LoaderBlock->ArcDiskInformation` is a list of
`ARC_DISK_SIGNATURE` — `{ LIST_ENTRY, Signature, PCHAR ArcName, CheckSum, ValidPartitionTable }`
(`ArcName` at offset 12, `CheckSum` at 16; the kernel's own compare fixes the order).
Printing it:

```
HAL: arc disk sig 00000000 sum d5d64a58 valid 0 'multi(0)scsi(0)cdrom(0)fdisk(0)'
HAL: arc disk sig 00000000 sum 29f28bb3 valid 0 'multi(0)scsi(1)disk(0)rdisk(0)'
```

The kernel computed `0x2d88048b` for that CD, which is exactly the sum of sector 16 of the ISO
— our DMA path is byte-perfect. `0xd5d64a58` is not its negation, and no 2048-byte window
anywhere in the 606 MB image negates to it. So SETUPLDR checksummed something else.

`BlReadSignature` (SETUPLDR image VA `0x609fc8`) opens `<path>partition(0)`, seeks to
`0x8000`, reads `0x800`. Breaking on it live:

| | requested | `ArcRead` returned | first words |
|---|---|---|---|
| `…scsi(1)disk(0)rdisk(0)partition(0)` | 512 @ 0 | **512** | `"GS NT VENEER STAGING DISK v1…"` |
| `…scsi(0)cdrom(0)fdisk(0)partition(0)` | 2048 @ 0x8000 | **428** | an ISO directory record for `HP35036.PC_` |

428 is `0x1AC`, and the ISO's root directory is extent 29, **size 428**. Seeking `0x8000` into
a thing based at LBA 29 lands on LBA 45, which is where those bytes live. The veneer had
opened the CD's *root directory as a file* instead of the raw device.

Why is already in this repository's notes: "Apple's `disk-label` gives raw sector access only
for an *empty* Open Firmware argument; the veneer's `VrOpen` appends `:0`." That was patched
for the no-partition case by blanking the `:0` string at `0x5E168`. But `VrOpen` has a second
path — image VA `0x54744` — taken whenever an ARC `partition(N)` component is present, and it
builds `":" + digits` of its own, which `disk-label` again reads as a filesystem open:

```
00054744  cmplwi cr1,r27,0x0     ; a partition component?
00054748  bne    cr1,0x00054784  ; -> ":" + digits           <-- patched to nop
0005474c  cmplwi cr1,r30,0x0     ; a file path?
00054750  bne    cr1,0x00054784
00054754  ...                    ; raw: append the (blanked) ":0", flag $10
```

Turning the first branch into a `nop` sends `partition(N)` **with no file path** down the raw
route, and leaves `partition(N)\path\file` alone. One word, in the same family as the two
byte patches already in the boot recipe:

```
machine.memory.poke.l 0x5474c 0x60000000   # veneer VrOpen: partition(N) with no file is raw
```

(`0x5474c` is `0x54748 ^ 4`: the little-endian word munge, as with the existing pokes.)

With it, `BlReadSignature` reads the real 2048 bytes of the PVD, records `0xd277fb75`, the
kernel's sum matches, `\ArcName\…` is created, and **INACCESSIBLE_BOOT_DEVICE is gone**.

## 4. Three registers the emulated Cirrus did not implement

Setup then died with *"a fatal error while initializing your computer's video (0, 0xc0000034)"*
— `setupdd.sys` could not open `\Device\Video0`, because `videoprt.sys` never created it,
because `cirrus.sys`'s `HwFindAdapter` failed. Three separate reasons, found by breaking on its
return sites (cirrus image VAs `0x10a38`, `0x10a6c`, `0x10a8c`, `0x10ab8`, `0x10b20`):

1. **`CR27`, the chip ID.** `cirrus.sys` unlocks the extensions, reads `CR27`, and requires
   `CR27 >> 2` to be in `[0x0B, 0x2F]`. The model had a 64-byte RAM array, so `CR27` read 0.
   `0xA0` is the CL-GD5430 — the die this card carries, and the value that agrees with its
   `0x00A0` PCI device ID.
2. **`SR06`, "Unlock ALL Extensions".** The same routine requires the register to read back
   `0x12` after unlocking and `0x0F` after locking. RAM reads back whatever was written.
3. **`SR15`, "DRAM Control", bits 3:0.** The driver *sizes the framebuffer from this field* and
   rejects every mode larger than what it reports; zero meant zero modes, and no modes means
   `ERROR_INVALID_PARAMETER`. `2` is 1 MB, which is what the board fits.

Two more things were missing on the way: the **Miscellaneous Output** register had no read
address (`$3CC`), so a driver asking which CRTC pair to use got 0 and went to the monochrome
addresses `$3B4/$3B5`, which decoded nothing. The model now answers `$3CC` with what `$3C2` was
given and folds the monochrome pair onto the colour one, and this HAL's own mode-set writes
`$3C2 = 0xE3` like every VGA mode-set does.

All five are in `src/core/peripherals/pci/cards/cirrus54m30.c`; the `ans-console`,
`ans-device-tree`, `ans-pci-slots` and `tnt-pci-slots` integration tests still pass.

## 5. Where it stops now

`cirrus.sys` initialises, `\Device\Video0` exists, Setup switches the console to the monitor,
and the next screen is:

> **Setup did not find a keyboard connected to your computer.**

([`traces/2026-09-06-setup-keyboard-check.png`](../traces/2026-09-06-setup-keyboard-check.png),
serial log alongside it. The grey status bar with black text is the giveaway that this is
Setup drawing through the *video driver* and not the HAL's two-colour console: our palette has
exactly two entries.)

which is true. The Network Server's keyboard is ADB, `i8042prt.sys` probes port `0x60` (the
HAL correctly refuses to translate it — there is no 8042 on this machine), and NT 4.0 ships no
ADB keyboard port driver. That is the next wall, and it is a driver, not a HAL.

## 6. Still outstanding, in the order they will matter

- **`VideoPortVerifyAccessRanges` returns `ERROR_INVALID_PARAMETER`**, and everything in §4 was
  measured with it bypassed by a diagnostic poke (`0x70333c = 0x38600000`, videoprt image VA
  `0x13338`). `IoReportResourceUsage` succeeds and sets the *conflict* flag for the four
  ranges `cirrus.sys` claims: I/O `0x3B0+0xC`, I/O `0x3C0+0x20`, memory `0xA0000+0x20000`,
  memory `0x81000000+0x1000000`.

  It is the third one. Poking the range's start to `0x70000000` at the
  `VideoPortVerifyAccessRanges` breakpoint and leaving everything else alone makes the call
  return success — so the collision is the **legacy VGA aperture at `0xA0000`, which on this
  machine is ordinary RAM**. A PCI VGA part does decode that aperture on the bus, but Bandit
  does not forward CPU accesses there (the HAL's `HalTranslateBusAddress` already says so for
  the ISA case), and the kernel's conflict scan works on the *raw*, untranslated list.

  What it collides with is **the machine's RAM**. Moving the range to `0x03000000`, still
  inside the 64 MB, conflicts again; `0x70000000`, outside it, does not. That is
  `System Resources\Physical Memory`, which the kernel builds from the loader's memory
  descriptors (INIT image VAs `0x138dd0`/`0x138e34`).

  There is no HAL lever for it. `HalReportResourceUsage` is empty here, so nothing we report
  can be the cause; `HalAdjustResourceList` — the one place the HAL gets to edit a resource
  list — is called only from `IoAssignResources` (PAGE image VA `0xbb0f4`), not from the
  report path. The claim is simply not satisfiable on this machine: a PCI VGA part decodes
  `0xA0000` on the *bus*, `cirrus.sys` asks for it as *system-physical* memory, and here that
  address is RAM. The HAL's `HalTranslateBusAddress` now refuses PCI memory below `0x80000000`
  for the same reason — handing the driver back an identity translation of `0xA0000` would
  have let it write over the kernel.

  So this one is a shipped-driver/machine mismatch rather than a bug to fix, and it needs
  either a driver that does not claim the aperture or a way to override the conflict. Until
  then the poke stands, and it is the only patch in the loop that is not defensible as a fix.
- **`IoAssignDriveLetters` is a stub.** Setup calls it; nothing has failed for want of it yet.
- **`HalAssignSlotResources` reports no interrupt for the Cirrus** (2 resources, both memory
  BAR-derived). Correct for this card — Apple states the part has no interrupt line — but
  worth stating rather than leaving as an accident.

## 7. Tooling added

- `tools/run-hal.py`: `--poke addr=value` (after the checkpoint load), `--onbreak <statement>`,
  `--peek r1+0x9bc` (register-relative), `--deref r1+2472` (follow a pointer and dump 12
  words). Peek addresses are masked with `0x7fffffff` and XORed with 4, which covers both
  KSEG0 and the low physical addresses SETUPLDR's stack lives at.
- `run-hal.py --screenshot` had been emitting `machine.screen.checksum` as an attribute; the
  shell wants the call form, and the error aborted the script — taking `--save` with it, since
  the save line comes after. Both work now.
- `tools/restamp-ckpt.py`: rewrites the 20-byte build ID at offset 8 of a checkpoint. The
  emulator refuses a checkpoint written by a different build, and each NT boot checkpoint
  costs ten minutes; when the rebuild changed no checkpointed structure this is safe, and it
  is the difference between iterating in ten minutes and iterating in twenty.
