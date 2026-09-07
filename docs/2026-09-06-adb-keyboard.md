# An ADB keyboard for NT, in three HAL exports

*2026-09-06, third session. `halshinr` build 28 + `source/cuda.c`.*

Setup's next screen after the video came up was *"Setup did not find a keyboard connected to
your computer"*, and it was right: the Network Server's keyboard is ADB behind Cuda, NT 4.0
ships no ADB driver, and `i8042prt` probes a port `0x60` that does not exist here.

It turns out the driver already exists, and the contract it needs from a HAL is three functions.

---

## 1. What the adjacent projects actually have

Cloned and read on 2026-09-06 (`/tmp/ntppc/{entii,macintosh,bandit}`):

| | ADB? | source? |
|---|---|---|
| Wack0/maciNTosh | yes — `usbadb.sys`, *"PowerMac General HID & Storage"* | **no**: "NT HAL and drivers have no source present for now"; the binaries are release assets (`nt_arcfw_grackle_0.08.zip` → `drivers.img`), not in the repo |
| MCJack123/maciNTosh-bandit | yes, for **our** chipset — but in the *ARC firmware* | **yes**: `arcbandit/source/{pxi.c,adb_bus.c,adb_kbd.c}`, GPL-2.0, `main.c:741` doing `PxiInit(GrandCentralStart + 0x16000, MRP_VIA_IS_CUDA, …)` |
| Wack0/entii-for-workcubes | no ADB (GameCube/Wii input) | **yes**, and `fpsidrv/source/` is a complete NT 4.0 PowerPC keyboard+mouse port driver — the template if we ever write our own |

Correcting our own notes while we were there: `boot_files/boot.img` in the Bandit fork is **not**
the NT HAL and drivers. It is a 256 KB HFS volume named "Windows NT" holding `stage1.elf`,
`stage2.elf` and `System/BootX` — the ARC loader, which *is* built from source in the repo.
That fork publishes no releases at all.

## 2. Three functions, and what they mean

`usbadb.sys` is a PowerPC LE PE (machine `0x1F0`) that imports **six** names from `HAL.dll`:

```
KeRaiseIrql   KeLowerIrql   KeStallExecutionProcessor      <- already exported
HalPxiCommandAdb   HalPxiAdbSetCallback   HalPxiAdbAutopoll
```

It connects no interrupt and imports nothing chipset-specific: all the Cuda knowledge belongs to
the HAL, which makes the driver portable and the contract three functions wide. The three are
declared in `maciNTosh-bandit/inc/halpxi.h`; the *semantics* came out of maciNTosh's own
`halgoss.dll`, which is far more precise than the header:

- `HalPxiAdbSetCallback` is `*g_callback = arg` and nothing else (`halgoss` image VA `0x18730`).
- `HalPxiCommandAdb(Command, Data, Length, Poll)` (`0x1845c`) refuses `Length > 15`, builds
  `[Command][Data…]`, sends it as a Cuda packet of type 0 at raised IRQL, reads up to 16 bytes
  back, and — if `reply[0] == 0 && reply[2] == Command` — hands the reply to the callback.
- The delivery routine (`0x17bf4`) is four lines and settles the callback's arguments exactly:

  ```c
  if (len <= 1) return;
  if (!(cb = *g_callback)) return;
  cb(/*Status*/ p[0], /*Command*/ p[1], /*Data*/ p + 2, /*Length*/ len - 2);
  ```

  with `p` = the Cuda reply from its *flags* byte on. So `Status` is Cuda's flag byte (bit 1 =
  the addressed device did not answer, bit 6 = the packet came from an auto-poll), `Command` is
  the ADB command byte echoed back, `Data`/`Length` are what the device returned.

`usbadb.sys`'s own callback (image VA `0x102a0`) confirms it from the other side: it reads the
ADB address out of `Command >> 4`, the command kind out of `(Command >> 2) & 3` and the register
out of `Command & 3`, and special-cases a Talk-register-3 reply to record each device's handler.

## 3. The HAL side

