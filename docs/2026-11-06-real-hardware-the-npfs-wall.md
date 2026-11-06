# Real hardware: the NPFS wall, read from the emulator side

*6 November 2026.  A desk study of Mr. Macintosh's physical ANS 700 work
([MrMacintoshBlog/powermac-nt-hal](https://github.com/MrMacintoshBlog/powermac-nt-hal),
`docs/ANS-HANDOFF-2026-11-05.md` and the files it links), compared with what this
project runs in the emulator every day.  Nothing here was run on hardware; every claim
names the capture, binary or manual it comes from, and the conclusions are a theory with
an experiment attached, not a fix.*

## 1. Where the physical machine is

Text-mode Setup completed on a real Network Server 700 on 18 September.  The installed
system has booted many times since and stops at the same place every time:

```
STOP: c000026c {Unable to Load Device Driver}
\SystemRoot\System32\Drivers\Npfs.SYS device driver could not be loaded.
Error Status was 0xc0000185
```

Earlier builds stopped one driver earlier, at `Beep.SYS`, with `C0000221`
(`STATUS_IMAGE_CHECKSUM_MISMATCH`), and one diagnostic build caught the moment itself:
`VAL1 exception 1 code c0000006 ... info 00000000 00040000 c0000185` -- an in-page
error raised by the checksum loop, whose underlying I/O status is again `C0000185`.

`C0000185` is `STATUS_IO_DEVICE_ERROR`.  It is what `SCSIPORT.SYS` hands up for any
SRB status it has no better translation for, and `SRB_STATUS_BUS_RESET` (`0x0E`) is one
of them.  Three different symptoms, one status: **a paging read of the driver image was
completed as failed by the SCSI stack**, and the loader reported that failure in
whichever way the stage it was at reports it.  The HAL is not corrupting the bytes; the
read never delivered them.  This is also why the planned next step in the handoff --
instrumenting the kernel's image loader to record which stage returns the error -- would
mostly confirm what is already in the capture: the stage is the page read, and its
status is `C0000185`.

The whole-file firmware checks in the handoff are consistent with that: `npfs.sys`
on the disk is byte-identical to the CD's.  Open Firmware reads it fine.  NT's own
read path does not.

## 2. What SCSIRING2 actually recorded

The HALIM2 build hooks the miniport's two `SCSIPORT` imports, `ScsiPortNotification`
(ring `kind 0`) and `ScsiPortCompleteRequest` (`kind 1`).  Decoding the retained ring
with the installed `symc810.sys` in hand (taken from this project's own installed-disk
test image; PE timestamp `31ed22e3`, image size `79e0` -- the same file the physical
machine runs, per the fingerprint HALIM2 checks before hooking):

| Ring field | Meaning |
|---|---|
| `kind 0`, `ids 0000000a`, `status 00010028`, detail `lba count bytes` | `RequestComplete` for a READ(10) on path 0 / target 0 / LUN 0, `SrbStatus 01` = success |
| `kind 0`, `ids 00000006`, `status 00010000` on the *other* adapter | a six-byte TEST UNIT READY on the CD-ROM controller (the class driver's media poll), success |
| `kind 1`, `ids 00ffff00`, `status 0000000e`, detail `8007d62c 80081874` | `ScsiPortCompleteRequest(ext, path 0, target 0xFF, LUN 0xFF, SRB_STATUS_BUS_RESET)`: *complete every outstanding request on this adapter as bus-reset*; called from image offset `0x62c`, with `r28` = `0x4874` |

So after almost every successful read on the disk controller, the miniport tells
SCSIPORT that **everything in flight on that bus has been killed by a bus reset**.
Where `0x62c` and `0x4874` fall in `symc810.sys` says who:

- `0x05c0` is the miniport's *complete-all-with-bus-reset* helper.  It saves LR in
  `r28` (that is the `0x4874` in the ring), calls `ScsiPortCompleteRequest` at `0x628`
  with `0x0E`, and resets the per-LUN state.
- `0x4874` is the return address inside `0x476c`, the miniport's **own SCSI bus reset
  routine**: read `SCNTL1`, write it with bit 3 (`RST`) set, `ScsiPortStallExecution(30)`,
  clear it, then call `0x05c0`.  It also sets flag `0x0002` in the device extension so
  that the `RST` interrupt the chip raises for its own reset is swallowed
  (`0x2518`: `ScsiPortNotification(ResetDetected)`, then "if flag 2, clear it and
  return; else complete-all").

Every `kind 1` record in the ring is therefore the driver **resetting the SCSI bus
itself**.  It was not the drive, not the backplane's hot-swap reset logic
(Apple, *Network Server Hardware Developer Notes* §7.2.2, which does assert `RST` for
5 ms on a carrier insertion), and not SCSIPORT's timeout path -- that one enters
through `HwResetBus` (`0x1f9c`: `ISTAT = ABRT`, poll, re-init, then `0x476c`), whose
return address would be `0x217c`, not what the ring shows.

Which ISR paths call `0x476c`?  Fifteen sites.  The two that matter are the two the
October progress file already recorded without knowing what they were:

> "Seven records at native origin `0x3ACC` contain cached SIST0 `0x90`; four at
> `0x281C` contain cached DSPS low byte `0x08`."

Both are return addresses of `bl 0x476c`:

| Return address | Caller | Condition that gets there |
|---|---|---|
| `0x3acc` | `0x3a84`, the SCSI-interrupt dispatcher (`SIST0` in `r4`) | not `RST`, and **the extension's active-request pointer (`+0x88`) is NULL** -- "a SCSI interrupt with no request on the bus" -- re-initialise the chip and reset the bus |
| `0x281c` | `0x27f8`, a plain "re-init chip, reset bus" wrapper | reached from the DMA-interrupt dispatcher for a SCRIPTS `INT` whose code the driver has no state for (again: no active request, `DSPS` not the idle code) |

`SIST0 = 0x90` is `M/A` (phase mismatch, bit 7) together with `RSL` (**reselected**,
bit 4).  Read plainly: the disk **reselected** the controller and presented a phase the
running SCRIPTS did not expect, at a moment when the miniport believed nothing was
active.  Its only recovery for that is a bus reset, which kills whatever *was* active --
the page read of the driver image -- with `SRB_STATUS_BUS_RESET`.

The second dispatcher branch worth noting: when `ISTAT` shows both `DIP` and `SIP`,
the ISR (`0x1e40`) services the **DMA** cause and returns; the SCSI cause is left for
the *next* interrupt.  It handles one cause per invocation, as NT miniports do,
and depends on the interrupt controller to call it again.

## 3. What is different about the real machine

Two things the emulator never does, and three things the HAL gets away with there.

### 3.1 The drive disconnects and reselects

The physical disk is an IBM DFHSS4W on a fast/wide bus.  Like every real drive, it
disconnects while it seeks and reselects the initiator when data is ready.  The
emulator's SCSI targets **never disconnect**: `scripts53c8xx.c` parks a *Wait
Reselect* with the comment "there are no disconnecting targets to reselect it".  So the
miniport's whole disconnect/reselect state machine -- the part of it that produced
`RSL`, the part where the active-request pointer is legitimately NULL between a
disconnect and a reselection -- has run only on the real machine, never in the suite
that validated the HAL.  That alone does not break it; Microsoft's miniport ran on
PReP machines with real drives.  It is the path on which the next two defects bite.

### 3.2 Grand Central latches edges, and does not re-request on unmask

Linux's driver for this exact controller (`arch/powerpc/platforms/powermac/pic.c`,
re-read today) encodes three hardware facts the HAL's `ints.c` does not know:

1. After writing the mask or the ack, it **spins reading the register back**
   ("make sure ack gets to controller before we enable interrupts").  Writes through
   Bandit are posted; enabling `MSR[EE]` before they land re-enters the handler with
   nothing pending.
2. "Unfortunately, setting the bit in the enable register when the device interrupt is
   already on *doesn't* set the bit in the flag register or request another
   interrupt."  Events latch on an **edge** of the source; a source that is still
   asserted when it is re-enabled is simply not delivered, and Linux retriggers it by
   hand.
3. When deciding what is pending it ORs **Levels** into Events
   (`bits = event | lost; bits |= level; bits &= mask`), because for a level source
   the event bit alone is not trustworthy.

Our `HalpExternalInterrupt` does the opposite on all three counts.  It reads Events
only, never Levels.  It clears the source's event bit *before* the driver's ISR has
touched the chip, so the chip's INT is still asserted at the clear -- fine for an
edge latch, but it means the only thing that will ever deliver the next interrupt is a
fresh falling edge on the chip's INT pin.  It then masks everything at or below the
device IRQL with a posted write and enables EE at once.  And when the handler is
entered with nothing pending it does

```c
if (events) MmioWrite32(GC_INT_CLEAR, events & ~mask);   /* "spurious: clear whatever fired while masked" */
```

which on a controller that latches edges throws away a real, masked, not-yet-serviced
event for some *other* source -- Cuda's VIA line, the other SCSI controller -- and
with it the only edge that source will produce.

The physical captures show item 1 happening.  Every one of the first twelve external
interrupts after the SCSI controller is enabled comes as a pair:

```
HAL: ext int: events 00400000 mask 00400000 pending 00400000
HAL: ext int: events 00000000 mask 00000000 pending 00000000
```

The second line is the handler re-entered with the HAL's shadow mask already zero
(IRQL 24, inside the SCSI ISR) and Events reading empty: the posted mask-and-clear had
not reached Grand Central when EE went back on.  Section 5 below records whether the
emulator prints the same pairs.

Now put 3.1 and 3.2 together on the 53C825A's documented interrupt behaviour (LSI,
*LSI53C825A/825AE Technical Manual* v3.1 §2.4.13.5): when a second cause arrives
*after* `SIP`/`DIP` is set it is stacked, and clearing the first cause **deasserts INT
for at least three clocks** before re-asserting -- an edge Grand Central latches.  But
"there is a small timing window in which multiple interrupts can occur but are not
stacked": both land in `SIST0`/`DSTAT` at once, with one assertion of INT.  A
reselection followed immediately by a phase mismatch is precisely a pair of SCSI
causes in one window (`0x90`), and a reselection's SCRIPTS `INT` (DMA cause) with a
SCSI cause alongside is a `DIP`+`SIP` pair.  In both cases the miniport services one
cause and returns with INT still asserted.  In the emulator the mode-0 line is
combinational, `(events | levels) & mask`, so unmasking re-fires immediately and the
second cause is serviced on the next instruction.  On the real controller nothing is
latched, nothing re-fires, and the second cause sits in the chip until some *other*
event produces an edge -- the drive reselecting, a completion -- by which time the
driver's idea of the bus (no active request) and the chip's (a reselected target in
a data phase) disagree.  Its answer to that disagreement is `0x476c`: reset the bus,
and complete the page read as `SRB_STATUS_BUS_RESET`.

This is a mechanism, not a proof; the proof is the experiment in §6.  But it is the
only candidate found that explains, with one cause, all of: the resets being the
*driver's own*; them following reselection (`RSL`) with the active pointer NULL; the
emulator never seeing any of it; the paired spurious interrupts; and the failure
landing on whichever image happens to be paging in while two requests overlap on the
bus (Beep, Sfloppy, Npfs -- the first drivers loaded while the CD-ROM class driver's
one-second TEST UNIT READY polls are also running on the other controller).

### 3.3 Three smaller divergences

- **The timebase.**  `ANS_TIMEBASE_HZ` is 11 000 000, the 500's 44 MHz bus divided by
  four.  The 700 has a 50 MHz bus (the machine prints it; the emulator's `ans700.c`
  attests it), so its timebase is 12.5 MHz.  On the 700, `KeStallExecutionProcessor`
  returns 12% early and the system clock gains 13.6%.  The miniport's bus-reset pulse
  of "30 µs" becomes 26.4 µs, still above the SCSI minimum of 25, so this is not the
  wall; it should be read from the firmware (`timebase-frequency` or the processor's
  `clock-frequency`/`bus-frequency`) rather than assumed.
- **`HalFlushIoBuffers` and the KSEG0 alias.**  Ours sweeps `0x80000000 + (pfn << 12)`
  for every page of a paged-in image.  HALIM2 replaced that with the MDL's mapped
  system address ("ANS-CACHEMAP1: a PFN does not imply a mapped 0x80000000 alias").
  The physical machine and the emulator both have 64 MB and `kseg0 top 80800000`, so
  this is not a hardware difference, but his version is the right one and should be
  taken.
- **The GPIO patch.**  HALIM2 patches two instructions in the miniport so that GPIO0 is
  left high (`li r4,8` → `0x0F`, `andi. 0xFE` → `0xFF`).  Per the LSI manual, GPIO0 is
  the SDMS activity LED and GPIO1 the EEPROM clock; GPIO3 is the differential-board
  sense.  The ANS developer note says nothing about what Apple wired to them.  The
  patch changed nothing measurable in his captures and can be dropped from the
  baseline.

What is *not* different, and was checked: the 604 snoops Bandit's DMA (Apple's note
calls the 604 a "snooping master" on a 32-byte coherence grain, and MkLinux/Linux on
these boards treat PCI DMA as coherent), so the HAL's cached common buffer and the
absence of data-cache sweeps around DMA are fine; SCSI parity RAM is irrelevant to the
PCI path; the installed HAL (`D600`, FNV `085AE99B`) is the one his source builds; and
the on-disk `npfs.sys` matches the CD.

