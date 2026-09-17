# The BitBLT engine, and what it took to see it draw

*Written on 17 September 2026, the day the engine specified in
[`2026-09-16-handover-the-bitblt-engine.md`](2026-09-16-handover-the-bitblt-engine.md) was built
and run. That handover is the specification; this is the outcome, the four places the
specification differed from the manual, and the one question it left open — which turned out to
have a different answer than either candidate it offered.*

**Status.** The engine is in `granny-smith` at `f2fd1ec`, this HAL at `47f3274`. All four of the
handover's acceptance criteria are met. GUI-mode Setup draws. A complete text-mode installation
has been built from scratch on a fresh disk and the installed disk boots. The GUI wizard stops
at its Registration page for want of a Product ID, which is not a technical wall.

---

## 1. What was built

Two things, in one file, `src/core/peripherals/pci/cards/cirrus54m30.c`:

**The legacy-window mirror.** The chip decodes the 128 KB VGA window at `0xA0000` and
memory-maps the BitBLT registers inside it at `0xB8000`. This board cannot use that address —
Bandit does not forward CPU accesses to PCI addresses below `0x80000000` — so the model exposes
the same 128 KB at the top of BAR0's 16 MB aperture, offset `0xFE0000`. **That placement is a
convention of this model, not a property of the part**, and the source says so where it is
defined. Inside it: banked display memory through `GR9`/`GRA`/`GRB` with `GR6[3:2]` selecting the
map, and the 256-byte register block of Appendix B20 at `0xB8000`, gated by `SR17[2]` and aliased
at every 256-byte boundary to `0xBFF00`, or moved by `SR17[6]` to the last 256 bytes of the
linear address space.

**The engine.** Synchronous: the blit is finished by the time the write that set the start bit
returns, so a driver polling the status bit sees a completed engine on its first read. It covers
screen-to-screen with the decrementing direction bit, 8×8 pattern copy with the vertical preset,
colour expansion at 8 and 16 bpp with and without transparency, system-to-screen fed by CPU
writes after the start bit, `GR2F`'s left-edge clip, and all sixteen raster operations.

The HAL side is one line: `HalTranslateBusAddress` answers the display driver's moved access
range with the mirror instead of raw VRAM. The `0xA0000` branch below it is unchanged — still
refused, for the reasons its comment gives.

---

## 2. Four places the handover differed from the manual

The handover said to re-check every transcribed register against the *Alpine VGA Family
CL-GD543X/4X Technical Reference Manual*, 4th ed. (Feb 1995), "because a transcription is not a
citation". That was worth doing four times.

| | The handover said | The manual says | Why it matters |
|---|---|---|---|
| **`GR31` bit 1** | clear bit 0 when the blit completes | "This bit will be cleared to '0' when the BLT is completed" is said of the **START** bit (§9.40) as plainly as of the status bit | A driver polling bit 1 would have spun exactly as before. This one could have cost the run |
| **`GR32` ROPs** | `AD` = `~S \| D`, `D6` = `~S \| ~D`, `DA` = `~(S & D)` — with `D6` and `DA` the same operation | Table D8-3: `AD` = `S \| ~D`, `D6` = `~S \| D`, `DA` = `~S \| ~D` | Two of the sixteen were swapped and one duplicated |
| **`GR2F`** | "destination write mask" | §7: "the first n **pixels** of each scan line of the destination will not be written" — a left-edge clip | Named "Write Mask" in the register list, defined as clipping in the text |
| **`GR2A`** | open question 3: five bits or six? | §2: "Each start address is a 21-bit value for the CL-GD5430/'40" — **five** | Answered, and the question is closed |

Table D8-3 also turned out to be a gift. It gives each ROP as a truth table over (source,
destination), so returning it as four bits indexed by `(S << 1) | D` reduces all sixteen
operations to three lines of bitwise algebra rather than a switch — and the same table serves
pattern-op-destination, because "the value actually programmed into GR32 is independent of
whether a source or pattern is used".

One inconsistency is worth recording because it is in the handover twice, in two directions: it
describes `0x18100`–`0x1FFFF` as banked VRAM in one bullet and the register block as "aliased
every 256 bytes from `0xB8000` to `0xBFF00`" in the next. Appendix B20 settles it — "Address bits
14:8 are 'don't care', so the block is actually aliased at every 256 boundary" — and the model
aliases it.

---

## 3. The byte-lane question, and why both candidate answers were wrong

The handover's section 4 is careful and well-argued, and its conclusion — *write the decode
against the documented offsets and let the first run tell you* — was exactly right. The two
candidates it offered were that `cirrus.dll` pre-XORs its register addresses, or that it uses an
access size whose byte lanes fall differently. **It was neither.**

