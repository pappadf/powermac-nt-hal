# Getting the emulator this project is developed against

> ## ⚠ Read this first
>
> The emulator support this HAL needs is **not in Granny Smith's mainline**. It is
> **[pull request #135](https://github.com/pappadf/granny-smith/pull/135)**, branch
> **`ppc-le-mode-and-bandit-lane-reversal`** — open, not merged, as of 2026-09-07.
>
> If you build Granny Smith's `main` you get an emulated Power Macintosh that **cannot run any
> of this**, and it will not tell you why: the firmware's little-endian reboot is what fails, so
> you never reach a point where anything mentions NT. Use the branch.
>
> When #135 merges this page's step 2 becomes `git clone` and nothing else. Until then it is a
> required step, not a footnote.

---

## 1. What the branch adds, and what breaks without it

Four commits, none of which exist on `main`:

| commit | what it adds | without it |
|---|---|---|
| `96a7336` | **PowerPC little-endian mode on the 604** (MSR `ILE`/`LE`, the fetch and data address munge, `MSR[LE]` copied from `ILE` on exception) and **Bandit byte-lane reversal** (the endian bit in the bridge's mode-select register, which reverses PCI byte lanes for a little-endian CPU) | Open Firmware's `little-endian? true` reboot produces a CPU that fetches garbage. Nothing after step 1 of the boot works. This is the load-bearing commit |
| `39fa333` | **The Bandit config ports reverse with the lanes** — a bridge whose data lanes are reversed reverses its own configuration ports too | PCI configuration space reads back byte-swapped, so the HAL's `HalGetBusData` sees nonsense and no device is identified |
| `8ebed27` | Documentation of the NT-on-ANS boot | nothing functional |
| `b4dbbe9` | **The four Cirrus 54M30 registers a driver identifies the part by** (`SR06` unlock, `CR27` device id, `SR15` memory size, the `$3CC` misc-output read-back) | `cirrus.sys` cannot identify the chip; Setup stops with *"fatal error while initializing your computer's video (0, 0xc0000034)"* |

This HAL was developed against `b4dbbe9`, the branch tip.

## 2. Get the emulator source

You need `git`, `gcc` (C11) and `python3`. The headless build needs nothing else — no SDL, no
Emscripten.

**Either** clone the branch directly:

```bash
git clone --branch ppc-le-mode-and-bandit-lane-reversal \
          https://github.com/pappadf/granny-smith.git
cd granny-smith
```

**or**, if you already have a clone:

```bash
cd granny-smith
git fetch origin ppc-le-mode-and-bandit-lane-reversal
git checkout ppc-le-mode-and-bandit-lane-reversal
```

**or**, with the [GitHub CLI](https://cli.github.com/), check the pull request out by number —
useful if the branch is later renamed or force-pushed:

```bash
cd granny-smith
gh pr checkout 135
```

## 3. Confirm you have the right tree

Do this before building. Each check is one line and fails loudly:

```bash
# the little-endian core
grep -q le_xor src/core/cpu/ppc/ppc_internal.h            && echo "OK  little-endian mode"
# the Bandit lane reversal
grep -q bandit_lanes_reversed src/machines/tnt/bandit.c    && echo "OK  Bandit lane reversal"
# the Cirrus identification registers
grep -q C54M30_CR27_ID src/core/peripherals/pci/cards/cirrus54m30.c \
                                                            && echo "OK  Cirrus 54M30 id registers"
```

Three `OK` lines means you are on the branch. If any is silent you are on `main` (or a stale
branch) — go back to step 2.

## 4. Build the headless emulator

```bash
make headless          # ~1 minute; output: build/headless/gs-headless
```

`make headless` builds a native binary with a TCP shell, breakpoints, device logpoints and
checkpoints. That shell is what this project's tools drive; the WebAssembly build (`make`) is
not needed and will not help.

## 5. Check it works before involving NT

Run the emulator's own integration test for the machine family this HAL targets. It exercises
exactly the Bandit behaviour the branch changed, so a pass here means the emulator half is
sound:

```bash
make integration-test-tnt-pci-slots > /tmp/tnt-pci-slots.log 2>&1; tail -5 /tmp/tnt-pci-slots.log
```

## 6. Start the emulator's daemon

The tools in [`../tools/`](../tools/) talk to a long-running emulator over TCP, so that restoring
a checkpoint and trying a new `hal.dll` takes seconds instead of a ten-minute cold boot. From
the Granny Smith checkout:

```bash
ROM=path/to/ans-2.26NT.rom            # see section 7
nohup ./build/headless/gs-headless --daemon --kill --port=6820 --speed=turbo \
      --no-prompt -q --checkpoint-dir=tmp/ckpt-daemon rom=$ROM --var ROM=$ROM \
      > tmp/daemon.log 2>&1 &
```

The tools default to port 6820 (`GS_PORT` overrides it). A daemon holds one machine; restoring a
consolidated checkpoint materialises a fresh copy-on-write delta under `--checkpoint-dir` each
time, so **watch that directory's size** — each restore of the NT checkpoint costs about 540 MB,
and a dozen iterations will fill a small disk.

## 7. What you must supply yourself

None of this can ship with either project.

| | |
|---|---|
| **Open Firmware 2.26NT for the ANS** | a ROM dump from the [TinkerDifferent thread](https://tinkerdifferent.com/threads/apple-network-server-macos-based-roms-found.4756/). The one used throughout has MD5 `ad405e01c663340c479668c70f741f1b` |
| **Windows NT 4.0 Workstation for PowerPC** | the OEM 000-48303 (Oct 1996) CD image, MD5 `ab37556d72818ed082c1d01c2d7f1898`, archive.org item `windowsnt40workstationoem_00048303_alt`. Work on a **copy** — the tools patch it |
| **A staging disk** | a raw disk image with `VENEER.EXE` (from the CD's `\PPC`) written at block `0x800`, where the ROM's `pe-loader` is told to read it from |
| **A disk for NT** | any raw image of 512 MB or more; [`../tools/mkarcdisk.py`](../tools/mkarcdisk.py) gives it the FAT16 system partition an ARC machine's `ARCINST.EXE` would create |
| **`clang-18`, `lld-18`** | to build the HAL itself. Separate from the emulator's `gcc`; no Microsoft toolchain and no DDK |

## 8. Then build the HAL and run it

From *this* repository:

```bash
make                                   # -> build/hal.dll
```

Getting from a cold ROM to Setup's computer-type menu is described step by step in
[`CHARTER.md` §3.1](CHARTER.md#31-replicating-it) — the little-endian reboot, loading the veneer,
and the four veneer patches this project applies. Take one checkpoint at that menu, then iterate
against it with [`../tools/run-hal.py`](../tools/run-hal.py); [`../tools/README.md`](../tools/README.md)
documents every option.

## 9. When #135 merges

Then: delete step 2's branch dance and step 3 entirely, clone `main`, and update the three
places that name the branch — this page, [`../README.md`](../README.md)'s *Testing it* section,
and [`CHARTER.md` §3.1](CHARTER.md#31-replicating-it). Until that happens, anyone following
mainline instructions fails at the first reboot with no clue why, so the warning at the top of
this page stays.
