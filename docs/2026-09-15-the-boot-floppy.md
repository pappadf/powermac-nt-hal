<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright (C) 2026 powermac-nt-hal contributors -->

# The boot floppy

*15 September 2026. A plan to stop patching the user's Windows NT CD. Everything this project
adds — the HAL, the veneer's seven patch sites, the ADB keyboard driver — moves onto a floppy,
and the CD is used unmodified, as pressed. Targeting the Apple Network Server first, but built so
the same work reaches a 7500/8500 later instead of being thrown away.*

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
   boot device [E3].
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
| E7 | **The veneer does not enumerate the floppy** | Full ARC dump (31 `dump_node` entries) with a floppy inserted: exactly **one** `FloppyDiskPeripheral`, `Parent → CdromController` — it is the `fdisk(0)` tail of the CD path. Identical with and without a floppy present |
| E8 | The emulator models the hardware already | `ans500.c` declares `.floppy_slots = tnt_floppy_slots` ("Internal FD0", `FLOPPY_HD`); `tnt.c` binds SWIM3 to Grand Central and DBDMA. Insert works and survives `reset-all` |
| E9 | `pe-loader` is the **only** NT-ROM-specific package | `ans-2.26NT` packages: deblocker, disk-label, obp-tftp, mac-files, mac-parts, aix-boot, fat-files, iso-9660-files, xcoff-loader, **pe-loader**, terminal-emulator. Plain `ans-1.1.22`: the same list **minus `pe-loader`**. `little-endian?` exists in both |
| E10 | `nvramrc` exists on this firmware | `printenv` shows `use-nvramrc? false`, `auto-boot? true`, `boot-command boot`, and `nvramrc` |

## 4. The blocker

**E7 is the whole problem.** Open Firmware can reach the drive (E1, E2) and SETUPLDR would
happily use it (E3), but the ARC translation layer between them never advertises it. SETUPLDR
addresses devices by ARC path, and `multi(0)disk(0)fdisk(0)` does not exist because the veneer
does not build the node.

Everything else in §2 is proven or strongly evidenced. This one step is not.

### 4.1 The next experiment, before any code is written

We have never pointed `/chosen bootpath` **at the floppy**. The veneer builds its tree from what
it finds, and it created a `CdromController` for the CD; it may create a floppy node when the
boot path names one. That is a one-run test and it decides the whole shape of the project:

```
" /bandit/gc/swim3" encode-string " bootpath" _chosen (property)
go            \ then dump the ARC tree and count FloppyDiskPeripheral nodes
```

* **If a floppy node appears** — the plan in §2 is achievable with the veneer we already patch,
  and §6 is mostly packaging work.
* **If it does not** — the veneer must be taught to enumerate SWIM3, which is new functionality
  inside a proprietary binary rather than a seven-site patch, and §7 becomes the main line.

## 5. What has to be built

| # | component | what it is | depends on |
|---|-----------|------------|-----------|
| C1 | `tools/mkbootfloppy.py` | writes the 1.44 MB image: FAT12, `\PPC\SETUPLDR`, the patched veneer, `HALSHINR.DLL`, `I8042PRT.SYS`, `txtsetup.oem` | `mkveneer.py` (exists) |
| C2 | `txtsetup.oem` | the OEM description: `[Disks]`, `[Defaults]`, `[Computer]` naming our HAL | E4; format confirmed, contents untested |
| C3 | floppy-aware cold boot | `mkcoldboot.py --veneer-dev /bandit/gc/swim3`, reading `0x13C` blocks from block 0 | already parameterised (`--veneer-dev`) |
| C4 | veneer floppy enumeration **or** §7 | whichever §4.1 selects | §4.1 |
| C5 | our own ADB port driver | replaces maciNTosh's `usbadb.sys`, the last non-shippable piece | `entii-for-workcubes` `fpsidrv`; HAL half exists |
| C6 | `nvramrc` installer | the §2 block as one line, so the machine boots the floppy unattended | E10; one-line form untested |

## 6. Order of work

1. **Run §4.1.** One boot. Nothing else is worth starting until it is answered.
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

* **R1 — the blocker.** §4.1 may say no. Then the choice is veneer surgery (ANS-only) or §7
  (large, portable). This is the decision the plan hinges on.
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

## 9. Two corrections worth carrying forward

Both cost real time today and both were mine.

**A prefix-only signal reports "absent" and "never ran" identically.** Two experiments looked
like findings and were infrastructure: the ARC-tree test that reported zero floppy nodes had a
dead daemon, and the run after it failed because rebuilding the emulator invalidated the
checkpoint (`checkpoint.c` refuses a build-ID mismatch; `tools/restamp-ckpt.py` fixes it when no
checkpointed structure changed). Count the *sentinel* as well as the thing you are looking for.

**`bad address to DMA-MAP-IN` was my bug, twice.** `map-space` only exists inside
`dev /packages/pe-loader`. Issued outside it, it is an unknown word, the buffer is never mapped,
and the read fails in a way that looks exactly like a firmware limitation. The floppy reads
perfectly once it is mapped — and the earlier conclusion that the CD cannot be read with
`read-blocks` deserves re-testing for the same reason.