The decode was written against the documented offsets. On the first run the driver's registers
arrived at those offsets, decoded to coherent values, and drew a correct GUI. A driver that
pre-XORed would have produced nonsense. So the driver writes where the manual says.

The `XOR 7` in the captured block was an artifact of the instrument. `tmp/diskboot.gs`'s
VRAMDUMP loop calls `machine.memory.dump`, which reads bytes with `memory_debug_read_uint8` at
ascending addresses and applies **no little-endian address munge** — so the byte it prints at
position `A` is the device byte at `A ^ 7`. The same script's own `peekw` helper compensates by
hand, with an explicit `^ 4` on every 32-bit peek; the dump loop has no such compensation.

This is STORY lesson 10 — "its memory peek does not apply the little-endian address munge; both
have produced confident wrong conclusions before" — collected in the handover's own list of
things that would bite, and it bit anyway. **A measurement taken through an uncalibrated
instrument is a measurement of the instrument.**

---

## 4. The evidence the engine is right

### 4.1 Pixel-identical to a CPU-drawn reference

The strongest result, and it costs nothing to check again. On 14 September the same
`cirrus.dll` drew GUI Setup's wizard **entirely on the CPU**, with no engine present, and that
screen is in the tree as
[`../traces/2026-09-14-gui-02-setup-wizard.png`](../traces/2026-09-14-gui-02-setup-wizard.png).
The engine-drawn screen of 16 September differs from it in **0 of 307,200 pixels**.

The same driver, through two completely different code paths, produces the identical
framebuffer. Nothing else available here comes close to that as a correctness argument.

It also disposes of a defect that was reported and was not one: the speckled blue behind the
dialog is 75% black and 23% navy in isolated single pixels, which looks wrong and is not a
dither — and is exactly what the CPU-drawn reference has too. It is what NT Setup renders on
this machine.

### 4.2 What a real driver actually exercises

5,402 blits across two GUI-Setup runs, with **no** unknown ROP, no unsupported expansion width
and no write to a reserved offset:

| Mode | ROP | Count |
|---|---|---|
| colour-expand 8×8 pattern | `$0D` SRCCOPY | 3,375 |
| colour-expand transparent, system source | `$0D` | 780 |
| colour-expand 8×8 pattern | `$59` PATINVERT | 752 |
| screen to screen | `$0D` | 214 |
| system source | `$0D` | 93 |
| 8×8 pattern | `$59` | 88 |
| colour-expand 8×8 pattern | `$00` BLACKNESS | 76 |
| screen to screen | `$59`, `$05`, `$0E` | 20 |

Every mode in the specification is exercised by the driver except the decrementing direction
bit, which only the unit suite covers. **The window's banking arithmetic is likewise
modelled-but-untested** — nothing on this machine drives the window through a bank register, and
the source says so where it is defined. Do not let a run that never exercised it be cited as
verifying it.

### 4.3 The unit suite

`tests/unit/suites/cirrus54m30` drives the card through a real `pci_bus_t` window with lane
reversal on — the Bandit path, not a direct poke at the model — in thirteen rows. Its
centrepiece replays the register block the NT driver left in display memory on 16 September and
asserts the one-byte-wide, thirteen-line XOR strip lands where the arithmetic says. The first
row pins the premise everything rests on: a plain byte store through a lane-reversed window
lands at the offset it was aimed at, which this HAL's own console has depended on all along.

---

## 5. The installation

Built from scratch on 17 September, to see the engine work on something other than the disk it
was developed against.

1. **A fresh disk.** `mkarcdisk.py --part 4096:65536 --part 69632:0` on an empty 512 MB file:
   the 32 MB ARC system partition at LBA 4096 and the 478 MB target at 69632. Setup's partition
   list confirmed it empty — `C: FAT 32 MB (31 MB free)`, `D: FAT 478 MB (477 MB free)`.
2. **Text-mode Setup ran the whole way through**, from the boot floppy against the stock CD,
   thirty keystrokes on the ADB keyboard, *"Leave the current file system intact"* at the file
   system page so FAT survives for the ARC path, and *"This portion of Setup has completed
   successfully."* at the end
   ([`2026-09-17-text-mode-complete-on-a-fresh-disk.png`](../traces/2026-09-17-text-mode-complete-on-a-fresh-disk.png)).
   The resulting disk carries `\OS\WINNT40\HAL.DLL`, `OSLOADER.EXE`, `VENEER.EXE` and a full
   `\WINNT\SYSTEM32`.
3. **The installed disk boots.** Cold boot, two lines at the firmware prompt, and NT loads from
   it: kernel, this HAL, `adbport.sys` from the registry, `win32k`, `cirrus.dll`.
   `IoAssignDriveLetters` puts the boot device at `D:` and the system path at `D:\WINNT`, and
   GUI Setup's wizard comes up — drawn by the engine
   ([`2026-09-17-gui-wizard-drawn-by-the-blt-engine.png`](../traces/2026-09-17-gui-wizard-drawn-by-the-blt-engine.png)).
