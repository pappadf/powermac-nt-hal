# Provenance log

Every file or function in this repository that is not original work of this project has a
row here, added when it was copied or adapted, never reconstructed later. The project's
license follows from this table (README §13). Rows are never deleted; a removal is a new row.

| path in this repo | origin (repo, path, commit) | license | extent (verbatim / adapted / rewritten from reading) | date | who |
|---|---|---|---|---|---|
| `include/nt.h` (KPCR, KPRCB, LOADER_PARAMETER_BLOCK, CONFIGURATION_COMPONENT, CM_PARTIAL_RESOURCE_DESCRIPTOR, HAL enums, IRQL/vector constants) | Microsoft NT 4.0 DDK public interface as reproduced in Wack0/entii-for-workcubes `nt4/hal/{arc.h,hal.h,halppc.h,ppcdef.h}` and `halartx/source/nthal.h` (commit c9b041da, 2026-02-26) | interface facts (DDK-documented structure layouts); host files GPL-2.0 / Microsoft | **rewritten from reading**: layouts re-declared in our own words with static asserts; no text copied | 2026-09-06 | pappadf using Claude |
| `src/irql.c`, `ints.c` (IRQL-to-mask model, external-interrupt dispatch through `PCR->InterruptRoutine`, the `KINTERRUPT`-from-dispatch-code trick), `clock.c` (decrementer reload, `KeUpdateSystemTime` call, `KeSetTimeIncrement` use), `init.c` (phase 0/1 order, PRCB version check), `thunk.S` (descriptor-call idea), `dma.c`/`misc.c` (which routines may be stubs) | Wack0/entii-for-workcubes `halartx/source/{irql.c,ints.c,clock.c,init.c,hwsup.c,bushnd.c,kd.c,stall.s,cache.s}` (commit c9b041da) | GPL-2.0 | **rewritten from reading**: the structure and the kernel-contract facts come from reading these files; the code was written fresh for Grand Central/Bandit/ESCC; no lines copied | 2026-09-06 | pappadf using Claude |
| `hal.exports` | export table of the shipped `HALEAGLE.DLL` (NT 4.0 PowerPC CD, read with `tools/pe-exports.py`) | Microsoft binary, read not copied | list of 66 public names | 2026-09-06 | pappadf using Claude |
| `src/vga.c` (Cirrus 54M30 text console: VGA/Cirrus mode-set, DAC palette, OEM-font glyph blit) | original, written from the Cirrus GD543x manual (register facts), the emulator's 54M30 model (which registers the scanout derives from), and the NT OEM-font layout read from halartx `display.c` (glyph Map[]/column-major bytes) | own work + hardware-doc facts; layout rewritten from reading GPL-2.0 halartx | original | 2026-09-06 | pappadf using Claude |
| `src/pci.c` (Bandit config-port access, IDSEL encoding, bus-address translation, ANS interrupt-line routing, `HalAssignSlotResources` from BAR sizing + interrupt routing), `include/nt.h` (CM_RESOURCE_LIST / CM_PARTIAL_RESOURCE_DESCRIPTOR layout, NT pshpack4), `src/dma.c`, `src/clock.c`, `src/display.c`, `src/misc.c`, `src/arc.c`, `src/thunk.S`, `include/ans.h` | original, written from Apple's Network Server Hardware Developer Notes (§4 address/interrupt map, §4.6.2 IDSEL), the TNT dossier's Grand Central and Bandit register maps, the Zilog Z8530 manual, the MPC604 manual, and the observed ARC tree; no code copied | own work + hardware-doc facts | original | 2026-09-06 | pappadf using Claude |
| `tools/elf2pe.py`, `mkstubs.py`, `patch-iso.py`, `gsh.py`, `run-hal.py`, `pe-dis.py`, `pe-exports.py`, `restamp-ckpt.py` | original tools (the NT PowerPC PE layout facts from the shipped HALs read with `pe-exports.py`; the ISO-9660 and PE structures are public formats; the checkpoint header field is this repository's own format) | own work | original | 2026-09-06 | pappadf using Claude |
| `include/nt.h` (WAIT_CONTEXT_BLOCK, ARC_DISK_SIGNATURE, IO_ALLOCATION_ACTION, the hidden-pointer rule for eight-byte return values), `src/dma.c` (`HalAllocateAdapterChannel`, `IoMapTransfer` signature) | field offsets read out of the shipped `NTKRNLMP.EXE`'s own code with `tools/pe-dis.py`: `IoAllocateAdapterChannel` (image VA `0x368a4`) writes the WCB, `IopCreateArcNames` (`0x139dc0`) walks the signature list, `MmGetPhysicalAddress` (`0x5038c`) reads its argument from `r4` | Microsoft binary, read not copied; the layouts are DDK-documented interface facts | **rewritten from reading**: offsets observed, declarations written here in our own words | 2026-09-06 | pappadf using Claude |
| `src/cuda.c` (Cuda transport over Grand Central's VIA; `HalPxiCommandAdb`, `HalPxiAdbSetCallback`, `HalPxiAdbAutopoll`) | the handshake sequence from MCJack123/maciNTosh-bandit `arcbandit/source/pxi.c` (GPL-2.0, commit bafef57) — the same silicon — which is itself 95% Wack0/maciNTosh's `arcgrackle/source/pxi.c` (208 of its 268 substantive lines identical; the fork's contribution is the Bandit port, i.e. Cuda at Grand Central +0x16000, which is the part that applies to this machine). The three entry points' names and prototypes come from `inc/halpxi.h`, which is **byte-identical** between the two trees and so is Wack0/maciNTosh's file; their exact semantics read out of maciNTosh's `halgoss.dll` binary with `tools/pe-dis.py` (`HalPxiCommandAdb` at image VA `0x1845c`, the callback dispatcher at `0x17bf4`) and cross-checked against `usbadb.sys`'s own callback (`0x102a0`) | GPL-2.0 (source); binaries read, not copied, and never redistributed | **rewritten from reading**: the protocol steps follow `pxi.c`, the code is written fresh with bounded waits, HAL-side interrupt ownership and this machine's addresses; no lines copied | 2026-09-06 | pappadf using Claude |
| `tools/ppcdis.py` | this project author's own PowerPC disassembler, written for reading an AIX kernel extension, vendored from a private research tree so `tools/pe-dis.py` works from a clean checkout | own work, relicensed here as GPL-2.0-only with the rest of the tools | verbatim | 2026-09-07 | pappadf using Claude |
| `src/thunk.S` cache sweeps | MPC604 User's Manual (cache operations); `halartx/source/cache.s` read for the shape of the loops | Motorola document; GPL-2.0 | rewritten | 2026-09-06 | pappadf using Claude |

**A note on `include/nt.h` and interface-mandated overlap.** A line-level comparison against
`entii-for-workcubes` (which vendors Microsoft's NT 4.0 DDK headers under `nt4/`) finds about
thirty identical normalised lines in `nt.h`: structure open/close lines and field declarations
such as `LARGE_INTEGER StartingOffset;` and `} LOADER_PARAMETER_BLOCK, *PLOADER_PARAMETER_BLOCK;`.
That overlap is unavoidable and expected — a HAL that the NT kernel can bind to must declare the
same structures with the same field names, or it does not link and does not run. Every layout
here was re-declared in this project's own words from the documented interface and pinned with
`_Static_assert`; none of it was copied, and no DDK header is vendored in this repository. The
same comparison across the HAL's C files finds no upstream file above 22% token overlap, and the
identical lines are single API calls (`KiDispatchSoftwareInterrupt();`) or one-line mandatory
stubs (`VOID KdPortSave(VOID) { }`) — which is what "rewritten from reading" looks like when it
is measured rather than asserted.

**Consequence (README §13):** because `irql.c`, `ints.c`, `clock.c` and `init.c` are derived by reading GPL-2.0 `halartx` sources, this project is **GPL-2.0-only** unless those four files are later rewritten from the DDK contract alone. Every source file carries `SPDX-License-Identifier: GPL-2.0-only`.

## Upstream candidates and their licenses (checked 2026-09-05 via the GitHub API)

| repo | license | role |
|---|---|---|
| https://github.com/Wack0/entii-for-workcubes | GPL-2.0 | intended HAL template (`halartx/`), toolchain bits (`msvc-ppc/`), build rules |
| https://github.com/Wack0/peppc | GPL-2.0 | compiler (tool; does not license our output) |
| https://github.com/MCJack123/maciNTosh-bandit | GPL-2.0 | ARC firmware/loader source for Bandit machines, **including Cuda/ADB on Grand Central** (`arcbandit/source/{pxi.c,adb_bus.c,adb_kbd.c}`) and `inc/halpxi.h`, the HAL↔driver ADB contract. No NT binaries in the repo (`boot_files/boot.img` is the ARC loader's HFS volume) and no releases; the NT HAL and drivers ship in the *parent* repo's release `drivers.img` — run locally, never redistribute (GPL-2.0 binaries with no published source) |
| https://github.com/Wack0/maciNTosh | GPL-2.0 | parent project; "NT HAL and drivers have no source present for now" |

## Not to be used

- Microsoft NT 4.0 DDK/SDK headers and libraries: build-time dependency the developer supplies; never committed.
- Windows NT source code leaks: not consulted; everything so far comes from the shipped binaries' own symbol tables and observed behaviour. Each new source file states this in its header.
- maciNTosh's NT binaries (`halgoss.dll`, `usbadb.sys`, `atapimio.sys`, `offrmbuf.sys`, from the
  parent repo's release `drivers.img`): **run and read locally, never redistributed.** They are
  GPL-2.0 with no published source, so we could not accompany a copy with the corresponding
  source or a written offer (GPL-2.0 §3). Reading them to learn an interface is what the rows
  above record; shipping one, or a patched one, is not something this project may do.
- Apple, Symbios/LSI and Motorola documents: facts and citations only.
