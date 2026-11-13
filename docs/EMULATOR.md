# Getting the emulator this project is developed against

Development is against the [Granny Smith](https://github.com/pappadf/granny-smith) emulator,
whose breakpoints, device logpoints and checkpoints are what made the HAL tractable. Everything
this HAL needs from it — PowerPC little-endian mode on the 604, Bandit byte-lane reversal, the
Cirrus 54M30's identification registers and its BitBLT engine — is in Granny Smith's `main`
since September 2026. There is no branch to find and nothing to patch: clone it and build.

Granny Smith also carries the end-to-end proof. Its integration tests `ans-nt-install` and
`ans-nt-installed-boot` install Windows NT 4.0 from this project's floppy on an emulated Network
Server 500 and cold-boot the result to the logon screen, every one of the Open Firmware lines
and Setup screens below typed by the test.

## 1. Get and build the emulator

You need `git`, `gcc` or `clang` (C11), `make` and `python3`. The headless build needs nothing
else — no SDL, no Emscripten.

```bash
git clone https://github.com/pappadf/granny-smith.git
cd granny-smith
make headless          # ~1 minute; output: build/headless/gs-headless
```

`make headless` builds a native binary with a TCP shell, breakpoints, device logpoints and
checkpoints. That shell is what this project's tools drive; the WebAssembly build (`make`) is
the browser frontend and is not needed for any of this.

## 2. Check it before involving NT

Each line is one check and fails loudly if the tree is older than this project needs:

```bash
grep -q le_xor src/core/cpu/ppc/ppc_internal.h              && echo "OK  little-endian mode"
grep -q bandit_lanes_reversed src/machines/tnt/bandit.c      && echo "OK  Bandit lane reversal"
grep -q C54M30_CR27_ID src/core/peripherals/pci/cards/cirrus54m30.c \
                                                              && echo "OK  Cirrus 54M30 id registers"
```

Then run the emulator's own test for the machine family. It exercises exactly the Bandit
behaviour this HAL depends on, so a pass means the emulator half is sound:

```bash
make integration-test-ans-pci-slots > /tmp/ans-pci-slots.log 2>&1; tail -5 /tmp/ans-pci-slots.log
```

(Granny Smith's integration tests need its private test-data checkout for the ROM; see its
`docs/guide/TEST_DATA.md`. With your own ROM you can skip this and go straight to section 4.)

## 3. What you must supply yourself

None of this can ship with either project.

| | |
|---|---|
| **Open Firmware 2.26NT for the ANS** | a ROM dump from the [TinkerDifferent thread](https://tinkerdifferent.com/threads/apple-network-server-macos-based-roms-found.4756/). The one used throughout has MD5 `ad405e01c663340c479668c70f741f1b`. It must be the **NT** ROM: the production 1.1.22 ROM cannot run NT at all |
| **Windows NT 4.0 Workstation for PowerPC** | the OEM 000-48303 (Oct 1996) CD image, MD5 `ab37556d72818ed082c1d01c2d7f1898`, archive.org item `windowsnt40workstationoem_00048303_alt`. It is **never written**: the floppy build reads four files off it, and the install boots it as pressed |
| **A disk for NT** | `make disk` in this repository writes a 512 MB image with the ARC system partition Setup requires; see the README |
| **`clang-18`, `lld-18`, `python3`** | to build the HAL and the ADB driver. Separate from the emulator's C compiler; no Microsoft toolchain and no DDK |

## 4. Build the HAL, the driver and the floppy

From *this* repository:

```bash
make                                   # -> build/hal.dll, build/adbport.sys
make floppy ISO=/path/to/nt4-ppc.iso   # -> build/boot-floppy.img
make disk                              # -> build/nt-disk.img
```

## 5. Run it

The machine is a Network Server 500 with 64 MB, the CD at SCSI id 0 on the first fast/wide
controller, the disk at id 0 on the second, and the floppy in the internal drive. Typing at the
firmware prompt is easiest in the browser frontend (`make run` in Granny Smith, then pick the
2.26NT ROM and attach the same media), where the 54M30 screen and the ADB keyboard are live. The
headless binary takes the same machine on its command line:

```bash
ROM=path/to/ans-2.26NT.rom
./build/headless/gs-headless --speed=turbo rom=$ROM model=ans500 ram=65536 \
      drive=scsi:0:cd:/path/to/nt4-ppc.iso drive=scsi2:0:hd:/path/to/nt-disk.img \
      fd0=/path/to/boot-floppy.img
```

and its shell can type into the serial console and read it back; Granny Smith's integration
test `tests/integration/ans-nt-install/test.script` is the runnable, authoritative form of
everything from here on, every Open Firmware line and Setup key included.

At the `0 >` prompt:

```forth
load fd:,\boot.of          \ to install from the CD
load-base loadsize eval
```

and `\bootdisk.of` in place of `\boot.of` to start what you installed. Nothing is configured
first: the firmware stays in its factory big-endian mode and nothing is stored in NVRAM. The README and the
[wiki](https://github.com/pappadf/powermac-nt-hal/wiki) have the screen-by-screen walk through
Setup.

## 6. Iterating on the HAL

The tools in [`../tools/`](../tools/) talk to a long-running emulator over TCP, so that
restoring a checkpoint and trying a new `hal.dll` takes seconds instead of a ten-minute cold
boot:

```bash
nohup ./build/headless/gs-headless --daemon --kill --port=6820 --speed=turbo \
      --no-prompt -q --checkpoint-dir=tmp/ckpt-daemon rom=$ROM --var ROM=$ROM \
      > tmp/daemon.log 2>&1 &
```

The tools default to port 6820 (`GS_PORT` overrides it). A daemon holds one machine; restoring a
consolidated checkpoint materialises a fresh copy-on-write delta under `--checkpoint-dir` each
time, so **watch that directory's size** — each restore of the NT checkpoint costs about 540 MB,
and a dozen iterations will fill a small disk. [`../tools/README.md`](../tools/README.md)
documents every option of every tool, and [`CHARTER.md` §3.1](CHARTER.md#31-replicating-it) has
the firmware sequence and the gotchas.
