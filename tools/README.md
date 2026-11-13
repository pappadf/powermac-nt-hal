# tools — build, deliver and run the HAL on the emulated Network Server

Everything here talks to a stock Granny Smith headless emulator over its TCP shell; nothing
links against it. Setting the emulator up is [`../docs/EMULATOR.md`](../docs/EMULATOR.md).

| tool | what it does |
|---|---|
| `elf2pe.py` | lld ELF (PowerPC LE, `--emit-relocs`) → NT PowerPC PE with descriptors, imports (IAT pre-filled with the name RVAs, as NT's boot loader requires), base relocations, checksum |
| `mkstubs.py` | generates `imports.S` (IAT slots + descriptor-call stubs) and `exports.S` (function descriptors) from `hal.imports` / `hal.exports` |
| `pe-exports.py` | prints machine, sections, exports and import counts of any NT PE (used to read the shipped HALs) |
| `gsh.py` | minimal TCP client for the emulator daemon (`GS_PORT`, `GS_TIMEOUT`, `GS_IDLE`) |
| `ppcdis.py` | a PowerPC disassembler, enough of one to read an NT 4.0 PowerPC PE. Vendored from this project author's own research tools so `pe-dis.py` works from a clean checkout (see PROVENANCE.md); imported, not run directly |
| `pe-dis.py` | PowerPC disassembler for an NT PE, annotating branch targets, imports and TOC-relative loads (`TOC=<hex>` names the anchor; take it from the entry descriptor's second word) — this is how the kernel's and the shipped drivers' structure layouts were read |
| `pe-fn.py` | disassembles one routine of an NT PowerPC PE by **export name** (or by the hex address of a descriptor, e.g. a driver's entry point): resolves the `{entry, toc}` descriptor and runs `pe-dis.py` from the entry with that TOC. `pe-fn.py ntoskrnl.exe IoCreateDevice 700` is how `DEVICE_OBJECT` was read |
| `check-nt-layouts.py` | checks every NT structure layout in `drivers/adbport/adbport.h` against the code of a shipped kernel and of `kbdclass.sys`, `mouclass.sys`, `i8042prt.sys` and `floppy.sys`, as instruction patterns; 21 rows, each naming what it proves. Run it on the binaries of any build other than 1381 before trusting the driver there ([`docs/2026-11-05-adbport-layouts-verified.md`](../docs/2026-11-05-adbport-layouts-verified.md)) |
| `run-hal.py` | from a checkpoint at Setup's computer-type menu: picks *MOTOROLA PowerStack*, Enter at mass storage, the Cirrus entry at video, then runs the kernel; `--save` keeps the end state, `--screenshot` grabs the monitor. Diagnosis: `--scsi-log`; `--poke addr=value` right after the checkpoint load (veneer patches); `--bp <va>` breakpoints, with `--peek <va \| r1+0x9bc>`, `--deref <addr>` (follow the pointer there and dump 12 words) and `--onbreak "<statement>"` (any shell statement, e.g. a poke into code that only exists once the kernel has loaded it) reported at every stop; `--adb-key <text>` types it on the emulated ADB keyboard once per kernel-phase chunk, which is how the ADB path was proved end to end; `--computer-ups N` says how many Up presses reach the computer type to select (5 = MOTOROLA PowerStack on a stock CD, 3 = the slot `mkoem.py` repurposes); `--adb-then <text|#code>` types a key and screenshots after it, repeatable, for walking Setup screen by screen (`#` prefixes a raw ADB key code: `#121` Page Down, `#100` F8); `--disk-delta <delta>=<raw.img>` splices a prepared disk image into a writable image's copy-on-write delta after the checkpoint load |
| `mkveneer.py` | (history: the veneer is no longer used) bakes ledger rows 1-5 into `VENEER.EXE` as **file** patches instead of memory pokes: the two `claim` `nop`s, `OFClose`'s nret, the blanked `partition(1)`/`:0` strings, the `NCR,53C810` -> `NCR,825A` SCSI identifier, and (with `--for cd`) `VrOpen`'s whole-device route, which is wrong for a disk. Every site's shipped word is verified before it is written, so it refuses an image these addresses were not derived from. `--stage img[@block]` also writes the result to a raw disk at the block the ROM's `pe-loader` reads from (default `0x800`). The point is a cold boot that needs only the four firmware steps and no pokes — `CHARTER.md` §3.1's "better answer than 'type these into Open Firmware'" |
| `mkfloppy.py` | **the one command**: `make floppy` runs it. Lays out the floppy (`mkbootfloppy.py`) with the HAL, the ADB driver and the ARC firmware `make` built; nothing comes off any CD since 5 November 2026 (`--iso` is accepted and ignored) |
| `isocat.py` | reads one file out of an ISO 9660 image by path; unpacks the single-file Microsoft Cabinet NT stores `CIRRUS.DL_` in, so the caller asks for `CIRRUS.DLL` |
| `savedisk.py` | lays the emulator's copy-on-write delta (the store's `GSDL` layout) over a base image to capture an installed disk; `--check` walks the FAT and reports every file whose cluster chain is shorter than its size |
| `mkbootfloppy.py` | builds the 1.44 MB FAT12 floppy of [`docs/2026-09-15-the-boot-floppy.md`](../docs/2026-09-15-the-boot-floppy.md): this project's ARC firmware as `ARCFW.XCF` (`--arc-firmware`; an XCOFF for the ROM's `xcoff-loader`, which both the production 1.1.22 ROM and the 2.26NT ROM have; a `.exe` raw COFF is placed as `ARCFW.EXE` and started through the 2.26NT ROM's `pe-loader` instead; both `BOOT.OF` and `BOOTDISK.OF` read it; `--veneer`/`--disk-veneer` are the old Microsoft route), its HAL as `HALSHINR.DLL`, its ADB/OEM-disk driver `ADBPORT.SYS` (`build/adbport.sys` by default, offered to Setup under `[SCSI]`), optionally a display pair (`--display-driver`/`--display-dll`; no longer needed since the HAL translates the legacy VGA window), a `txtsetup.oem`, and three Open Firmware scripts — `SETUP.OF` (once per machine), `BOOT.OF` (every boot, to install) and `BOOTDISK.OF` (to start the installed system) — which take the typing from twenty-eight lines to two. The veneer is allocated **first**, so it starts at cluster 2 and is contiguous for `read-blocks`. `BOOT.OF` also reads the *whole* disk into RAM and writes the header `include/oemdisk.h` describes, which is how the OEM files reach Setup's copy phase under NT (NT has no SWIM3 driver). Every Microsoft file is named on the command line and comes from the user's own media |
| `mkarcdisk.py` | writes an MBR and an empty FAT16 system partition onto a raw disk image — what an ARC machine's `ARCINST.EXE` would do, which the Network Server's Open Firmware has no equivalent of. Setup will not start without a FAT partition with 750 KB free on the boot disk |
| `restamp-ckpt.py` | rewrites a checkpoint's 20-byte build ID so a rebuilt emulator will load it. Only when the rebuild changed no checkpointed structure — but then it turns a ten-minute cold boot back into a two-minute iteration |
| `gen-probe.py` | the phase 0 menu probes (see `docs/2026-09-05-phase0-probes.md`) |
| `coffsyms.py` | reads a **raw NT PowerPC COFF** image's header, sections and symbol table. `VENEER.EXE` and `OSLOADER.EXE` are not PEs — no MZ stub — and both shipped with their symbol tables intact (1,512 and 2,135 symbols, including every string constant's mangled `??_C@` name). `pe-dis.py` cannot open them; this can. Importable as `Coff` |
| `coffdis.py` | `pe-dis.py`'s counterpart for those two: disassembles a COFF image, labelling functions and branch targets from the symbol table. How `VrOpen`, `BlOsLoader` and `BlLoadSystemHive` were read (see `docs/2026-09-07-booting-the-installed-disk.md`) |
| `fatls.py` | lists the FAT16 partitions of an MBR disk image and walks their directories, from the host, with nothing mounted — `mount -o loop` is unavailable in a container and `mtools` is not worth a dependency. How an install is checked after the fact |
| `fatcat.py` | extracts one file from such an image (this is how `\OS\WINNT40\HAL.DLL` was confirmed byte-identical to `build/hal.dll`) |
| `fatput.py` | replaces one file's contents, **growing or shrinking its cluster chain**. Two jobs: restoring a hive from its own `.SAV` copy the way NT's repair option does (wall 48), and dropping a freshly built `hal.dll` onto an installed system — which is the iteration loop for everything after text-mode Setup, since a rebuilt HAL is never the size Setup copied |

## The delivery loop

1. Build: `make` → `build/hal.dll`, `build/adbport.sys`, `build/arcfw.xcf` (the ARC firmware inside `src/stage0`, its big-endian front end).
2. Floppy: `make floppy` → `build/boot-floppy.img`. Rebuild it whenever any of the three
   changes; the copies on the floppy are the ones Setup and the firmware load.
3. Disk: `make disk` → `build/nt-disk.img`, once.
4. Run: the three Open Firmware lines in [`../docs/HOWTO.md`](../docs/HOWTO.md). The CD is
   attached read-only and never written.

**Iterating on an installed system** without re-running Setup: the loader takes `HAL.DLL` from
the ARC system partition (partition 1), so `fatput.py <disk.img> 1 '\OS\WINNT40\HAL.DLL'
build/hal.dll` drops a rebuilt HAL onto it; `fatput.py` grows or shrinks the cluster chain.
`savedisk.py` captures the emulator's delta over the base image when a run is worth keeping.

**Reading the shipped binaries.** `pe-exports.py` for a PE's exports and imports, `pe-dis.py` for
its code (give it the TOC from the entry descriptor's second word), `coffsyms.py` and `coffdis.py`
for `VENEER.EXE` and `OSLOADER.EXE`, which are raw COFF images with their symbol tables intact.
This is how every NT structure layout in `include/nt.h` and `drivers/adbport/adbport.h` was read:
from the shipped code's own field offsets, never from a header.

Gotchas met on the way: keys go one byte per `machine.scc.a.receive`; `machine.memory.peek/poke`
are physical and little-endian mode puts the word at A at `A ^ 4`; a breakpoint makes the whole
run single-step, so a diagnostic run takes about ten minutes rather than two; and a checkpoint
records the build ID of the emulator that wrote it and is refused by any other build
(`restamp-ckpt.py`).

The tools of the patched-CD era, which edited the user's disc instead of using a floppy, are in
[`attic/`](attic/README.md).
