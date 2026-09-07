# tools — build, deliver and run the HAL on the emulated Network Server

Everything here talks to a stock Granny Smith headless emulator over its TCP shell; nothing
links against it. The reference emulator build is noted in [`../docs/CHARTER.md`](../docs/CHARTER.md) §3.

| tool | what it does |
|---|---|
| `elf2pe.py` | lld ELF (PowerPC LE, `--emit-relocs`) → NT PowerPC PE with descriptors, imports (IAT pre-filled with the name RVAs, as NT's boot loader requires), base relocations, checksum |
| `mkstubs.py` | generates `imports.S` (IAT slots + descriptor-call stubs) and `exports.S` (function descriptors) from `hal.imports` / `hal.exports` |
| `pe-exports.py` | prints machine, sections, exports and import counts of any NT PE (used to read the shipped HALs) |
| `patch-iso.py` | writes `hal.dll` into an ISO 9660 image over an existing file's extent, in place |
| `gsh.py` | minimal TCP client for the emulator daemon (`GS_PORT`, `GS_TIMEOUT`, `GS_IDLE`) |
| `ppcdis.py` | a PowerPC disassembler, enough of one to read an NT 4.0 PowerPC PE. Vendored from this project author's own research tools so `pe-dis.py` works from a clean checkout (see PROVENANCE.md); imported, not run directly |
| `pe-dis.py` | PowerPC disassembler for an NT PE, annotating branch targets, imports and TOC-relative loads (`TOC=<hex>` names the anchor; take it from the entry descriptor's second word) — this is how the kernel's and the shipped drivers' structure layouts were read |
| `run-hal.py` | from a checkpoint at Setup's computer-type menu: picks *MOTOROLA PowerStack*, Enter at mass storage, the Cirrus entry at video, then runs the kernel; `--save` keeps the end state, `--screenshot` grabs the monitor. Diagnosis: `--scsi-log`; `--poke addr=value` right after the checkpoint load (veneer patches); `--bp <va>` breakpoints, with `--peek <va \| r1+0x9bc>`, `--deref <addr>` (follow the pointer there and dump 12 words) and `--onbreak "<statement>"` (any shell statement, e.g. a poke into code that only exists once the kernel has loaded it) reported at every stop; `--adb-key <text>` types it on the emulated ADB keyboard once per kernel-phase chunk, which is how the ADB path was proved end to end; `--adb-then <text|#code>` types a key and screenshots after it, repeatable, for walking Setup screen by screen (`#` prefixes a raw ADB key code: `#121` Page Down, `#100` F8); `--disk-delta <delta>=<raw.img>` splices a prepared disk image into a writable image's copy-on-write delta after the checkpoint load |
| `mkarcdisk.py` | writes an MBR and an empty FAT16 system partition onto a raw disk image — what an ARC machine's `ARCINST.EXE` would do, which the Network Server's Open Firmware has no equivalent of. Setup will not start without a FAT partition with 750 KB free on the boot disk |
| `restamp-ckpt.py` | rewrites a checkpoint's 20-byte build ID so a rebuilt emulator will load it. Only when the rebuild changed no checkpointed structure — but then it turns a ten-minute cold boot back into a two-minute iteration |
| `gen-probe.py` | the phase 0 menu probes (see `docs/2026-09-05-phase0-probes.md`) |

## The delivery loop (emulator)

1. Build: `make` → `build/hal.dll`.
2. The test ISO is the user's NT 4.0 PowerPC CD with `\PPC\HALEAGLE.DLL` overwritten by
   `hal.dll` (`patch-iso.py <cd.iso> <test.iso> PPC/HALEAGLE.DLL hal.dll`, then in place on
   later iterations). Setup loads it when *MOTOROLA PowerStack* is chosen; no `TXTSETUP.SIF`
   edit is needed for the emulator loop.
3. **Know that a checkpoint freezes the CD.** Granny Smith's consolidated checkpoints snapshot
   every attached image, and a restore fills the read-only image's scratch delta
   (`/tmp/gs-image-ro/<id>.delta`) with all blocks flagged, so the guest reads the CD as it was
   at save time. `run-hal.py --delta-patch hal.dll` therefore loads the checkpoint, writes the
   new HAL into the restored delta at the file's extent, and only then presses the keys.
4. Make the menu checkpoint once (cold boot, ~10 min): the emulator project's
   `repro-hal-menu` recipe with the CD path changed, the veneer's `OFClose` fix poked in before
   `go`, and `checkpoint.save` when *Select the computer type* appears.
5. Iterate: `patch-iso.py` in place → `run-hal.py --ckpt <menu.ckpt> --iso <test.iso> --out log`
   (~2 min at turbo speed, ~10 with a breakpoint). The cleaned console lands in `log.txt`.

**Delivering a driver, not just the HAL.** SETUPLDR loads the keyboard by *file name*, outright,
so a keyboard driver goes onto the CD over `\PPC\I8042PRT.SYS` and `[files.i8042]` in
TXTSETUP.SIF goes back to `i8042prt.sys,4`. TXTSETUP.SIF is parsed long before the menu
checkpoint, so changing it needs a cold boot from `nt-pre-go.ckpt` with the delta patched there
(`tmp/hal/run-mkkbd.py`); the driver file itself can be delta-patched per run like the HAL.
**Pad to the slot and recompute the PE checksum**: SETUPLDR checksums over the length in the ISO
directory record, so a short file plus zero padding is *"The file … is corrupted"*.

Gotchas met on the way: keys go one byte per `machine.scc.a.receive`; the daemon's
`machine.scsi` name is shadowed by a static helper object, so the CD cannot be swapped live
(`machine.scsi.device[N].eject/insert` is unreachable); `machine.memory.peek/poke` are physical
and little-endian mode puts the word at A at `A ^ 4` (`(va & 0x7fffffff) ^ 4` reaches both KSEG0
and the low addresses SETUPLDR's stack lives at, but *not* paged-pool addresses like
`0xEE5xxxxx`, which nothing here can read); a breakpoint makes the whole run single-step, so a
diagnostic run takes about ten minutes rather than two; and a checkpoint records the build ID
of the emulator that wrote it and is refused by any other build (`restamp-ckpt.py`).