## 4. Why the emulator did not catch it

Two modelling choices, both deliberate, both too kind:

1. **Grand Central mode 0 is combinational.**  `tnt.md` documents the power-on clear
   mode as `line = (events | levels) & mask` -- "the MkLinux scheme".  But MkLinux
   *ORs Levels in software* precisely because the chip does not; the hardware scheme,
   per Linux's comment, is `events & mask` with no re-request from a standing level.
   A model that follows the hardware here would have failed the HAL's ISR exactly as
   the machine does, as soon as two causes landed in one window.
2. **Targets never disconnect.**  The 53C825A engine has sync/wide negotiation and
   the SCRIPTS disconnect/reselect instructions, but the shared disk model answers
   every command without leaving the bus, so *Wait Reselect* is parked and the
   miniport's reselect handling has never executed.

Neither is a bug in the emulator's own terms (AIX, MkLinux and Mac OS all boot), but
both are places where it under-constrains a guest.

## 5. What the emulator does on the same boot

Recorded from a run of `ans-nt-installed-boot` with the HAL's serial transcript
echoed (the test passes, 75 s wall clock, logon and desktop reached):

```
HAL: EnableSystemInterrupt vector 54 irql 24
HAL: ext int: events 00400000 mask 00440000 pending 00400000
HAL: ext int: events 00400000 mask 00440000 pending 00400000
  ... twelve identical lines ...
```

