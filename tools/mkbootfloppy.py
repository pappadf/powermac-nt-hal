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
  * `\\TXTSETUP.OEM`     the OEM description that offers the HAL as a computer type (C2).

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


TXTSETUP_OEM = """\
; SPDX-License-Identifier: GPL-2.0-only
; The OEM description text-mode Setup reads off this floppy.
;
; Every section and key name here is SETUPLDR's own.  `Disks`, `Defaults`, `Computer`, `Files.`,
; `Config.` and `Strings` sit together in its string table beside the source path
; `D:\\nt\\private\\ntos\\boot\\setup\\oemdisk.c`, and `hal`, `driver`, `inf`, `dll`, `class`, `port`
; and `detect` -- the keys inside a `[Files.<class>.<id>]` section -- sit together a little
; before it.  The shapes of the entries follow the CD's own TXTSETUP.SIF: a `[Computer]` line is
; `id = "description", files-section`, and a `[Keyboard]` line adds the driver's registry key as
; a third field, exactly as `STANDARD = "XT, AT, or Enhanced Keyboard (83-104 keys)",files.i8042,
; i8042prt` does there.
;
; UNTESTED as a whole: what is verified is the format, not that Setup accepts an OEM `Computer`
; in place of the HAL TXTSETUP.SIF names.  That is risk R4 in
; docs/2026-09-15-the-boot-floppy.md.

[Disks]
d1 = "Apple Network Server 500/700 support disk", \\txtsetup.oem, \\

[Defaults]
computer = shiner_up
keyboard = adb_kbd

[Computer]
shiner_up = "Apple Network Server 500/700", files.shiner_up

[Files.Computer.shiner_up]
hal = d1, halshinr.dll

[Config.shiner_up]

[Keyboard]
adb_kbd = "Apple Desktop Bus keyboard", files.adb_kbd, i8042prt

[Files.Keyboard.adb_kbd]
driver = d1, i8042prt.sys, i8042prt

[Config.i8042prt]

[Strings]
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--veneer', required=True, help='VENEER.EXE, already patched by mkveneer.py')
    ap.add_argument('--setupldr', required=True, help="PPC/SETUPLDR from the user's CD")
    ap.add_argument('--hal', required=True, help='build/hal.dll — installed as HALSHINR.DLL')
    ap.add_argument('--kbd', help='the driver installed as I8042PRT.SYS (optional while C5 is open)')
    ap.add_argument('--oem', help='a txtsetup.oem to use instead of the one built in')
    ap.add_argument('--label', default='NTOEMDISK')
    a = ap.parse_args()

    def read(p):
        with open(p, 'rb') as f:
            return f.read()

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
                              ('I8042PRT.SYS', a.kbd, root)):
        if path is None:
            print(f'  (no --kbd: {name} omitted)')
            continue
        data = read(path)
        c, _ = fs.alloc(data)
        where.append(fs.dirent(name, c, len(data)))

    oem = read(a.oem) if a.oem else TXTSETUP_OEM.encode('ascii')
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
