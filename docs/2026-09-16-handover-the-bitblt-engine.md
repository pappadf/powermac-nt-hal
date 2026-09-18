# Handover — the BitBLT engine and the legacy-window mirror

*Written on 16 September 2026, at the point where the work was decided but not yet written. The
session that did the research ended on a server error before a line of the implementation
existed; this document is what that session knew, so the next one does not have to rediscover
it. Everything below is either a measurement, a citation, or a decision — where it is a guess it
says so.*

**Status when this was written.** `granny-smith` at `7ed8023`, `powermac-nt-hal` at `718a400`,
both trees clean, no emulator process running, nothing half-applied. The research is complete
and no implementation has started. Section 5 is the specification to write; section 6 says where
it goes; section 7 is the command sequence that proves it.

---

## 1. Why this exists

GUI-mode Setup now starts from the installed disk and never draws. The wall is
[`2026-09-15-the-boot-floppy.md`](2026-09-15-the-boot-floppy.md) **E28**, and in one sentence:
`cirrus.dll` draws its fills through the GD5430's BitBLT engine, reaching it through
memory-mapped registers in the legacy VGA window at `0xB8000`, and the emulated 54M30 has
neither the engine nor the window. The driver writes a blit, sets the start bit, polls the
status bit, and the status bit never clears, so it spins.

The boot itself is healthy. Kernel, this HAL, `adbport.sys` from the registry, `autochk`,
`win32k` and `cirrus.dll` all load; the palette loads correctly; one grey **< Back** button is
drawn in the right place. Everything after the first blit is the engine that is not there.

Two routes were on the table on 16 September and the second was chosen:

| | Route | Cost | What it buys |
|---|---|---|---|
| A | Our own linear-framebuffer miniport (`drivers/ansfb`) paired with Microsoft's `framebuf.dll` | one more text-mode Setup run, plus the GUI | retires ledger row 6 and the `cirrus.sys` patch; no dependence on emulator fidelity |
| B | **Emulate the BitBLT engine and a legacy-window mirror** | several hundred lines in the emulator | the model becomes truthful to the part; the Microsoft driver pair runs as shipped |

Route B is the one being built, and the reason is worth recording because it is not the
narrowest path to a booting desktop: the emulator's 54M30 should model the chip the Network
Server actually has, and that is worth something on its own. Route A remains the right answer
for ledger row 6 and is not cancelled by this work.

---

## 2. What is already verified

Do not re-establish these. Each was paid for with a cold boot of roughly twenty minutes.

- **Text-mode Setup completes** from the boot floppy against a stock CD, and the installed disk
  boots from the floppy into GUI Setup (`STOP 0x35` fixed by `adbport`'s `DEVICEMAP`
  registration — E27).
- **The palette is correct.** The driver uses the 6-bit DAC and writes `$00` to the hidden DAC
  register. Red-on-black console text is the *correct* rendering of the HAL's colour indices
  under Windows' palette, not a fault. An earlier "values above `$3F` imply 8-bit" heuristic was
  wrong and has been withdrawn.
- **No BLT is ever started through the I/O ports.** `GR31` bit 1 was logged at `$3CF` across
  several runs and never fired. The engine is reached by MMIO only.
- **The graphics register file really was too small.** The model had 32 graphics registers, so
  the driver's `GR20`–`GR35` writes aliased onto the bank-select and write-mode registers. Fixed
  to `0x40` in `7ed8023`. It did not change the picture, but it was a genuine defect.
- **Three answers to the legacy `0xA0000` window have been tried and all three fail**: aliased
  to VRAM, backed by private RAM, and refused outright. The refused variant is what the working
  14 September desktop runs used, and it changes nothing here.
- **Granting the resource claim while refusing the later mapping does not work.** The miniport
  needs a *working* window mapping to initialise, not merely an accepted claim; refusing the
  mapping still ends in `STOP c0000143`. This was a deliberate ten-line experiment on
  16 September at 20:11 and it failed. It has been reverted. Do not retry it.