Twelve interrupts, twelve with the SCSI event set, **no spurious entry**.  The
physical machine's first twelve are six real ones each followed by one with
`events 00000000 mask 00000000`.  Same HAL, same driver, same bus scan; the only
variable is the controller the writes go to.  (The mask differs -- `00440000` here
because the emulator's load order brings `adbport.sys` up before the SCSI driver and
the VIA line is already registered -- and that is immaterial.)  The emulator's SCSI
log at the default level shows no bus reset from the miniport during the boot: in
the emulator `0x476c` never runs.

## 6. What to do

For the HAL (`src/ints.c`), in the order a hardware run would want them:

1. **Read Grand Central back** after every mask or clear write before enabling EE
   (`HalpSetGcMask`, and the clear in `HalpExternalInterrupt`).  One `MmioRead32` of
   the mask register, as Linux does.  Removes the paired spurious entries.
2. **Service by level, not just by edge.**  In `HalpExternalInterrupt`, after the
   driver's ISR returns, re-read `GC_INT_LEVELS`; while the serviced source's level
   bit is still set and its ISR says it handled something, call it again before
   restoring the mask.  Equivalently, compute `pending` as
   `(events | (levels & LEVEL_SOURCES)) & mask`, with both 53C825A lines and the VIA in
   `LEVEL_SOURCES`.  This is the change that addresses §3.2 directly.
