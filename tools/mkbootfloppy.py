#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
"""mkbootfloppy.py — build the 1.44 MB boot floppy that carries everything this project adds,
so the user's Windows NT CD can be used exactly as it was pressed.

`docs/2026-09-15-the-boot-floppy.md` is the plan; this is component C1.  The image holds:

  * `\\PPC\\VENEER.EXE`   Microsoft's ARC shim with ledger rows 1-5 baked in (see mkveneer.py).
                          Deliberately allocated **first**, so it starts at cluster 2 and is
                          contiguous: Open Firmware reads it with `read-blocks` off the raw
                          device long before anything can parse a filesystem, and a contiguous
                          extent at a known block is the only thing that makes that possible.
  * `\\PPC\\SETUPLDR`     NT's text-mode Setup loader, copied from the user's own CD.
  * `\\HALSHINR.DLL`     this project's HAL, in the **root**, because that is where a
                          `[Disks]` line's directory field points Setup.
  * `\\I8042PRT.SYS`     the ADB keyboard driver that stands in for the one NT expects.
  * `\\TXTSETUP.OEM`     the OEM description that offers the HAL as a computer type (C2), and
                          -- with `--display-driver` -- a display type as well, which is how
                          ledger row 6 reaches a boot that applies no pokes.

Nothing from Microsoft is stored in this repository: every file above is named on the command
line and comes from the user's own media.  The tool writes an image; it ships no content.

    mkbootfloppy.py --out tmp/boot.img \\
        --veneer <PPC/VENEER.EXE> --setupldr <PPC/SETUPLDR> \\
        --hal build/hal.dll --kbd <i8042prt replacement>

It prints the veneer's start block and length in blocks, which is what
`mkcoldboot.py --veneer-dev /bandit/gc/swim3 --veneer-block N --veneer-blocks M` needs.
"""
import argparse, os, struct, sys

SECTOR = 512
GEOM = dict(bps=512, spc=1, reserved=1, nfats=2, root_entries=224, total=2880,
            media=0xF0, spf=9, spt=18, heads=2)

FREE, EOC, BAD = 0x000, 0xFFF, 0xFF7


def fat12_get(fat, n):
    i = n * 3 // 2
    v = fat[i] | (fat[i + 1] << 8)
    return (v >> 4) if (n & 1) else (v & 0x0FFF)


def fat12_set(fat, n, val):
    i = n * 3 // 2
    v = fat[i] | (fat[i + 1] << 8)
    v = ((val << 4) | (v & 0x000F)) if (n & 1) else ((v & 0xF000) | (val & 0x0FFF))
    fat[i], fat[i + 1] = v & 0xFF, (v >> 8) & 0xFF


def shortname(name):
    """'HALSHINR.DLL' -> the 11-byte 8.3 field.  Refuses anything that will not fit, rather
    than silently truncating a name the loader then cannot find.

    '.' and '..' are not 8.3 names at all -- they are two literal directory entries whose name
    field is a dot or two dots padded with spaces, and splitting them on '.' yields an empty
    name that no FAT reader will follow."""
    if name in ('.', '..'):
        return name.ljust(11).encode('ascii')
    stem, _, ext = name.upper().partition('.')
    if len(stem) > 8 or len(ext) > 3:
        sys.exit(f'{name}: not an 8.3 name, and this builder writes no long-name entries')
    return (stem.ljust(8) + ext.ljust(3)).encode('ascii')