---

## 3. The evidence: a real blit, captured

At the end of a disk-boot run the rig dumps all 1 MB of VRAM over the bus. About 2 KB of it is
non-zero: the HAL console's text, the button, and — at VRAM `0x18000`, which is the legacy
window's `0xB8000` — the driver's register block, written as pixels because nothing decoded it
as registers.

```
VRAM 0x18000:
  +00: 00 00 00 00 ff ff ff ff 00 10 02 80 00 0c 00 00
  +10: 00 04 b0 40 00 03 c9 4a 00 00 00 00 00 59 00 40
  +40: 00 00 00 00 00 00 00 02
```

Every non-zero byte in that block decodes to a meaningful field, and none decodes to nonsense,
**provided each byte is read at its memory-mapped offset XOR 7**:

| Byte seen at | Value | Reaches | Register | Meaning |
|---|---|---|---|---|
| `+07` `+06` | `ff` `ff` | `00` `01` | `GR0` / `GR10` | background colour, white |
| `+05` `+04` | `ff` `ff` | `02` `03` | (colour bytes 2–3) | not used at 8bpp |
| `+0E` `+0F` | `00` `00` | `09` `08` | `GR21` / `GR20` | BLT width − 1 = 0, so **1 byte wide** |
| `+0D` `+0C` | `0c` `00` | `0A` `0B` | `GR22` / `GR23` | BLT height − 1 = 12, so **13 lines** |
| `+0B` `+0A` | `80` `02` | `0C` `0D` | `GR24` / `GR25` | destination pitch **640** |
| `+09` `+08` | `10` `00` | `0E` `0F` | `GR26` / `GR27` | source pitch **16** |
| `+17` `+16` `+15` | `4a` `c9` `03` | `10` `11` `12` | `GR28` / `GR29` / `GR2A` | destination start **`0x03C94A`** |
| `+13` `+12` `+11` | `40` `b0` `04` | `14` `15` `16` | `GR2C` / `GR2D` / `GR2E` | source start **`0x04B040`** |
| `+10` | `00` | `17` | `GR2F` | destination write mask, none |
| `+1F` | `40` | `18` | `GR30` | mode: bit 6, **8×8 pattern copy** |
| `+1D` | `59` | `1A` | `GR32` | ROP **`0x59`**, `S ^ D` — `SRCINVERT` / `PATINVERT` |
| `+47` | `02` | `40` | `GR31` | **start bit set, and still set at the end of the run** |

The blit reads as a focus rectangle or caret: a one-byte-wide, thirteen-line vertical strip,
XOR-drawn from an 8×8 pattern. Two numbers confirm the reading independently. The destination
`0x03C94A` at pitch 640 is pixel (458, 387), which is in the button's neighbourhood at the
bottom of a 640×480 screen. The source `0x04B040` is 64 bytes past `0x4B000`, the exact end of
the visible frame buffer — off-screen memory, which is where a display driver caches its 8×8
patterns.

**This is the single most useful artifact in the handover.** It is a known-good input: an
implementation that executes this blit correctly, with these register values, is doing the right
thing. Keep it as the first unit test.

---

## 4. The byte-lane question — sharper than it was left

The session recorded this as "the driver's MMIO bytes arrive at the documented offset XOR 7,
calibrate it by logging". That is still the plan, but the field has been narrowed since, and the
narrowing matters because it decides whether the emulator compensates or not. **Getting this
backwards produces a model that works only for this one driver, which would be worse than not
modelling it at all.**

The first reading was that the XOR is the little-endian 604's address munge leaking through, and
that the window decode therefore has to undo it. That reading is wrong, and here is why.

- The emulator already cancels the munge, once, in the right place. `pci.c`'s `lane_offset()`
  turns an N-byte access at window offset `o` into the access at `o ^ (8 − N)` with its bytes
  reversed, exactly so that no device model ever sees anything but PCI byte addresses. A
  little-endian CPU byte store munges the effective address by 7 on the way out; the window
  XORs 7 again; the two cancel.
