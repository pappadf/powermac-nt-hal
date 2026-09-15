<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright (C) 2026 powermac-nt-hal contributors -->

# The boot floppy

*15 September 2026. A plan to stop patching the user's Windows NT CD. Everything this project
adds — the HAL, the veneer's seven patch sites, the ADB keyboard driver — moves onto a floppy,
and the CD is used unmodified, as pressed. Targeting the Apple Network Server first, but built so
the same work reaches a 7500/8500 later instead of being thrown away.*

## 0. Before you start

This assumes nothing except a checkout. Read [`EMULATOR.md`](EMULATOR.md) first — it is the
step-by-step emulator setup — and [`CHARTER.md` §3.1](CHARTER.md#31-replicating-it) for what you
must supply yourself. In short:

| you need | notes |
|---|---|
| Granny Smith, branch `ppc-le-mode-and-bandit-lane-reversal` | **not `main`** — little-endian mode, Bandit lane reversal and the Cirrus id registers are unmerged. `make headless` |
| An ANS Open Firmware ROM | `ans-2.26NT`, MD5 `ad405e01c663340c479668c70f741f1b`. The **NT** ROM: §7 explains why that matters |
| An NT 4.0 Workstation PowerPC CD | OEM 000-48303, MD5 `ab37556d72818ed082c1d01c2d7f1898`. Work on a copy |
| This repo | `make` → `build/hal.dll` |

Neither the ROM nor the CD is in any repository, and neither may be redistributed.

### 0.1 Running an experiment

Everything below is a Granny Smith shell script (`.gs`) fed to the emulator. Two ways to run one:

```bash
# one-shot: builds a machine, runs the script, exits.  No daemon needed.
R=<path to the ANS ROM>
./build/headless/gs-headless --speed=turbo --no-prompt -q \
    rom=$R --var ROM=$R --checkpoint-dir=tmp/ckpt script=tmp/my.gs > tmp/my.log 2>&1

# against a long-lived daemon (needed by tools/run-boot.py, which splices a disk image)
./build/headless/gs-headless --daemon --port=6820 --speed=turbo --no-prompt -q \
    --checkpoint-dir=tmp/ckpt-daemon rom=$R --var ROM=$R &
python3 tools/gsh.py 'echo alive'
```

`--var ROM=` matters as well as `rom=`: the boot helper re-reads `$ROM` when it restarts the
machine to apply a console change.

### 0.2 Generating the scripts

```bash
# a cold boot from power-on to Setup: no pokes, no breakpoints, no checkpoint
python3 tools/mkcoldboot.py --rom $R --cd <patched.iso> --staging <disk.img> \
        --out tmp/cold.gs --screenshot tmp/cold.png --chunks 1400
#   --veneer-dev / --cd-dev   the Open Firmware paths for the veneer source and the boot device
#   --console screen          type on the ADB keyboard instead of the serial port (see §9)

# the veneer, with ledger rows 1-5 applied as bytes rather than pokes
python3 tools/mkveneer.py <PPC/VENEER.EXE> --out tmp/veneer.exe --for cd \
        --stage <disk.img>@0x800

# a whole patched CD + a staging image, from a pristine disc
python3 tools/mkpatchediso.py <NT.iso> tmp/out.iso --hal build/hal.dll \
        --kbd <i8042prt replacement> --staging tmp/staging.img
```

### 0.3 The artifacts these experiments used

None are in git; all are reproducible.

| artifact | what | how to make it |
|---|---|---|
| `tmp/nt-one.iso` | patched CD | `mkpatchediso.py` (above) |
| `tmp/nt-onedisk.img` | 512 MB disk: ARC system partition at LBA 4096, **and the veneer at block `0x800`** in the gap before it | `mkarcdisk.py --part 4096:65536 --part 69632:0` then `mkpatchediso.py --veneer-into` |
| `tmp/oem-floppy.img` | 1.44 MB FAT12 floppy | below |
| `tmp/nt-pre-go-big2.ckpt` | pre-`go` checkpoint | expensive; `mkcoldboot.py` makes it largely unnecessary. **Invalidated by any emulator rebuild** — `tools/restamp-ckpt.py <gs-headless> <ckpt>` fixes that when the rebuild changed no checkpointed structure |

```python
# tmp/oem-floppy.img — a plain FAT12 floppy, enough for the firmware to read
import struct
img = bytearray(b'\x00' * 1474560); bs = bytearray(512)
bs[0:3] = b'\xeb\x3c\x90'; bs[3:11] = b'MSDOS5.0'
struct.pack_into('<HBHBHHBHHHII', bs, 11, 512,1,1,2,224,2880,0xF0,9,18,2,0,0)
bs[38] = 0x29; bs[43:54] = b'NTOEMDISK  '; bs[54:62] = b'FAT12   '; bs[510:512] = b'\x55\xaa'
img[0:512] = bs; img[512] = 0xF0; img[513] = 0xFF; img[514] = 0xFF
open('tmp/oem-floppy.img','wb').write(bytes(img))
```

### 0.4 The one command that reproduces the blocker

```bash
python3 tools/mkcoldboot.py --rom $R --cd tmp/nt-one.iso --staging tmp/nt-onedisk.img \
    --veneer-dev /bandit/53c825@12/sd@0,0 --cd-dev /bandit/gc/swim3 \
    --out tmp/fd.gs --chunks 500
# then, in tmp/fd.gs: insert the floppy after the attach lines, and before `go` add
#     machine.memory.poke.l 0x60c0c 0x00000760      (VrDebug: see Appendix A)
# run it, then:
grep -c FloppyDiskPeripheral tmp/fd.log        # 2 with bootpath on the floppy, 1 without
grep -oE "find_boot_dev:[^\\]*|VrOpen returned [0-9]+" tmp/fd.log
```

## 1. Why

The deliverable today is a 578 MB ISO with 126 KB overwritten. It works, it is verified, and it
can never be shared: it is Microsoft's disc with our bytes in it. Every recipient has to bring
their own CD anyway, so the only thing worth distributing is the 126 KB — plus whatever replaces
maciNTosh's `usbadb.sys`, which is not ours to redistribute either.

A floppy inverts that. The user keeps their CD untouched; we ship ~1.4 MB. It also makes the
browser-patcher idea trivial — no 578 MB upload, no in-place ISO surgery, just a download.

And it retires the two ledger rows that exist *because* we edit the disc:

| row | what | retired by |
|-----|------|-----------|
| 7 | ~~HAL delivered by overwriting `HALEAGLE.DLL`~~ (already retired by `mkoem.py`) | — |
| 8 | the `TXTSETUP.SIF` and `\PPC` directory patches edit a user's CD image | this plan |

Rows 1–5 (the veneer patches) are *not* retired by a floppy. They are retired by §7.

## 2. How the boot would work, step by step

Numbers in brackets are the evidence in §3.

**Once per machine, at the `0 >` prompt.** `little-endian?` is firmware NVRAM and `reset-all` is
what applies it; no medium can set it before the firmware has read the medium. This step cannot
be moved onto the floppy, and any claim of "insert and go" on a virgin machine is false.

```
setenv little-endian? true        \ present on plain ROMs too, not an NT extension  [E9]
setenv real-mode? false
setenv real-base 3F00000
setenv load-base 3E00000
reset-all
```

**Then, every boot.** The firmware reads the veneer off the floppy and starts it:

```
dev /packages/pe-loader   3D00000 27800 map-space   dev /      \ map first, or DMA fails  [E2]
0 value fdih   " /bandit/gc/swim3" open-dev to fdih            \ OF opens the drive       [E1]
3D00000 0 13C " read-blocks" fdih $call-method .               \ OF reads the floppy      [E2]
dev /packages/pe-loader   3E00000 27800 map-space   dev /
3D00000 3E00000 27800 move   27800 to loadsize   init-program
" /bandit/gc/swim3" encode-string " bootpath" _chosen (property)
go
```

None of those words is line-oriented, so the whole block can very likely be one line — which is
what makes `nvramrc` viable (§6, step 5) and reduces the typing to nothing.

**What happens after `go`:**

1. The veneer starts at `0x50000`, already carrying ledger rows 1–5 as bytes (`mkveneer.py`).
2. It reads `/chosen bootpath` — now the floppy — and builds its ARC device tree.
3. It loads `\PPC\SETUPLDR` **from the floppy**, because SETUPLDR is resolved relative to the
   boot device [E3]. *This is the step that does not work yet*: the veneer currently names the
   drive `multi(0)other(0)other(0)` and `VrOpen` on it returns `EIO` (E7b, §4).
4. SETUPLDR opens its boot device **raw** and parses the filesystem itself [E3]. On the CD that
   is ISO 9660; on a floppy it must be FAT — and `fat-files` is in every ROM we looked at [E9],
   though it is SETUPLDR's own reader that matters, not the firmware's (§8, R2).
5. SETUPLDR finds `txtsetup.oem` on that floppy and reads the OEM `Computer` class — our HAL —
   using firmware I/O only, with no NT driver anywhere in the path [E4][E5].
6. It also finds `\PPC\I8042PRT.SYS` there, because that name is resolved on the boot device
   [E3]. This is how the ADB keyboard driver arrives without touching the CD.
7. NTOSKRNL starts. From here the **CD**, unmodified, supplies every other file over SCSI with
   stock Microsoft drivers — which this project has already driven end to end to a desktop.

## 3. What we have actually verified

Every row was measured on the emulator, not inferred.

| id | claim | evidence |
|----|-------|----------|
| E1 | Open Firmware sees and opens the floppy | `/bandit/gc/swim3@15000` in `dev /bandit/gc ls`; `open-dev` → `4D6040`. `/swim3/disk` → `0`, so there is no child node |
| E2 | Open Firmware can **read blocks** off it | `3C00000 0 8 " read-blocks" fdih $call-method` then host peek → `EB 3C 90 'M'`, `'SDOS'` — the FAT12 boot sector we wrote. **Only works when `map-space` is issued inside `dev /packages/pe-loader`**; outside it, `map-space` is an unknown word and the read fails `bad address to DMA-MAP-IN` |
| E3 | SETUPLDR resolves files against its **boot device**, and parses the filesystem itself | With `VrDebug 0x1200` over a whole Setup run: 4 × `VrOpen multi(0)scsi(0)cdrom(0)fdisk(0)` (the raw device), 1 × `…\PPC\SETUPLDR`, and **zero** by-name `.SYS` opens. The device is character-for-character the path it booted from |
| E4 | OEM-disk support is compiled into **SETUPLDR** | `D:\nt\private\ntos\boot\setup\oemdisk.c` at two offsets, beside `setup.c` and `arcdtect.c`; strings `ForceOemHal`, `Hal.Load`, `Computer`, `Map.Computer`, `Disks`, `Files.`, `Config.`, `Defaults`, `Strings`, `txtsetup.oem` |
| E5 | SETUPLDR's I/O is firmware I/O | `VrOpen: Exit - FileId: 2 IHandle: ff8d3200` — an Open Firmware instance handle. ARC → veneer → OF, no NT driver |
| E6 | `setupdd.sys` has an **NT-side** OEM path | wide strings `\device\floppy0\txtsetup.oem`, `\device\floppy%u`, sections `Disks Defaults Computer Display Keyboard Mouse SCSI` |
| E7a | The veneer does not enumerate the floppy **when the boot path does not name it** | Full ARC dump (31 `dump_node` entries) with a floppy inserted and `bootpath` on the hard disk: exactly **one** `FloppyDiskPeripheral`, `Parent → CdromController` — the `fdisk(0)` tail of the CD path. Identical with and without a disk in the drive |
| E7b | It **does** enumerate it when `bootpath` names it — but types it `other`, and `VrOpen` then fails | With `bootpath` = `/bandit/gc/swim3`: `FloppyDiskPeripheral` 1 → **2**, `DiskController` 1 → **2**. `find_boot_dev: bootpath (len 16) '/bandit/gc/swim3'` → `find_boot_dev: bootpath 'multi(0)other(0)other(0)'` → `Booting from 'multi(0)other(0)other(0)'` → `VrOpen: Entry - Path: multi(0)other(0)other(0)` → **`VrOpen returned 8`**. Eight is `EIO` on the ARC status ordering, corroborated by wall 22's `VrOpen returned d` = 13 = `ENODEV`. So the device is *found* and the I/O fails — not a missing device |
| E8 | The emulator models the hardware already | `ans500.c` declares `.floppy_slots = tnt_floppy_slots` ("Internal FD0", `FLOPPY_HD`); `tnt.c` binds SWIM3 to Grand Central and DBDMA. Insert works and survives `reset-all` |
| E9 | `pe-loader` is the **only** NT-ROM-specific package | `ans-2.26NT` packages: deblocker, disk-label, obp-tftp, mac-files, mac-parts, aix-boot, fat-files, iso-9660-files, xcoff-loader, **pe-loader**, terminal-emulator. Plain `ans-1.1.22`: the same list **minus `pe-loader`**. `little-endian?` exists in both |
| E10 | `nvramrc` exists on this firmware | `printenv` shows `use-nvramrc? false`, `auto-boot? true`, `boot-command boot`, and `nvramrc` |

## 4. The blocker

**E7b is the whole problem, and it is one layer deeper than it first looked.**

The veneer is not blind to the drive. Point `/chosen bootpath` at `/bandit/gc/swim3` and it reads
the property, adds a `DiskController` and a `FloppyDiskPeripheral` to its ARC tree, and derives a
path for it. What it derives is **`multi(0)other(0)other(0)`** — it recognises Bandit SCSI and the
CD-ROM and falls through to "other" for SWIM3 — and opening that path returns **`EIO`**.

So the failure is *not* "the device does not exist". It is:

1. a **device-type mapping gap** — SWIM3 is classified `other`, so the ARC name SETUPLDR would
   expect (`multi(0)disk(0)fdisk(0)`) is never produced; and
2. an **I/O path gap** — whatever `VrOpen` does to open that node fails, even though Open
   Firmware itself reads the same drive perfectly (E2).

The second is the surprising one and worth dwelling on: raw `read-blocks` through
`/bandit/gc/swim3` returns the FAT12 boot sector byte for byte, so the capability is there and
the veneer is not using it — or is using it in a way the `swim3` package does not answer.

That makes the target much better defined than "teach the veneer about floppies", and it is now
*traceable*: `VrDebug 0x760` prints `find_boot_dev`'s reasoning line by line, and `VrOpen`'s entry
and exit, so the next person can watch exactly where `EIO` comes from rather than guessing.

It is still new behaviour inside a proprietary binary, which is what §7 weighs.

### 4.1 The experiment that produced E7b — **done, 15 September**

Run as described: `mkcoldboot.py --veneer-dev /bandit/53c825@12/sd@0,0 --cd-dev /bandit/gc/swim3`,
floppy inserted, `VrDebug` poked to `0x760` before `go`. The veneer is still loaded from the disk,
so only the boot path is under test. Result above.

### 4.2 The next experiment

Find where `EIO` is raised. `VrOpen` is at veneer image `0x54744`–`0x5486c` (the range
`mkveneer.py` already restores for wall 46), and the image has a full symbol table — 1,512
symbols, readable with `tools/coffsyms.py` / `tools/coffdis.py`.

**Use `VrDebug 0x1208`, not `0x760`.** Bit `0x0008` traces *the OBP → ARC device-tree
conversion* — which is precisely where SWIM3 is classified `other` — and was not enabled in
the run that produced E7b. With `0x0200` (`VrOpen`) and `0x1000` (`VrRead`) beside it you see
the decision and the failed I/O in one pass. Appendix A has the full table.
Two questions, in order:

* does `VrOpen` reach an Open Firmware call at all for an `other(0)other(0)` node, or does it
  reject the type before trying?
* if it calls, which method does it call — and does the `swim3` package implement it? OF answers
  `read-blocks` on this device (E2) but has no `/swim3/disk` child (E1), so a veneer that expects
  `disk-label` to interpose would find nothing to talk to.

The answer decides whether this is a small patch (map SWIM3 to `disk`/`fdisk`, use `read-blocks`
directly) or confirmation that §7 is the honest route.

## 5. What has to be built

| # | component | what it is | depends on |
|---|-----------|------------|-----------|
| C1 | `tools/mkbootfloppy.py` | writes the 1.44 MB image: FAT12, `\PPC\SETUPLDR`, the patched veneer, `HALSHINR.DLL`, `I8042PRT.SYS`, `txtsetup.oem` | `mkveneer.py` (exists) |
| C2 | `txtsetup.oem` | the OEM description: `[Disks]`, `[Defaults]`, `[Computer]` naming our HAL | E4; format confirmed, contents untested |
| C3 | floppy-aware cold boot | `mkcoldboot.py --veneer-dev /bandit/gc/swim3`, reading `0x13C` blocks from block 0 | already parameterised (`--veneer-dev`) |
| C4 | veneer SWIM3 **type mapping + `VrOpen` I/O**, or §7 | narrower than first written: enumeration already works (E7b); what is missing is the `disk`/`fdisk` classification and an open that does not return `EIO` | §4.2 |
| C5 | our own ADB port driver | replaces maciNTosh's `usbadb.sys`, the last non-shippable piece | `entii-for-workcubes` `fpsidrv`; HAL half exists |
| C6 | `nvramrc` installer | the §2 block as one line, so the machine boots the floppy unattended | E10; one-line form untested |

## 6. Order of work

1. ~~Run §4.1.~~ **Done** — the veneer enumerates the drive but names it `other(0)other(0)` and
   `VrOpen` returns `EIO` (E7b). **Run §4.2 next**: find where `EIO` is raised. Nothing else is
   worth starting until that is understood, because it decides C4 and therefore §7.
2. **C1 + C2 + C3** — build the floppy and boot it far enough to see SETUPLDR loaded *from the
   floppy*. Success is a `VrOpen` trace naming the floppy device, not the CD.
3. **Get SETUPLDR to read `txtsetup.oem`** and load our HAL as the OEM `Computer`. Success is
   Setup running with a HAL that never came off the disc. At this point the CD is pristine and
   ledger row 8 is retired.
4. **C5**, the ADB driver, which is what makes the floppy image redistributable.
5. **C6**, `nvramrc`, which removes the remaining typing.
6. Only then revisit the single-file/browser idea, which becomes a ~1.4 MB download.

## 7. Keeping the door open to other TNT models

A 7500/8500 has no NT ROM, and E9 says exactly what that costs: **`pe-loader` is missing, and
nothing else is**. `little-endian?`, `xcoff-loader`, `fat-files`, `iso-9660-files`, `disk-label`
and `mac-parts` are all present on a plain ROM. So the CPU-mode switch — the part that sounds
hardest — is not ANS-specific at all.

The consequence is blunt: **patching `VENEER.EXE` is structurally ANS-only.** The veneer is a
file on the CD and travels fine, but without `pe-loader` nothing can lay it out or start it.
Every hour spent on rows 1–5 buys nothing on an 8500.

What does travel is a **replacement ARC firmware** started through `xcoff-loader` (present
everywhere) or via BootX from an HFS partition. maciNTosh's `arcbandit` is already that shape and
already targets this chipset — `STORY.md` wall 26 records that its `pxi.c` / `adb_bus.c` /
`adb_kbd.c` are for the hardware in front of us.

**Design rules so this plan does not have to be redone:**

* Keep everything that is *ours* free of veneer assumptions. C1, C2, C5 and the HAL are all
  equally valid under a replacement firmware.
* Treat the ARC path a device is reached by as a parameter, never a constant — `mkcoldboot.py`
  already has `--veneer-dev` and `--cd-dev` for this reason.
* Prefer fixing a problem in *our* code over patching the veneer, even when the patch is
  smaller. A veneer patch is a dead end outside the ANS.
* If §4.1 says the veneer must be extended, weigh that against starting §7 instead: teaching a
  proprietary binary to enumerate SWIM3 is work that cannot be reused, and a replacement
  firmware would have to implement the same enumeration anyway — once, portably.

## 8. Risks and open questions

* **R1 — the blocker.** §4.1 answered: the veneer enumerates the drive but classifies it `other`
  and `VrOpen` returns `EIO`. Whether that is a small patch or a wall depends on §4.2. The choice
  remains veneer surgery (ANS-only) or §7 (large, portable), and it is what the plan hinges on.
* **R2 — can SETUPLDR read FAT?** E3 shows it parses its boot device's filesystem itself. On a
  floppy that is FAT12. `fastfat.sys` in its string table is a file it *loads*, not proof it
  *contains* a reader. If it cannot, the floppy must present something it can read, or the boot
  must stay on the CD and only the OEM disk move to the floppy.
* **R3 — does `setupdd` re-read the OEM disk under NT?** E6's `\device\floppy0\txtsetup.oem` is
  an NT path needing an NT driver, and no SWIM3 driver exists for NT. If that read is
  unconditional, an NT-side SWIM3 driver becomes mandatory; if it is a fallback for classes
  SETUPLDR already resolved, it may never happen. Answerable by disassembling around the string.
* **R4 — OEM `Computer` semantics.** Whether `txtsetup.oem` can *replace* the HAL that
  `TXTSETUP.SIF` names, or only add a menu entry, is untested.
* **R5 — nothing here has run on real hardware.** Every row in §3 is emulator evidence. The
  tinkerdifferent threads show real ANS machines failing differently from ours.
* **R6 — `usbadb.sys` is not redistributable.** Until C5, the floppy image is as unshippable as
  the patched ISO, and the main benefit is unrealised.

## 9. Three corrections worth carrying forward

Both cost real time today and both were mine.

**A prefix-only signal reports "absent" and "never ran" identically.** Two experiments looked
like findings and were infrastructure: the ARC-tree test that reported zero floppy nodes had a
dead daemon, and the run after it failed because rebuilding the emulator invalidated the
checkpoint (`checkpoint.c` refuses a build-ID mismatch; `tools/restamp-ckpt.py` fixes it when no
checkpointed structure changed). Count the *sentinel* as well as the thing you are looking for.

**A measurement is only as good as the condition it was taken under.** E7 was first written as
"the veneer does not enumerate the floppy", full stop, from a run where `bootpath` pointed at the
hard disk. With `bootpath` on the drive the node appears immediately (E7b). The observation was
correct and the generalisation was wrong, and it was the generalisation that went into the plan —
where it would have sent the next person to rewrite a firmware they did not need to touch. State
the condition beside the result, especially when the result is a negative.

**`bad address to DMA-MAP-IN` was my bug, twice.** `map-space` only exists inside
`dev /packages/pe-loader`. Issued outside it, it is an unknown word, the buffer is never mapped,
and the read fails in a way that looks exactly like a firmware limitation. The floppy reads
perfectly once it is mapped — and the earlier conclusion that the CD cannot be read with
`read-blocks` deserves re-testing for the same reason.

## Appendix A — reference tables

**`VrDebug`**, a word at veneer image `0x60C0C`. Poke it with the little-endian address munge:
`machine.memory.poke.l 0x60c0c <value>`. Documented nowhere else we could find; the full table is
in [`2026-09-07-booting-the-installed-disk.md`](2026-09-07-booting-the-installed-disk.md) §9.

| bit | traces |
|---|---|
| `0x0001` | `VrGetChild`, `VrGetPeer`, `VrGetParent`, `VrGetComponent`, `VrGetConfigurationData` |
| `0x0008` | **the OBP → ARC device-tree conversion** — where a device gets its ARC type |
| `0x0010` | memory descriptors |
| `0x0020` | `main`, `parse_args`, `find_boot_dev`, the boot file and OsLoader paths |
| `0x0040` | the `Vr*Initialize` phases, `select_boot`, `choose_args` |
| `0x0100` | `ArcPathToNode`, `NodeToPath` |
| `0x0200` | `VrOpen`, `VrClose`, `VrMount`, `VrGetFileInformation`, `VrGetDirectoryEntry` |
| `0x0800` | `VrLoad` |
| `0x1000` | `VrRead`, `VrWrite`, `VrSeek`, `VrGetReadStatus` |
| `0x2000` | `Argv[n]` — the argv actually handed to the loader |
| `0x4000` | `VrGetEnvironmentVariable`, `VrSetEnvironmentVariable`, `GetEnvVar`, `FindInLocalEnv` |

**ARC status codes**, in the order the ARC specification defines them — this is how `VrOpen
returned 8` becomes `EIO`. Corroborated by wall 22, where `VrOpen returned d` (13) was `ENODEV`.

```
 0 ESUCCESS   1 E2BIG   2 EACCES   3 EAGAIN   4 EBADF   5 EBUSY   6 EFAULT   7 EINVAL
 8 EIO        9 EISDIR 10 EMFILE  11 EMLINK  12 ENAMETOOLONG     13 ENODEV  14 ENOENT
15 ENOEXEC   16 ENOMEM 17 ENOSPC  18 ENOTDIR 19 ENOTTY 20 ENXIO  21 EROFS   22 EMAXIMUM
```

## Appendix B — traps in the emulator's script language

Each of these cost at least one run.

* **`$` in a line typed at the firmware is spliced as a shell binding.** `$call-method` becomes
  `$ca` and the line dies. Escape it: `\$call-method`.
* **Wait for the prompt `0 > `, not for `" ok"`.** The firmware's narration is full of `ok`, and
  so is whatever is still in the receive buffer, so a wait on `" ok"` returns instantly and the
  next stage is typed into a machine that is still rebooting. Drain the buffer first.
* **`map-space` only exists inside `dev /packages/pe-loader`.** Outside it the word is unknown,
  the buffer is never mapped, and the read fails `bad address to DMA-MAP-IN` — which reads
  exactly like a firmware limitation and is not one.
* **`machine.memory.poke.l A` writes the guest word at `A ^ 4`**, and `poke.b A` the guest byte at
  `A ^ 7` — the 604's little-endian address munge. `DAR` and blue-screen addresses are munged too.
* **`screen.checksum()` with no arguments hashes `stride × height`**, including off-screen video
  memory the display driver uses as scratch. It churns while the picture is still. Pass the
  visible region.
* **The SCC receive FIFO holds about sixteen characters.** Type long lines four at a time with the
  machine running in between.
* **An inline `if` block must be one statement on one line.** Multi-line needs `{` last and `}`
  first on their lines.

## Appendix C — glossary

| term | meaning |
|---|---|
| **veneer** (`\PPC\VENEER.EXE`) | Microsoft's ARC firmware shim, a file *on the NT CD*. Translates NT's ARC firmware interface into Open Firmware calls. Not part of the ROM, not part of NT |
| **ARC** | the Advanced RISC Computing firmware standard NT expects: a device tree, paths like `multi(0)scsi(0)cdrom(0)fdisk(0)`, and environment variables |
| **`pe-loader`** | an Open Firmware package that relocates and starts a PE/COFF client program. **Only in the NT ROM** — see §7 |
| **SETUPLDR** | NT's text-mode Setup loader. Runs before the kernel, does all its I/O through ARC, and parses its boot device's filesystem itself |
| **`setupdd.sys`** | the Setup driver that runs *after* NTOSKRNL, using NT I/O |
| **HAL** | the Hardware Abstraction Layer — what this project builds, from source. Not a patch |
| **miniport** | the hardware-specific half of an NT driver, sitting on a generic port driver (`scsiport`, `videoprt`). Stock Microsoft miniports drive this machine because the HAL answers their abstract questions |
| **the ledger** | the table of workarounds at the end of [`../STORY.md`](../STORY.md): what each one is, why it is not a fix, and what a real fix would be |
| **wall *n*** | a numbered obstacle in `STORY.md`'s narrative; the walls referenced here are 22, 26, 27, 46 and 52 |