4. **GUI Setup ran the whole way through**, once the Product ID from the machine's Certificate
   of Authenticity was to hand: *"Windows NT 4.00 has been installed successfully"*
   ([`2026-09-17-gui-setup-installed-successfully.png`](../traces/2026-09-17-gui-setup-installed-successfully.png)).
   The wizard's page order, measured, is Welcome → Setup Options → Name and Organization →
   Registration → Computer Name → Administrator Account → Emergency Repair Disk → Select
   Components → Installing Windows NT Networking → how to participate on a network → [Network
   Adapter search]. Along the way NT's Display Properties reported *"The system found the
   following video adapter in your machine: cirrus compatible display adapter"* at 256 colours.
5. **The finished installation boots.** A cold boot of the resulting disk, nothing typed beyond
   the two firmware lines, reaches *Microsoft Windows NT Workstation 4.0 with Microsoft Internet
   Explorer* and its "Press Ctrl + Alt + Delete to log on" dialog
   ([`2026-09-17-installed-nt-boots-to-logon.png`](../traces/2026-09-17-installed-nt-boots-to-logon.png)),
   with 2,124 blits and no bug check. Two screenshots six seconds of emulated time apart are
   byte-identical, so it is settled and waiting rather than still drawing.

### 5.1 Three pages that Return alone cannot take

Worth writing down, because each one costs a run to rediscover and two of them present as
something they are not.

* **The Product ID's three boxes do NOT auto-advance.** Typing all seventeen digits straight
  through leaves five in the first box and the other two empty, because each box stops at its
  own maximum length and the rest of the string goes nowhere. Setup answers that with *"The
  Product Id you entered is not valid"*, which reads exactly like a rejected key and is not one.
  Tab between the boxes.
* **The Emergency Repair Disk page defaults to "Yes, create"** and there is no floppy NT can
  write here -- the drive it sees is the RAM disk `adbport.sys` serves. Down picks "No".
* **The networking page defaults to "This computer will participate on a network"**, which leads
  to an adapter search this machine has nothing to answer. The search finds nothing, `Next` stays
  disabled, and Return lands on `Back` instead, so a Return-only sequence walks between those two
  pages for ever. Up picks "Do not connect this computer to a network at this time", which skips
  the branch entirely.

---

## 6. Things that will bite, collected fresh

The handover's own list is still accurate. These are the ones this work added, all of them paid
for with a run.

* **The emulator does not exit when its script ends.** It goes idle and sits there. A run that
  looks hung after the last line of the script has simply finished, and the guest's writes are
  still only in the emulator's private copy of the image.
* **Save the disk from inside the run.** `machine.scsi2.device[0].image.export("out.img")`
  flattens base + delta to a new file on demand, and refuses to overwrite. Relying on process
  exit instead cost a whole text-mode installation: the run was killed at what looked like a
  hang, and the copy in the checkpoint directory never reached the host.
* **The writable copy lives in `--checkpoint-dir`, not `/opfs/images`.** An hour was spent
  hunting for a delta in the wrong directory.
* **Never kill by a pattern that matches your own waiter.** `pkill -f "script=tmp/full-text.gs"`
  matches every shell polling for that script as well as the emulator. This is in the handover's
  list already; it has now burned a third run. Kill by PID.
* **`settle` never converges on a page with a blinking caret**, so it burns its whole budget.
  With generous budgets the thirty-odd pages of the GUI wizard came to something over twenty
  hours at the ~1.25e9 instructions a minute this machine manages. Cap every wait; a wizard page
  draws in well under 1e9 cycles.
* **A plain letter is an accelerator unless an edit control has focus.** Typing `"ANS"` on the
  Setup Options page sent `A` nowhere and `N` straight to **Next**, turning the page mid-string.
  Type only where a text field is known to have focus, and prefer strings with no `N`, `B` or
  `F` in them.

---

## 7. What is left

1. **Finish GUI Setup.** It needs the Product ID; everything before and after it is driven.
2. **The window's banking arithmetic is unexercised.** If anything is ever made to drive the
   window through `GR9`/`GRA`/`GRB`, that would turn modelled-but-untested into verified.
3. **Route A is not cancelled.** A linear-framebuffer miniport paired with `framebuf.dll` still
   retires ledger row 6 and the `cirrus.sys` patch, and this work does not change that. It is
   now the smaller of the two jobs rather than the alternative to this one.
4. **`GR33`, autostart and pause are '36-only** and are decoded but not implemented. No driver
   on this machine has been seen to touch them.