- That cancellation is not theoretical here — **this HAL's own console depends on it.**
  [`src/vga.c`](../src/vga.c) writes every pixel as a single byte store through the same BAR
  (`HalpFb[i] = BG` in `HalpFbClear`, `row[x] = …` in `HalpFbGlyph`), with no compensation
  anywhere in the file, and the console renders correctly on screen in every trace we have.

So plain byte stores to this aperture land where they are aimed, and the XOR 7 in the register
block is **not** coming from the bus. It is coming from the driver — either because `cirrus.dll`
computes its register addresses pre-XORed (a compensation for byte-swapping host bridges, which
a PowerPC NT display driver has every reason to carry), or because it reaches those registers
with an access size other than a byte, so a different lane rule applies.

Those two are distinguishable in one run, and the experiment is already in the plan: **log every
access to the new MMIO block as `offset, size, value` before interpreting any of it.** Then

- if the log shows byte-sized accesses arriving at `TRM offset ^ 7`, the driver pre-XORs, and the
  model must decode the register block at its documented offsets and let the driver's XOR be
  visible in the log — *the emulator must not compensate*, because a correctly-written driver
  would then be broken by it; or
- if the log shows word-sized accesses, work the lane rule through from the size and fix the
  decode accordingly, and the XOR 7 was an artifact of how the bytes fell out of 32-bit writes.

Until that log exists, treat the XOR 7 as an observation about one driver, not a property of the
chip. Write the register decode against the documented offsets and let the first run tell you
what arrives.

---

## 5. What to build

Source: *Alpine VGA Family CL-GD543X/4X Technical Reference Manual*, 4th edition, February 1995 —
the BitBLT chapter, and Appendix B20 for the memory-mapped register block. The register values
below were transcribed from it during the research session; **re-check each one against the
manual as you implement it**, because a transcription is not a citation.

### 5.1 The legacy-window mirror

The chip decodes the 128 KB legacy VGA window at `0xA0000`, and the BitBLT registers are
memory-mapped inside it at `0xB8000`. This board cannot use that: Bandit does not forward CPU
accesses to PCI addresses below `0x80000000`, which is the whole reason ledger row 6 exists and
the miniport's access range was moved to `0x90000000`.

So the model exposes the same 128 KB at an address the CPU *can* reach: **the top 128 KB of
BAR0's 16 MB aperture, at offset `0xFE0000`**. This is a documented convention of this model,
not a property of the real part, and it must be commented as such in the source. It sits above
the 1 MB of fitted DRAM, in aperture space the chip currently drives nothing into, so it costs
nothing real.

Within that 128 KB:

- **`0x00000`–`0x17FFF` and `0x18100`–`0x1FFFF`** — banked VRAM. The bank comes from `GR9`
  (offset in 4 KB units, or 16 KB units when `GRB` bit 5 is set), with `GRA` supplying the second
  bank for `SA15 = 1` when `GRB` bit 0 enables dual banking, and `GR6` bits 3:2 selecting the
  window's map.
- **`0x18000`–`0x180FF`** — the memory-mapped register block, *when `SR17` bit 2 enables it*, and
  aliased every 256 bytes from `0xB8000` to `0xBFF00`. When `SR17` bit 2 is clear this range is
  banked VRAM like the rest.
- `SR17` bit 6 (on the 5430/5436/5440) additionally places the register block at the highest 256
  bytes of the linear aperture. Model it; it is three lines and a driver may use it.

### 5.2 The memory-mapped register map (Appendix B, Table B20-1)