3. **Stop clearing masked events** on a spurious entry.  A masked source's latched
   event is the only edge it will produce; leave it for the unmask.
4. Take HALIM2's `HalFlushIoBuffers` (mapped-VA sweep), and derive the timebase from
   the firmware instead of `ANS_TIMEBASE_HZ`.

For the emulator (`granny-smith`), so that the suite constrains these:

5. Make Grand Central's **mode-0 line `events & mask`**, with the Levels register
   still live for software that reads it, and re-run the TNT/ANS rows (AIX and
   MkLinux both OR Levels in software and should be unaffected; the NanoKernel uses
   mode 1).  If `ans-nt-installed-boot` then fails at a SCSI interrupt, the emulator
   has reproduced the wall and item 2 can be developed against it.
6. Give the shared SCSI disk model a **disconnect/reselect option** (disconnect after
   the command, reselect after a scheduled seek delay) and enable it for the ANS NT
   rows, so the miniport's reselect path runs.

For the physical machine, instead of the loader-stage probe:

7. A HAL built with items 1-4 and one extra counter -- how many times item 2's
   level re-check found the source still asserted and called the ISR again -- printed
   at `CRASH1` or on the way to logon.  If that count is non-zero and the boot gets
   past NPFS, the theory held.  If the count is zero and NPFS still fails, §3.2 is
   wrong and the remaining suspect is the reselect path itself (§3.1), which item 6
   would then let us reproduce here.

