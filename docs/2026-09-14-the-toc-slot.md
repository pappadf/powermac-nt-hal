<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright (C) 2026 powermac-nt-hal contributors -->

# The four bytes that belong to the caller

*14 September 2026. Wall 49 — the `0x50` the installed system died on — was ours. The HAL had
been overwriting a word of every caller's stack frame since the first boot, because the ABI we
compile for and the ABI NT uses disagree about what lives at `4(r1)`. Fixing it took eight
instructions per export; finding it took a conditional breakpoint, a disassembler and one
arithmetic discrepancy that refused to go away.*

## 1. Where it stopped

```
HAL: HalAssignSlotResources bus 0 dev 15 fn 0 -> 2 resources
*** STOP: 0x00000050 (0xEE315C98,0x00000000,0x00000000,0x00000000)
PAGE_FAULT_IN_NONPAGED_AREA
```

## 2. Getting the faulting instruction

A bugcheck tells you the address that was *referenced*, never the instruction that referenced
it. On PowerPC that is `SRR0` at the data-storage interrupt, and the emulator's shell exposes
`srr0`, `srr1`, `dar` and `dsisr` — so a **conditional breakpoint on the DSI vector** picks the
one fault that matters out of the thousands a boot takes:

```
debug.breakpoints.add 0x300 "machine.cpu.dar == 0xEE315C98" "logical"
```