| Offset | Register | Field |
|---|---|---|
| `00` / `01` | `GR0` / `GR10` | background colour, bytes 0 and 1 |
| `04` / `05` | `GR1` / `GR11` | foreground colour, bytes 0 and 1 |
| `08` / `09` | `GR20` / `GR21` | BLT width − 1, 13 bits, **in bytes** |
| `0A` / `0B` | `GR22` / `GR23` | BLT height − 1 |
| `0C` / `0D` | `GR24` / `GR25` | destination pitch |
| `0E` / `0F` | `GR26` / `GR27` | source pitch |
| `10` / `11` / `12` | `GR28` / `GR29` / `GR2A` | destination start, 22 bits (the 5430 has 5 bits in `GR2A`) |
| `14` / `15` / `16` | `GR2C` / `GR2D` / `GR2E` | source start |
| `17` | `GR2F` | destination write mask, bits 2:0 |
| `18` | `GR30` | BLT mode |
| `1A` | `GR32` | ROP |
| `1B` | `GR33` | 5436-only extensions |
| `40` | `GR31` | start and status |

`GR30`, the mode byte: bit 7 colour expand, bit 6 8×8 pattern copy, bits 5:4 expand width
(`00` = 8bpp), bit 3 transparency, bit 2 source is system memory, bit 0 direction is decrementing.

`GR31`, start and status: bit 7 autostart (5436), bit 5 pause (5436), bit 4 buffered status
(5436), bit 3 progress status (read-only), bit 2 reset, bit 1 **start**, bit 0 **status**
(read-only). Bit 0 is the one the driver polls and the one that must be cleared when the blit
completes.

`GR32`, the sixteen ROP codes:

| Code | Operation | | Code | Operation |
|---|---|---|---|---|
| `00` | `BLACKNESS` | | `50` | `~S & D` |
| `05` | `S & D` | | `59` | `S ^ D` (`SRCINVERT`, `PATINVERT`) |
| `06` | `D` | | `6D` | `S \| D` |
| `09` | `S & ~D` | | `90` | `~(S \| D)` |
| `0B` | `~D` | | `95` | `~(S ^ D)` |
| `0D` | `S` / `P` (`SRCCOPY`, `PATCOPY`) | | `AD` | `~S \| D` |
| `0E` | `WHITENESS` | | `D0` | `~S` |
| | | | `D6` | `~S \| ~D` |
| | | | `DA` | `~(S & D)` |

### 5.3 The engine

Execute a blit **synchronously**, the moment `GR31` bit 1 is written, then clear the status bit
before the write returns. Nothing in this emulator needs a blit to take time, and a driver that
polls will see completion on its first read. The modes to cover:

- **Screen to screen**, honouring the decrementing direction bit.
- **8×8 pattern copy** (`GR30` bit 6). Patterns are 64 bytes at 8bpp, or 8 bytes of mono when
  colour expansion is on. This is the mode the captured blit uses, so it is the one to get right
  first.
- **Colour expansion** (`GR30` bit 7): foreground from `GR1`/`GR11`, background from `GR0`/`GR10`,
  and background left untouched when the transparency bit is set.
- **System to screen** (`GR30` bit 2): the source is fed by CPU writes into the window while the
  blit is pending, rather than read from VRAM.

Throughout: width and height are *minus one* in the registers, widths are in bytes, addresses are
22-bit and must be masked into the 1 MB of fitted DRAM before any access, and `GR2F`'s write mask
gates the destination bytes. A blit that would run outside VRAM must be clamped, not allowed to
scribble — the aperture comment in the model already promises that writes above the fitted DRAM
vanish, and the engine must keep that promise too.

---

## 6. Where it goes

Everything is in one file: `src/core/peripherals/pci/cards/cirrus54m30.c` in the emulator tree
(759 lines at `7ed8023`).

- **State** lives in `typedef struct c54m30` at line 134. It already holds `vram`, the register
  shadows (`seq`, `crtc`, `gr`, `attr`), the DAC and hidden-DAC state, `display`, `clut`, and
  three `memory_interface_t` members: `fb_if`, `io_if`, `vga_if`.
