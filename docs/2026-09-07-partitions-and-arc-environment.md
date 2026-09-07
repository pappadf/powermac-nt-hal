# Partition tables, the ARC environment, and the start of an install

*2026-09-07. `src/disk.c` (new), `src/misc.c` (the ARC environment and drive letters),
`tools/mkarcdisk.py` (new). The narrative version is [`../STORY.md`](../STORY.md) walls 32–40;
this document keeps the register- and structure-level detail.*

Setup began the day refusing to start — *"No valid system partitions are defined on this
computer"* — and ended it writing `\WINNT` to a formatted volume. Four HAL exports had to become
real, and two of them taught conventions that are not written down anywhere public.

---

## 1. Where the partition table lives on NT 4.0

On NT 4.0 the MBR is the HAL's business. Three exports, and nothing above the HAL reads sector 0
itself:

| export | who calls it | what it must do |
|---|---|---|
| `IoReadPartitionTable` | `disk.sys` at device init, `ftdisk`, text-mode Setup | return a `DRIVE_LAYOUT_INFORMATION` for one disk |
| `IoWritePartitionTable` | Setup and Disk Administrator, via `IOCTL_DISK_SET_DRIVE_LAYOUT` | lay a layout back down |
| `IoSetPartitionInformation` | Setup, after it formats | change one partition's system-id byte |

All three were stubs returning `STATUS_UNSUCCESSFUL`, so **every disk read back as having no
partitions at all**. Setup's screen is the honest report of that, and it is worth reading as a
specification rather than an error: *"System partitions are created and managed by a
manufacturer-supplied configuration program."*

### The I/O

Reads and writes go out as `IRP_MJ_READ`/`IRP_MJ_WRITE` built with
`IoBuildSynchronousFsdRequest` and dispatched with `IoCallDriver`, waiting on a `KEVENT` when
the driver returns `STATUS_PENDING`. That is legal here because every caller of these three
arrives at `PASSIVE_LEVEL` in a real thread — `IoBuildSynchronousFsdRequest`'s IRP is freed by
the I/O manager through an APC on completion, which needs exactly that.

Two details are specific to this target rather than to MBRs:

- **Misaligned fields.** A partition entry's 32-bit fields sit at `+8` and `+12` inside a
  16-byte record starting at `0x1BE`, so they are never 4-byte aligned. On a 604 running
  little-endian a misaligned access raises an alignment exception instead of being fixed up in
  hardware, so they are assembled a byte at a time — and the pointers are `volatile`, because
  clang otherwise folds the four byte loads back into the single `lwz` the code exists to avoid
  (STORY wall 33; the store-side equivalent is wall 34 and the `Makefile`'s
  `-mllvm -combiner-store-merging=false`).
- **No `__udivdi3`.** A byte offset divided by a sector size is a 64-bit division and a
  freestanding HAL has no libgcc. `HalpDivBySector` is shift-and-subtract long division, for
  the same reason `memset` and `memcpy` are hand-written in `misc.c`.

## 2. The layout convention `IoWritePartitionTable` has to know

This is the part that is not documented. When Setup created a partition it handed down eight
entries; logging every one of them is what made it tractable:

```
HAL: IoWritePartitionTable: 8 entries (2 tables), signature 4e544844, 32/64 geometry
HAL:  [0] type 00000006 start 4096   len 65536   hidden 0 num 1 boot
HAL:  [1] type 00000005 start 69632  len 978944  hidden 0 num 0 rewrite
HAL:  [2] type 00000000 start 0      len 0       hidden 0 num 0
HAL:  [3] type 00000000 start 0      len 0       hidden 0 num 0
HAL:  [4] type 00000006 start 69664  len 978912  hidden 0 num 2 rewrite
HAL:  [5..7] unused, rewrite
```

Three rules follow:

1. **Four entries per on-disk table.** `PartitionCount` is a multiple of four: group 0 is the
   MBR, and each group after it is the extended boot record of one logical drive. A caller that
   writes only group 0 silently loses everything in the chain.
2. **Setup prefers a logical drive.** Entry `[1]` is type `0x05` — an extended container — and
   `[4]` is the FAT16 logical drive inside it. Two primary slots were free and it used the chain
   anyway, so an implementation that handles only primaries does not survive first contact.