`source/cuda.c`, ~230 lines. The VIA's sixteen registers on 0x200 centres at Grand Central
`+0x16000`; TIP (PB5), ByteAck (PB4) ours, TREQ (PB3) Cuda's, all active low; the shift register
clocked by Cuda, so "a byte is ready" is "IFR bit 2 is set" and "this is the last byte" is "TREQ
went high". The handshake sequence is adapted from `arcbandit/source/pxi.c` — same silicon,
same protocol — with every wait bounded by the timebase, because a HAL may not hang the machine
on a microcontroller that stops answering.

Three things the ARC firmware does not have to solve:

- **The attention byte.** Cuda clocks a leading `0x00` that carries nothing. `HalpCudaReadFirst`
  takes it, asserts TIP and returns the byte after it, so a reply reads `[type][flags][cmd][data…]`
  — which is exactly the layout `halgoss` checks.
- **Unsolicited packets.** Auto-polled key data arrives with no request outstanding, so the VIA
  needs an owner. `HalPxiAdbAutopoll` enables the VIA's shift-register interrupt and adds Grand
  Central bit 18 to the HAL's own registered set; `HalpExternalInterrupt` services bit 18 inside
  the HAL and never dispatches it to a driver, because Cuda is not NT's to have.
- **The IRQL the callback runs at**, which the first working build got wrong and NT said so
  precisely: `STOP 0x00000009 IRQL_NOT_GREATER_OR_EQUAL (0x2, 0x15, 0, 0x8072CAC0)` — asked to
  raise to DISPATCH_LEVEL while at IRQL 0x15 = 21, from inside `usbadb.sys`. 21 is the VIA
  line's device IRQL, and the driver's callback takes a spin lock, which *raises* to
  DISPATCH_LEVEL. So the interrupt may not call the callback at all. It parks the packet in an
  eight-deep ring and queues a DPC; the DPC delivers at DISPATCH_LEVEL. maciNTosh's HAL says the
  same thing in its own way — `HalPxiCommandAdb` does `KeLowerIrql(2)` immediately before
  invoking the callback (`halgoss` image VA `0x1852c`).

  One more trap behind that one: a `KDPC` declared as a byte array gets byte alignment, and the
  kernel's DPC list operations fault on it — `STOP 0x1E` with `STATUS_DATATYPE_MISALIGNMENT` and
  a data address ending in `1`. It is declared as `ULONG[16]` here.

Cuda takes no device mask (it polls whatever answered its own bus scan), so a nonzero `Mask`
means "on".

## 4. Getting the driver onto the media

SETUPLDR does **not** consult `[files.i8042]` for the keyboard: it loads the files named
`i8042prt.sys` and `kbdclass.sys` outright (`SETUPLDR` image VA `0x601344`/`0x6013b8`). So the
delivery is to write `usbadb.sys` over `\PPC\I8042PRT.SYS` — and to put back the
`[files.i8042]` entry this project had renamed to keep the real `i8042prt` out of the load list.

One trap: SETUPLDR verifies the PE checksum over **the length the ISO directory record gives**,
not the length in the image. Dropping a 21,648-byte driver into a 38,928-byte slot and
zero-padding it gets *"The file i8042prt.sys is corrupted"*. Recomputing the checksum over the
padded image fixes it (`0xc866` → `0x10be6`). The shipped HAL slot never showed this because
`elf2pe.py` writes the checksum for whatever length it emits and the HAL is loaded by a path
that does not check.

## 5. What it does

```
HAL: adb status 00 cmd 00 len 0
HAL: adb status 02 cmd 1f len 0            <- address 1: nothing there
HAL: adb status 00 cmd 2f len 2 [02 01]    <- address 2, Talk r3: the KEYBOARD
HAL: adb status 00 cmd 3f len 2 [03 01]    <- address 3, Talk r3: the mouse
HAL: adb status 02 cmd 4f len 0            <- 4..12: nothing
…
HAL: adb status 00 cmd 2f len 2 [02 03]    <- keyboard moved to handler 3
HAL: ADB autopoll on
HAL: adb status 40 cmd 2c len 2 [00 ff]    <- AUTO-POLLED: 'a' down
```