- **Add a fourth interface**, say `win_if`, for the mirror. The interface is six function
  pointers (`src/core/memory/memory.h`):

  ```c
  uint8_t  (*read_uint8)(void *device, uint32_t addr);
  uint16_t (*read_uint16)(void *device, uint32_t addr);
  uint32_t (*read_uint32)(void *device, uint32_t addr);
  void     (*write_uint8)(void *device, uint32_t addr, uint8_t data);
  void     (*write_uint16)(void *device, uint32_t addr, uint16_t data);
  void     (*write_uint32)(void *device, uint32_t addr, uint32_t data);
  ```

- **Decoding it inside BAR0** is the simplest route, because BAR0 is already backed by
  `fb_if` through `pci_bar_backing_iface(dev, C54M30_BAR_FB, &c->fb_if, c)` near line 739. Send
  offsets at or above `0xFE0000` down the mirror path from inside `fb_read8` / `fb_write8`
  (lines 172–199), which the 16- and 32-bit accessors already decompose into. That keeps one BAR
  and one decode.
- **Follow the existing conventions**: `LOG(level, …)` under `LOG_USE_CATEGORY_NAME("54m30")` at
  line 60, and the register-count constants near line 108. `C54M30_VRAM` is `0x00100000`,
  `C54M30_FB_SPAN` is `0x01000000`.
- `c54m30_update()` (line 502) derives the 640×480×8 mode and sets `display.bits = c->vram +
  start`. Any blit must set `display.fb_dirty = true`, the way `fb_write8` does.
- There is a `_Static_assert` on the fitted DRAM at line 132 and a bounds check at 532; add the
  equivalent guard for the mirror's placement so that `0xFE0000 + 0x20000` can never exceed
  `C54M30_FB_SPAN`.
- The checkpoint writer at line 663 serialises card state. New engine registers that matter
  across a save must go in it — although note that these runs never use checkpoints (section 8).

On the HAL side, one line. [`src/pci.c`](../src/pci.c)'s `HalTranslateBusAddress` currently
aliases the moved window onto raw VRAM:

```c
*TranslatedAddress = vram + (bus_lo & 0x1FFFFu);
```

It becomes the mirror:

```c
*TranslatedAddress = vram + 0xFE0000u + (bus_lo & 0x1FFFFu);
```

The `0xA0000` branch below it stays exactly as it is — refused, with its comment intact. That
refusal is correct and is not part of this work.

---

## 7. The command sequence

Build the emulator, put the rebuilt HAL on the installed disk, rebuild the floppy, cold-boot:

```sh
cd /workspaces/granny-smith
make -f Makefile.headless -j2                      # → build/headless/gs-headless

cd local/powermac-nt-hal
make                                               # → build/hal.dll
python3 tools/fatput.py ../../tmp/nt-installed.img 1 '\OS\WINNT40\HAL.DLL' build/hal.dll
python3 tools/mkbootfloppy.py --out ../../tmp/boot-floppy.img \
    --veneer ../../tmp/veneer-fd.exe --disk-veneer ../../tmp/veneer-disk.exe \
    --setupldr ../../tmp/hal/cd/SETUPLDR --hal build/hal.dll \
    --display-driver ../../tmp/oemsrc/cirrus.sys --display-dll ../../tmp/oemsrc/cirrus.dll \
    --vga-aperture 0x90000000
```

Then run `tmp/diskboot.gs`, which cold-boots with cleared NVRAM, types `SETUP.OF` and
`BOOTDISK.OF` at the firmware prompt, turns on `debug.log "54m30" 1`, captures 24 seconds of
emulated time with a screenshot every sixth step into `tmp/hal/dN.png`, walks the IRP and the
stack on a `STOP`, and finally dumps all 1 MB of VRAM between `=== VRAMDUMP begin ===` and
`=== VRAMDUMP end ===`. Render that dump offline with `tmp/vramrender.py <log> <out.png> [width]`.

Roughly twelve minutes of emulation pass before the GUI phase begins. Budget twenty minutes a
run and do not start a second one alongside it (section 8).