3. **`HiddenSectors` arrives zero.** Every `StartingOffset` is absolute; the field that would
   have told you each table's own sector is not filled in on this path. Deriving the EBR's
   location as `start - HiddenSectors` puts it at 69664 — the start of the logical drive's
   *data* — instead of 69632, where the MBR's type-05 entry points.

So each relative-sector field must be computed from the absolutes, and the two rules an extended
chain uses are different from each other:

```
a partition entry's relative sector  =  its absolute start  −  the LBA of the table it sits in
a chain link's relative sector       =  its absolute start  −  the outermost extended partition
```

Get that wrong and the chain reads back unterminated; `IoSetPartitionInformation` then cannot
find partition 2 (`c0000001`), and `setupdd.sys` walks the result into a null pointer. Right:

```
HAL:  table 0 -> lba 0     (00000000)
HAL:  table 1 -> lba 69632 (00000000)
HAL: IoSetPartitionInformation: #2 type -> 00000006 (table lba 69632 slot 0, 00000000)
```

`IoReadPartitionTable` walks the same shape in reverse, and numbers recognised partitions the
way NT does — primaries in table order, then each logical drive in chain order.

## 3. Setup does not look for a system partition; it asks the firmware

With the table read correctly, Setup showed the *same* "no valid system partitions" screen, byte
for byte. It does not scan: it calls `NtQuerySystemEnvironmentValue`, which on this architecture
lands in **`HalGetEnvironmentVariable`** — a fourth stub, returning `ENOENT` for everything.

The trace shows the whole ARC vocabulary `usetup` expects, and which one answered:

```
HAL: env get 'SystemPartition'  -> 'multi(0)scsi(1)disk(0)rdisk(0)partition(1)'
HAL: env get 'LoadIdentifier'   -> ENOENT
HAL: env get 'OsLoader'         -> ENOENT
HAL: env get 'OsLoadPartition'  -> ENOENT
HAL: env get 'OsLoadFilename'   -> ENOENT
HAL: env get 'OsLoadOptions'    -> ENOENT
```