`tools/mkbootscript.py --bp ADDR[:COND]` now emits that, with `KeBugCheckEx` as a backstop
(`0x80679eac` — its export descriptor's entry, plus the load base the HAL prints). One run:

```
BP 0x300  srr0=0xee326bc0 srr1=0xb031 dar=0xee315c98 dsisr=0x40000000
          lr=0xee31db04 r1=0xe6e84320 r3=0x4 r4=0x0 r5=0x1 r6=0xe6e84358
BP KeBugCheckEx  r3=0x50 r4=0xee315c98
```

`DSISR` bit 1 is "translation not found", and `mmu.translate` agrees: `SRR0` maps, `DAR` does
not.

## 3. What the instruction was

```
0xEE326BC0  81628198  lwz    r11,-32360(r2)      <- faults
0xEE326BC4  818b0000  lwz    r12,0(r11)
0xEE326BC8  90410004  stw    r2,4(r1)
0xEE326BCC  7d8903a6  mtctr  r12
0xEE326BD0  804b0004  lwz    r2,4(r11)
0xEE326BD4  4e800420  bctr
```

That is NT PowerPC import glue, and the fault is its **first instruction — the TOC load**. So
`r2` was wrong, not the memory.

## 4. Which module

Neither `VIDEOPRT.SYS` nor `CIRRUS.SYS`. The caller's argument set-up is distinctive enough to
fingerprint — seven position-independent instructions, no branch displacements — so extracting
all 53 drivers off the disk with `tools/fatls.py` and searching them found exactly one match:

**`MGA_MIL.SYS`**, the Matrox Millennium miniport, loaded at `0xEE319000`. NT tries every video
miniport the registry lists until one claims the adapter, and Matrox's was in the queue. The
call it was making is `HalGetBusDataByOffset(PCIConfiguration, bus, slot, buf, 0, 4)` — reading
a candidate device's vendor id.

From its entry descriptor, MGA_MIL's TOC is at RVA `0x8280`, so loaded it should be
`0xEE321280`. The observed `r2` was `0xEE31DB04` — RVA `0x4B04`, a **code address**, and
specifically the instruction after the `bl` that entered the stub.

## 5. The four bytes

That is the value `mflr` would give inside a callee. And there it was, still sitting in the
caller's frame:

```
0xe6e84320  0xe6e84380      <- back chain
0xe6e84324  0xee31db04      <- 4(r1): should be MGA_MIL's saved TOC
```

`4(r1)` is where NT's import glue saves the caller's TOC — the `stw r2,4(r1)` two instructions
into the stub above. It is also, in the **SVR4** ABI we compile for, the caller's **LR save
slot**. Our prologues:

```
HalGetBusDataByOffset:
    mflr 0
    stwu 1, -48(1)
    stw  0, 52(1)        ; 52 = 48 + 4  ->  4(caller's r1)
```

Microsoft's, for comparison — LR into a callee-saved register, TOC at `8(r1)`, never `4`:

```
KeBugCheckEx:                       MGA_MIL's own helper:
    stw    r21,-44(r1)                  stw    r25,-28(r1)
    mfspr  r21,lr                       mfspr  r25,lr
    ...                                 ...
    stw    r2,8(r1)                     stwu   r1,-96(r1)
    stwu   r1,-736(r1)
```

So every HAL export that saved LR destroyed its caller's saved TOC. The caller's
`lwz r2,4(r1)` after the call then loaded our return address into `r2`, and its next
TOC-relative access read `r2 - 32360` — below the image, unmapped.

## 6. The last four bytes, and a lying register

One discrepancy held this up for a while: `r2` derived from `DAR` came out as `0xEE31DB00`,
while the slot plainly held `0xEE31DB04`. Four bytes, consistently.

`0xEE315C98 ^ 4 = 0xEE315C9C`. **The emulator reports `DAR` with the little-endian address
munge still applied** — the same munge that makes `poke.l A` write the guest word at `A ^ 4`.
Un-munged, the effective address is `0xEE315C9C`, `r2` is `0xEE31DB04`, and every number agrees.

That is the second entry for `STORY.md`'s "your instrumentation lies too" — and the bugcheck's
own parameter is munged as well, so the address NT prints on the blue screen is four bytes away
from the one the code asked for.

## 7. The fix

`thunk.S` already guarded the **other** direction, and its comment names the hazard exactly: an
NT callee may write the caller's frame at 4, 8 and 24..55, so every call *into* the kernel goes
through a 64-byte frame our SVR4 code never looks at. The same was never done for calls *out of*
the kernel into us.

- `tools/mkstubs.py` now emits, per export, `desc_F = { xthunk_F, 0 }` and

  ```
  xthunk_F:  mflr 0 ; stwu 1,-64(1) ; stw 0,60(1) ; bl F ; lwz 0,60(1) ; addi 1,1,64 ; mtlr 0 ; blr
  ```

  so `F`'s own LR store lands at offset 4 of the *thunk's* frame, which nothing reads.
- `DEFINE_DESC` in `include/nt.h` does the same for the three descriptors the kernel calls
  directly — the external-interrupt, decrementer and machine-check routines. A `static` handler
  referenced only from an asm string is discarded as unused before the assembler sees it, so the
  macro also emits a `__attribute__((used))` keepalive.

Cost: eight instructions and a 64-byte frame per call across the HAL boundary. `hal.dll` grew
from 42,496 to 45,056 bytes.

## 8. What it bought

The `0x50` is gone, and the installed system now completes NT's I/O initialisation:

```
HAL: HalAssignSlotResources bus 0 dev 17 fn 0 -> 4 resources     both 53C825A controllers,
HAL: GetAdapter / GetInterruptVector / EnableSystemInterrupt      with adapters and interrupts
HAL: ext int: events 00400000 ...                                 and real SCSI interrupts
HAL: IoReadPartitionTable: 2 partitions, signature 4e544844       our partition code, per disk
HAL: HalAssignSlotResources bus 0 dev 15 fn 0 -> 2 resources      the video device
```

Worth noting, without over-claiming: **no `VideoPortVerifyAccessRanges` bypass was applied in
this run** (ledger row 6's diagnostic poke), and no video conflict failure appeared. That the
check now *passes* has not been confirmed directly — only that the failure did not recur. Wall
25 is worth re-testing on purpose, because a video driver whose TOC we had been corrupting is a
plausible cause of an unexplained `ERROR_INVALID_PARAMETER`.

## 9. Where it stops now, and why that is not the HAL

```
STOP: c000026c {Unable to Load Device Driver}
\SystemRoot\System32\Drivers\Msfs.SYS could not be loaded.  Error Status was 0xc0000102
```

`0xC0000102` is `STATUS_FILE_CORRUPT_ERROR`, and it is right. Walking the FAT from the host
finds **49 files whose cluster chain is shorter than the size in their directory entry** —
`MSFS.SYS` has 16,384 bytes allocated of 42,512. Wall 48 found two truncated hives in this
image; it is wider than that, and clustered alphabetically (`MSFS`, `MUP`, `NDIS`, `NET*`,
`NDDE*`), which is what a capture taken mid-copy looks like.

That is ledger row 15, not a HAL defect. The fix is to re-capture the disk after text-mode Setup
has actually flushed — and `tools/fatput.py` now grows and shrinks cluster chains, so a rebuilt
`hal.dll` can be dropped onto an installed image without re-running Setup, which is the
iteration loop everything after this depends on.