**Read the run in this order:** the `54m30` MMIO log lines first — they answer section 4 and
nothing else is interpretable until they do — then the screenshots, then the VRAM dump.

### Acceptance

1. The MMIO log shows the driver's register writes arriving, and the decode has been calibrated
   against them.
2. `GR31`'s status bit clears, and the driver stops spinning.
3. The captured blit from section 3 executes with the right geometry.
4. The wizard's fills appear on screen.

Anything short of 4 is still progress worth committing, as long as the commit message says which
of the four it reached.

---

## 8. Things that will bite

Collected from the sessions that hit them, because every one of these cost a run.

- **Two CPU cores.** Run one emulator at a time. Kill it by PID —
  `ps -eo pid,comm | grep gs-headless` — and never poll with a pattern that matches the polling
  script's own command line, which has burned two runs.
- **No checkpoints, ever, in these runs.** This is a standing instruction: every run cold-boots.
  Checkpoints are flaky, and they would freeze the very things each experiment changes — the
  floppy, the veneer, the HAL on the disk image, and sometimes the emulator binary itself.
- **Every attached disk image adds a 512 MB copy to `/opfs/images`.** Prune after each series.
  There is presently 11 GB free and a run needs room.
- **Verify you are running what you just built** (`find build/hal.dll -newer src/...`). Launching
  a run with a stale binary has happened more than once and looks exactly like a failed fix.
- **The emulator's `clang-format` pre-commit hook** may reformat and abort the first commit
  attempt. Re-add and commit again.
- **Commit trailers differ between the two repositories.** `granny-smith` commits carry the
  `Co-Authored-By: Claude` trailer; `powermac-nt-hal` commits must **not** — its AI policy lives
  in the README instead.
- **Nothing published from this repository may reference the private document tree.** Cite the
  Cirrus manual by title, edition and section, as this document does. No Microsoft binaries,
  Apple ROMs or vendor PDFs are ever committed.
- **The emulator's breakpoints report after the instruction executes**, and its memory peek does
  not apply the little-endian address munge. Both have produced confident wrong conclusions
  before (STORY, lessons 6 and 10). Calibrate against a value you already know.
- Script-language traps are collected in
  [`2026-09-15-the-boot-floppy.md`](2026-09-15-the-boot-floppy.md) Appendix B. The expensive ones:
  no backslashes in `echo` tags, single quotes in tags, and `${…}` as a bare argument is
  unproven — bind it with `let x = try(call(...))` first.

---

## 9. Open questions

1. **Does `cirrus.dll` pre-XOR its register offsets, or use a non-byte access size?** Section 4.
   One run with the MMIO log answers it. Everything else in the decode depends on the answer.
2. **What does the miniport's MMIO probe actually test?** On 14 September the window was
   unmapped, the miniport reported no MMIO, and the DLL drew on the CPU — which worked. Today the
   same refusal kills the DLL. The difference was never explained, and it was deliberately not
   chased further: it means reading a Microsoft binary at the cost of twenty-minute runs. If the
   engine works, the question stops mattering.
3. **Is `GR2A` five bits or six on this part?** The manual gives the 5430 five. It only matters
   for a destination above 1 MB, which this card cannot have, so it is a correctness-of-model
   question rather than a blocker.
4. **Does anything else use the legacy window?** The HAL console does not; it writes linearly
   through BAR0. If nothing else does, the mirror's banking logic is unexercised code, and it
   should be marked as modelled-but-untested rather than claimed as verified.

---

## 10. If this route stalls

Route A from section 1 is unaffected by anything here: a small NT video miniport reporting the
54M30's BAR0 as a 640×480×8 linear frame buffer, paired with Microsoft's `framebuf.dll`. No BLT,
no legacy window, no aperture claim, and ledger row 6 retires with it. It needs one more
text-mode Setup run for the new OEM `[Display]` entry, and it uses the same toolchain as
`adbport`. It was the recommendation on 16 September and it remains the fallback.