`src/misc.c` now keeps a real store in the ARC `NAME=value\0…\0\0` shape, with **get and set
both working** — Setup writes `OSLOADER`, `OSLOADPARTITION`, `OSLOADFILENAME` and
`LOADIDENTIFIER` back at the end of the copy phase. It is RAM-backed: this machine has no ARC
NVRAM (Open Firmware's own `nvram` partitions are a different format with a different owner) and
nothing here boots from the ARC environment anyway, because Open Firmware's boot script starts
the veneer. That, and the fact that the HAL *synthesises* `SYSTEMPARTITION` at all, are both in
the workaround ledger.

**Where the value comes from.** On real hardware `ARCINST.EXE` writes it. There is no such tool
here, so a seeding pass at phase 0 derives it from the loader's own ARC disk list — the first
disk whose partition table SETUPLDR could read, plus `partition(1)`:

```
HAL: arc disk sig 20202020 sum d277fb75 valid 0 'multi(0)scsi(0)cdrom(0)fdisk(0)'
HAL: arc disk sig 4e544844 sum 60d0b3a8 valid 1 'multi(0)scsi(1)disk(0)rdisk(0)'
HAL: env set 'SYSTEMPARTITION' = 'multi(0)scsi(1)disk(0)rdisk(0)partition(1)'
```

`4e544844` is the signature `tools/mkarcdisk.py` writes into the MBR at `0x1B8`, and `valid 1`
means SETUPLDR parsed the table — so the ARC path of the system partition was already there for
the taking.

### `HalSetEnvironmentVariable`, and a bug worth the retelling

The first implementation removed an entry by shifting bytes down until it saw a NUL *pair*, and
never cleared what the shift vacated. Every re-set therefore left stale structure behind, which
the next removal walked into. Extracting the shipped functions into a native harness showed it
plainly — after eight sets and eight deletes:

```
|.F=6..F=6..F=6..F=6..F=6..F=6...=7...=8...|
```

and with a tail containing no NUL pair the loop would have run past the end of the array. Since
Setup's five writes land at the end of the copy phase, this sat directly in front of the next
thing to be attempted. The fix computes the gap and the tail explicitly, moves exactly that many
bytes, clears the vacated region, and bounds every walk by the store's own length. Verified with
the real functions under AddressSanitizer: both scrambling sequences, Setup's actual writes, and
200,000 random operations with the store's structure asserted after each one.

## 4. The disk itself

NT wants a FAT partition with 750 KB free on the boot disk before it will start, and at least
158 MB for the installation. [`../tools/mkarcdisk.py`](../tools/mkarcdisk.py) does `ARCINST`'s
job: an MBR, up to four primary FAT16 partitions, each formatted empty (BPB, two FATs seeded
with the media descriptor, a zeroed root directory). Plain MBR, plain FAT16 — the
machine-specific part is only that nothing on this platform will do it for you.

Two things about the emulated disk are worth writing down because they cost time:

- **The staging disk is not blank.** It holds `VENEER.EXE` at block `0x800`, which is where the
  ROM's `pe-loader` is told to read it from. A partition that starts at LBA 63 runs straight
  over it. The system partition therefore starts at 4096, leaving the veneer untouched — and
  leaving a 2 MB gap that Setup's partition screen offers as the *first* choice, which is how
  "too small for Windows NT" appeared twice for two different reasons.
- **A second SCSI disk does not work.** The veneer's ARC path for a second target does not reach
  the disk: both hard disks reported `valid 0` with *identical* checksums, which cannot be true
  of two disks with different sector 0s. The kernel, going through `symc810` moments later, read
  the same MBR correctly. One disk, larger, is the answer.

## 5. Where it got to

| screen | trace |
|---|---|
| Partition list on a 512 MB disk | [`2026-09-07-setup-13-partition-list-512mb.png`](../traces/2026-09-07-setup-13-partition-list-512mb.png) |
| The 478 MB free space selected | [`-14-free-space-selected.png`](../traces/2026-09-07-setup-14-free-space-selected.png) |
| Partition created, choose a file system | [`-15-partition-created-format.png`](../traces/2026-09-07-setup-15-partition-created-format.png) |
| `\WINNT` | [`-16-install-directory.png`](../traces/2026-09-07-setup-16-install-directory.png) |
| The surface-scan prompt | [`-17-disk-examination.png`](../traces/2026-09-07-setup-17-disk-examination.png) |
| *"Setup will install Windows NT on partition D: FAT 478 MB (477 MB free)"* | [`-18-install-on-existing-volume.png`](../traces/2026-09-07-setup-18-install-on-existing-volume.png) |
| *"Creating directory \WINNT…"* | [`-19-creating-winnt.png`](../traces/2026-09-07-setup-19-creating-winnt.png) |

## 6. Where it stops, and why

```
*** STOP: 0x0000001E (0xC0000005, 0x800E8730, 0x00000000, 0x00000006)
*** 800E8730 has base at 8007B000 - ntoskrnl.exe
```

`0x800E8730` is `ntoskrnl.exe + 0x6D730`, which disassembles to the first instruction of
**`_wcsicmp`**: `lhz r10,0(r3)`, reading a halfword at `r3 == 6`. Six is a `UNICODE_STRING`'s
`Length` — three characters, e.g. `"D:\"` — passed where its `Buffer` belonged.

The cause is `IoAssignDriveLetters`, which creates `\DosDevices\` links only for
`\Device\Harddisk%d\Partition1`: **one letter per disk, not one per partition.** Setup numbers
the volumes itself and called the install target `D:`; the HAL had given `D:` to the CD, and the
volume being installed to has no symbolic link at all.

The fix is understood: assign a letter to every recognised partition in NT's order — the first
primary of each disk, then logical drives, then the remaining primaries, then the CD-ROMs —
which needs `IoGetDeviceObjectPointer` to open each `\Device\Harddisk%d\Partition0` and the
partition table this HAL now reads. It is not written yet.

## 7. Two notes on the instrumentation, for whoever debugs the next one

Both cost a probe each, and neither is in any documentation:

- **This emulator's breakpoints report *after* the instruction executes.** A breakpoint on a
  faulting load never fires for the fault: 18,171 hits on the instruction that crashed, every
  one with a valid pointer, because the fatal execution raised an exception instead of
  completing. Conclusions drawn from "the last hit looked fine" were drawn from the wrong
  instruction.
- **The shell's `mmu.peek` does not apply the little-endian address munge.** A CPU word at `A`
  is `mmu.peek(A ^ 4)`. Calibrate against a value you already know from a register before
  trusting any dump.