`status 0x40` is Cuda's auto-poll flag, `cmd 0x2C` is address 2 / Talk / register 0, and `00 ff`
is ADB's "key `0x00` pressed, second slot empty". With the DPC in place the whole stream flows,
down and up, for as long as keys are typed:

```
HAL: adb status 40 cmd 2c len 2 [00 ff]    <- 'a' down
HAL: adb status 40 cmd 2c len 2 [80 ff]    <- 'a' up
HAL: adb status 40 cmd 2c len 2 [00 ff]
HAL: adb status 40 cmd 2c len 2 [80 ff]
```

Those came from `machine.adb.keyboard.type("a")` and travelled Cuda → VIA → Grand Central
interrupt → the HAL's ring → a DPC → the driver's callback, with the driver stable underneath.
**The keyboard works.**

And Setup agrees: the *"Setup did not find a keyboard"* screen is gone. It gets far enough to
launch `usetup.exe` — the CD reads show `\PPC\SYSTEM32\ntdll.dll` and `\PPC\SYSTEM32\smss.exe`
(which is `USETUP.EXE`; the ISO shares the extents) being mapped.

## 6. Where it stopped, and why (solved)

> **Setup could not load the keyboard layout file KBDUS.DLL.**

That message is `usetup.exe`'s, and `usetup` imports `LdrLoadDll` — so it loads the layout DLL
itself. Facts established:

- `\PPC\KBDUS.DLL` exists on the CD (9,488 bytes), is a valid PowerPC PE (`0x1F0`, native
  subsystem) and its stored checksum is correct.
- `LoaderBlock->NtBootPathName` is `\PPC\`, so `\SystemRoot` resolves, and `\SystemRoot\System32`
  demonstrably works — that is where `ntdll.dll` and `smss.exe` were loaded from.
- SETUPLDR never preloads a keyboard layout on this architecture: its whole vocabulary of
  TXTSETUP.SIF sections is `Scsi`, `DiskDrivers`, `ScsiClass`, `CdRomDrivers`, `Extenders`,
  plus files named outright. There is no `KeyboardLayout` string in it.
- **The CD is never read for it.** With the SCSI log on, no access to KBDUS.DLL's extent
  (100407) and none to the `\PPC` directory (196-249) after the kernel starts. Adding
  `KBDUS.DLL` to `\PPC\SYSTEM32`'s directory (the image directory, and therefore first on the
  loader's search path) changed nothing — same screen, byte-identical.

So the load failed before it reached the file system. The path `usetup` hands `LdrLoadDll` is a
**rooted** DOS path — a leading backslash and no drive — and `LdrLoadDll(NULL, ...)` resolves
one of those against the process's current drive. There was no current drive, and no drives at
all: **`IoAssignDriveLetters` is a HAL export, and ours was a stub**. Nothing had created
`\DosDevices\A:`, `\DosDevices\C:` or `\DosDevices\D:`, and nothing had rewritten
`NtSystemPathString` from an NT device path into a DOS one. The failure is inside `ntdll`,
which is why no medium is ever touched.

Assigning drive letters is policy rather than hardware; it sits in the HAL on NT 4.0 only
because the *order* is architecture-specific (an x86 machine numbers disks the way the BIOS did,
an ARC machine the way the firmware did). A stub is therefore completely silent until the first
component that assumes a DOS namespace goes looking for one.

`source/misc.c` now implements it: `A:`/`B:` for floppies, `C:` upward for
`\Device\Harddisk%d\Partition1`, then `\Device\CdRom%d`, each an `IoCreateSymbolicLink` under
`\DosDevices\`; the boot device is found by matching the `NtDeviceName` the kernel passes in,
and `NtSystemPathString` is rewritten in place with that drive's letter.

```
HAL: IoAssignDriveLetters: boot device '\Device\CdRom0' -> D:, 0 floppy 1 disk 1 cdrom
HAL: system path -> 'D:\PPC\'
```

Setup then comes up with **"Welcome to Setup"** and walks, one ADB keystroke at a time, through
the mass-storage list, six pages of licence agreement and the hardware-confirmation screen.
Screenshots: [`../traces/`](../traces/), `2026-09-06-setup-01-welcome.png` onward. The next wall
is the partition table — see [STORY.md](../STORY.md) wall 32.
