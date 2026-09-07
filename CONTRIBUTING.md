# Contributing

Patches are welcome. Two rules are absolute, because the project cannot be published without
them; everything else is ordinary taste.

## 1. Provenance: every borrowed line is logged, at the time it is borrowed

[`PROVENANCE.md`](PROVENANCE.md) has one row per file or function in this repository that is not
original work, recording where it came from, under what license, and how much was taken
(verbatim / adapted / rewritten from reading). The row is added **in the same commit** as the
code, never reconstructed afterwards.

**A pull request that adds code without a provenance row is not merged.** This is not
bureaucracy: the project's license is *derived* from that table (see the bottom of
`PROVENANCE.md`), so a missing row makes the license of the whole work unknowable.

Rows are append-only. Removing borrowed code adds a new row saying so; it does not delete the
old one.

## 2. Clean room: no leaked Microsoft source, ever

Everything here was derived from:

- the **DDK-documented** `Hal*` contract,
- the **export tables and symbol tables of shipped binaries** (`HALEAGLE.DLL`, `SETUPLDR`,
  `VENEER.EXE`, `NTKRNLMP.EXE` — all of which ship with their own COFF symbols),
- **observed behaviour** under a debugger,
- and **hardware documentation** (Apple, Motorola, Symbios, Zilog, Cirrus).

The leaked Windows NT source trees are **not consulted and must not be**. If you have seen
them, you cannot contribute to the files they would inform. Each source file's header states
this; keep it accurate.

## What must never be committed

| | why |
|---|---|
| Microsoft binaries — `VENEER.EXE`, `SETUPLDR`, `HAL*.DLL`, `NTOSKRNL.EXE`, CD images or any file off the NT CD | proprietary; cite by name and origin instead |
| Microsoft DDK/SDK headers or the PowerPC import libraries | the DDK EULA permits building with them, not redistributing them. The build here does not need them |
| Apple ROMs, Apple/Symbios/Motorola/Cirrus PDFs or their extracted text | proprietary; cite chapter and section |
| maciNTosh's NT binaries (`halgoss.dll`, `usbadb.sys`, `atapimio.sys`, `offrmbuf.sys`) | GPL-2.0 with no published source, so GPL-2.0 §3 cannot be satisfied. Reading one locally to learn an interface is fine and is what the provenance rows record; **redistributing one is not** |
| Disk images, ROM images, checkpoints | see `.gitignore`; they are inputs you supply, not project content |

## Style

- Every source file carries `SPDX-License-Identifier: GPL-2.0-only` (alone on its line, so
  licence scanners parse it) and `Copyright (C) 2026 powermac-nt-hal contributors`. Add yourself to
  [`AUTHORS`](AUTHORS) in your first commit.
- A file with a provenance row also carries an `UPSTREAM:` header block naming the project, the
  commit read, its licence and what came from it — see `src/irql.c` or `src/cuda.c` for the
  shape, and [`NOTICE`](NOTICE) for the collected version. The upstreams keep their notices in a
  repository-level `COPYING` rather than per file, so naming the project *is* the attribution;
  do not invent a copyright line for them.
- C is C99, freestanding, built with `clang` and `lld` — no Microsoft toolchain. Keep it that
  way; `make` must work on a stock Linux box with `clang-18` and `lld-18`.
- Structure layouts that the kernel and the HAL must agree on are pinned with
  `_Static_assert` on `sizeof`. If you add one, pin it.
- Two target-specific hazards, both of which have bitten this code and are documented in
  [`STORY.md`](STORY.md) (walls 33 and 34): the 604 runs **little-endian** here, where a
  misaligned access traps instead of being fixed up in hardware; and LLVM will happily
  re-create a misaligned access out of careful byte-at-a-time code, folding byte loads into
  `lwz` and byte stores into `stw`. The `Makefile` passes
  `-mllvm -combiner-store-merging=false` for the store side; the load side is blocked with
  `volatile` at the point of access. Do not remove either without reading those two walls.
- An eight-byte return value (`LARGE_INTEGER`, `PHYSICAL_ADDRESS`) comes back through a hidden
  pointer in `r3` on this ABI, so arguments start at `r4`. Such routines are declared here as
  `VOID` with an explicit out-pointer. Wall 20 in `STORY.md` is what happens when they are not.

## Testing a change

There is no unit-test suite; the test is that NT boots further than it did before. The loop is
in [`tools/README.md`](tools/README.md): build, write `hal.dll` over `HALEAGLE.DLL` on a copy of
the CD, restore a checkpoint at Setup's computer-type menu, and drive it with
[`tools/run-hal.py`](tools/run-hal.py). Console output, screenshots and the HAL's own
`HalpPrint` trace are the evidence. When a change fixes a wall, add its trace to
[`traces/`](traces/) and its story to `STORY.md` — that file is the project's real
documentation.