class Fat12:
    def __init__(self, **geom):
        self.__dict__.update(geom)
        self.fat_start = self.reserved
        self.root_start = self.fat_start + self.nfats * self.spf
        self.root_sectors = (self.root_entries * 32 + self.bps - 1) // self.bps
        self.data_start = self.root_start + self.root_sectors
        self.clusters = (self.total - self.data_start) // self.spc
        self.img = bytearray(self.total * self.bps)
        self.fat = bytearray(self.spf * self.bps)
        fat12_set(self.fat, 0, 0xF00 | self.media)
        fat12_set(self.fat, 1, 0xFFF)
        self.next_free = 2

    def alloc(self, data):
        """Write `data` into fresh clusters and return (first cluster, first LBA)."""
        n = max(1, (len(data) + self.bps * self.spc - 1) // (self.bps * self.spc))
        if self.next_free + n > self.clusters + 2:
            sys.exit(f'out of space: {n} more clusters needed, {self.clusters + 2 - self.next_free} left')
        first = self.next_free
        for k in range(n):
            c = first + k
            fat12_set(self.fat, c, EOC if k == n - 1 else c + 1)
        lba = self.data_start + (first - 2) * self.spc
        self.img[lba * self.bps: lba * self.bps + len(data)] = data
        self.next_free = first + n
        return first, lba

    def dirent(self, name, cluster, size, attr=0x20):
        e = bytearray(32)
        e[0:11] = shortname(name)
        e[11] = attr
        # A fixed stamp, so the same inputs always produce the same image: 1 Oct 1996, the
        # month this CD was pressed.  FAT dates count years from 1980.
        struct.pack_into('<HH', e, 22, 0, ((1996 - 1980) << 9) | (10 << 5) | 1)
        struct.pack_into('<H', e, 26, cluster)
        struct.pack_into('<I', e, 28, size)
        return bytes(e)

    def subdir(self, name, entries):
        """A one-cluster subdirectory holding `entries` plus '.' and '..'."""
        first, lba = self.alloc(b'\0' * (self.bps * self.spc))
        blob = self.dirent('.', first, 0, 0x10) + self.dirent('..', 0, 0, 0x10)
        for e in entries:
            blob += e
        if len(blob) > self.bps * self.spc:
            sys.exit(f'{name}: {len(entries)} entries do not fit in one cluster')
        self.img[lba * self.bps: lba * self.bps + len(blob)] = blob
        return self.dirent(name, first, 0, 0x10)

    def finish(self, root_entries, label):
        bs = bytearray(self.bps)
        bs[0:3] = b'\xeb\x3c\x90'
        bs[3:11] = b'MSDOS5.0'
        struct.pack_into('<HBHBHHBHHHII', bs, 11, self.bps, self.spc, self.reserved, self.nfats,
                         self.root_entries, self.total, self.media, self.spf, self.spt,
                         self.heads, 0, 0)
        bs[36], bs[38] = 0x00, 0x29
        struct.pack_into('<I', bs, 39, 0x4E544F45)
        bs[43:54] = label.upper().ljust(11)[:11].encode('ascii')
        bs[54:62] = b'FAT12   '
        bs[510:512] = b'\x55\xaa'
        self.img[0:self.bps] = bs
        for i in range(self.nfats):
            o = (self.fat_start + i * self.spf) * self.bps
            self.img[o:o + len(self.fat)] = self.fat
        root = b''.join(root_entries)
        cap = self.root_sectors * self.bps
        if len(root) > cap:
            sys.exit('root directory full')
        self.img[self.root_start * self.bps: self.root_start * self.bps + len(root)] = root
        return bytes(self.img)


OEM_HEADER = """\
; SPDX-License-Identifier: GPL-2.0-only
; The OEM description text-mode Setup reads off this floppy.
;
; Every section and key name here is SETUPLDR's own.  `Disks`, `Defaults`, `Computer`, `Files.`,
; `Config.` and `Strings` sit together in its string table beside the source path
; `D:\\nt\\private\\ntos\\boot\\setup\\oemdisk.c`, and `hal`, `driver`, `inf`, `dll`, `class`, `port`
; and `detect` -- the keys inside a `[Files.<class>.<id>]` section -- sit together a little
; before it.  The shapes of the entries follow the CD's own TXTSETUP.SIF: a `[Computer]` or
; `[Display]` line is `id = "description", files-section`, and a `[Keyboard]` line adds the
; driver's registry key as a third field, exactly as
; `STANDARD = "XT, AT, or Enhanced Keyboard (83-104 keys)",files.i8042,i8042prt` does there.
;
; The `[Computer]` class is the one that is exercised end to end: Setup offers it, loads
; \\HALSHINR.DLL off this disk, and carries on.  The others are written to the same shapes and
; are not yet confirmed -- see docs/2026-09-15-the-boot-floppy.md, risk R4.

[Disks]
d1 = "Apple Network Server 500/700 support disk", \\txtsetup.oem, \\
"""

OEM_COMPUTER = """
[Computer]
shiner_up = "Apple Network Server 500/700", files.shiner_up

[Files.Computer.shiner_up]
hal = d1, halshinr.dll

[Config.shiner_up]
"""

OEM_KEYBOARD = """
[Keyboard]
adb_kbd = "Apple Desktop Bus keyboard", files.adb_kbd, i8042prt

[Files.Keyboard.adb_kbd]
driver = d1, i8042prt.sys, i8042prt

[Config.i8042prt]
"""

# The display class exists for one reason: ledger row 6.  cirrus.sys claims the legacy VGA
# aperture, which is RAM on this board, VideoPortVerifyAccessRanges reports the conflict, and
# Setup dies initialising video (wall 25).
#
# There is exactly one `driver =` line because SETUPLDR loads exactly one image per OEM class --
# SlInit calls SlLoadOemDriver once after SlPromptOemVideo -- and it takes the *first* file key
# in the section.  Measured both ways: with `port` first, videoprt.sys came off this disk and no
# miniport was loaded at all; with `driver` first, cirrus.sys comes off this disk and its import
# of VIDEOPRT.SYS is resolved from the CD.  So whatever clears wall 25 has to be in the
# miniport -- and moving the miniport's own legacy-VGA access range out of RAM does not clear
# it, which was measured too.  The claim comes from somewhere else, and until that is found this
# class buys nothing: the options exist because they are the delivery channel the fix will need.
OEM_DISPLAY = """
[Display]
ans_cirrus = "Cirrus Logic 54M30 (Apple Network Server 500/700)", files.ans_cirrus

[Files.Display.ans_cirrus]
driver = d1, cirrus.sys, cirrus
dll = d1, cirrus.dll

[Config.ans_cirrus]
"""



def pe_checksum(data):
    """The PE image checksum NT verifies before it will load a driver: a 16-bit ones-complement
    sum of the whole file with the checksum field itself zeroed, plus the file length.  A driver
    whose checksum does not match is refused with STATUS_IMAGE_CHECKSUM_MISMATCH."""
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        sys.exit('not a PE image')
    d = bytearray(data)
    struct.pack_into('<I', d, pe + 24 + 64, 0)
    if len(d) & 1:
        d += b'\0'
    total = 0
    for i in range(0, len(d), 2):
        total += struct.unpack_from('<H', d, i)[0]
        total = (total & 0xFFFF) + (total >> 16)
    total = (total & 0xFFFF) + (total >> 16)
    return (total + len(data)) & 0xFFFFFFFF


VGA_APERTURE, VGA_APERTURE_LEN = 0x000A0000, 0x00020000


def move_vga_aperture(driver, to):
    """Ledger row 6, in the one file an OEM disk can deliver -- and now for the right reason.

    A display miniport hands `videoprt` an array of VIDEO_ACCESS_RANGE (two halves of a
    LARGE_INTEGER start, a length, then four flag bytes), and this one claims the legacy VGA
    aperture: 0xA0000 for 128 KB.  `IoReportResourceUsage` translates every reported range
    through the HAL, and ours refuses PCI memory below 0x80000000 -- deliberately, because on
    this board that address is ordinary RAM and handing it back would let the driver write over
    the kernel.  The kernel takes the refusal as STATUS_INVALID_PARAMETER and fails the whole
    call, which is wall 25.  It was never a resource conflict.

    So the claim has to name an address the machine can actually translate.  `to` must be at or
    above 0x80000000 for that reason; below it the HAL refuses exactly as before and nothing
    changes -- which is what made an earlier attempt at 0x70000000 look like a dead end.

    Still a workaround, and still ledger row 6: it edits a Microsoft driver's data.  What it
    buys is that the edit is in the miniport, which `txtsetup.oem` can carry, rather than in
    `videoprt.sys`, which it cannot."""
    if to < 0x80000000:
        sys.exit(f'{to:#x} is below 0x80000000, which the HAL refuses to translate — '
                 'the claim would fail exactly as it does unpatched')
    d = bytearray(driver)
    hits = [o for o in range(0, len(d) - 16, 4)
            if struct.unpack_from('<III', d, o) == (VGA_APERTURE, 0, VGA_APERTURE_LEN)]
    if len(hits) != 1:
        sys.exit(f'expected exactly one VGA-aperture access range in the display driver, '
                 f'found {len(hits)} — refusing to patch')
    struct.pack_into('<I', d, hits[0], to)
    struct.pack_into('<I', d, struct.unpack_from('<I', d, 0x3c)[0] + 24 + 64, pe_checksum(bytes(d)))
    print(f'  ledger row 6  file {hits[0]:#07x}  VGA access range {VGA_APERTURE:#x} -> {to:#x}, '
          f'PE checksum recomputed')
    return bytes(d)


def txtsetup_oem(display):
    defaults = ['\n[Defaults]', 'computer = shiner_up', 'keyboard = adb_kbd']
    if display:
        defaults.append('display = ans_cirrus')
    body = OEM_HEADER + '\n'.join(defaults) + '\n' + OEM_COMPUTER + OEM_KEYBOARD
    if display:
        body += OEM_DISPLAY
    return body + '\n[Strings]\n'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--veneer', required=True, help='VENEER.EXE, already patched by mkveneer.py')
    ap.add_argument('--setupldr', required=True, help="PPC/SETUPLDR from the user's CD")
    ap.add_argument('--hal', required=True, help='build/hal.dll — installed as HALSHINR.DLL')
    ap.add_argument('--kbd', help='the driver installed as I8042PRT.SYS (optional while C5 is open)')
    ap.add_argument('--display-driver', help='the display miniport, installed as CIRRUS.SYS')
    ap.add_argument('--display-dll', help='the display DLL, installed as CIRRUS.DLL')
    ap.add_argument('--vga-aperture', type=lambda x: int(x, 0), metavar='ADDR', default=None,
                    help='ledger row 6: move the display miniport\'s legacy-VGA-aperture claim '
                         'from 0xA0000 to ADDR, which must be at or above 0x80000000 — see '
                         'move_vga_aperture(). 0x90000000 is what this project uses')
    ap.add_argument('--oem', help='a txtsetup.oem to use instead of the one built in')
    ap.add_argument('--label', default='NTOEMDISK')
    a = ap.parse_args()

    def read(p):
        with open(p, 'rb') as f:
            return f.read()

    if bool(a.display_driver) != bool(a.display_dll) or (a.vga_aperture is not None
                                                         and not a.display_driver):
        sys.exit('the display class needs both --display-driver and --display-dll, or neither')

    fs = Fat12(**GEOM)
    veneer = read(a.veneer)
    vcluster, vlba = fs.alloc(veneer)
    if vcluster != 2:
        sys.exit('the veneer did not land on cluster 2 — it must be allocated first')
    vblocks = (len(veneer) + SECTOR - 1) // SECTOR

    # `\\PPC` mirrors the CD, because the veneer's boot-file path is `\\PPC\\SETUPLDR` and it
    # resolves that on whatever device it booted from.  The OEM files go in the **root**: a
    # `[Disks]` line's third field is the directory Setup prefixes to every filename it reads
    # from the disk, and Setup asked for `\\halshinr.dll` when that field was `\\`.
    ppc = [fs.dirent('VENEER.EXE', vcluster, len(veneer))]
    root = []
    for name, path, where in (('SETUPLDR', a.setupldr, ppc),
                              ('HALSHINR.DLL', a.hal, root),
                              ('I8042PRT.SYS', a.kbd, root),
                              ('CIRRUS.SYS', a.display_driver, root),
                              ('CIRRUS.DLL', a.display_dll, root)):
        if path is None:
            print(f'  ({name} omitted -- no path given)')
            continue
        data = read(path)
        if name == 'CIRRUS.SYS' and a.vga_aperture is not None:
            data = move_vga_aperture(data, a.vga_aperture)
        c, _ = fs.alloc(data)
        where.append(fs.dirent(name, c, len(data)))

    display = bool(a.display_driver and a.display_dll)
    oem = read(a.oem) if a.oem else txtsetup_oem(display).encode('ascii')
    oc, _ = fs.alloc(oem)

    root = [fs.subdir('PPC', ppc), fs.dirent('TXTSETUP.OEM', oc, len(oem))] + root
    img = fs.finish(root, a.label)
    with open(a.out, 'wb') as f:
        f.write(img)

    used = fs.next_free - 2
    print(f'{a.out}: {len(img)} bytes, FAT12, {used}/{fs.clusters} clusters used')
    print(f'  \\PPC\\VENEER.EXE   {len(veneer)} bytes at block {vlba} ({vlba:#x}), '
          f'{vblocks} blocks ({vblocks:#x}) — contiguous')
    print(f'  read it with:  mkcoldboot.py --veneer-dev /bandit/gc/swim3 '
          f'--veneer-block {vlba:#x} --veneer-blocks {vblocks:#x}')


if __name__ == '__main__':
    main()