## Sources

- MrMacintoshBlog/powermac-nt-hal: `docs/ANS-HANDOFF-2026-11-05.md`,
  `ANS-REASSESSMENT-2026-11-05.md`, `ANS-HARDWARE-PROGRESS-2026-10-02.md`,
  `-11-03.md`, `-11-05.md`; `traces/2026-11-05-halim2-recovery.log`,
  `2026-11-04-halv1-inpage.log`, `2026-11-04-classend-sfloppy.log`;
  `archives/floppies/2026-11-05/HALIM2-source-2026-11-05.tar.xz` (`src/scsitrace.c`,
  `src/dma.c`, `src/ints.c`).
- `symc810.sys` (NT 4.0 SP1 PowerPC), disassembled with `tools/pe-dis.py`; offsets
  above are image-relative.
- Linux `arch/powerpc/platforms/powermac/pic.c`: `__pmac_set_irq_mask`,
  `pmac_mask_and_ack_irq`, `pmac_pic_get_irq`.
- LSI Logic, *LSI53C825A/825AE PCI to SCSI I/O Processor Technical Manual* v3.1,
  §2.4.13.5 (stacked interrupts), `SIST0`, `GPREG`/`GPCNTL`, `DSTAT.BF`.
- Apple, *Network Server 500/700 Hardware Developer Notes*: §4.2 (interrupt map),
  §5.1 (system bus, snooping masters), §7.2 (SCSI backplane and hot-swap reset).
- granny-smith: `src/machines/tnt/grand_central.c`, `docs/internals/machines/tnt/tnt.md`
  ("Interrupt fabric"), `src/core/peripherals/pci/cards/scripts53c8xx.c`,
  `src/machines/tnt/ans700.c`.
