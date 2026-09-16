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
import argparse, os, re, struct, sys

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


# SETUPLDR has OEM prompts for SCSI, Computer and Display and none for the keyboard -- and the
# SCSI class is the one it loads *any number* of drivers for, in a loop, without checking what
# they are.  maciNTosh ships its ADB keyboard/mouse driver exactly this way: at the mass-storage
# screen, S, Other, and "PowerMac General HID & Storage" is an entry in its txtsetup.oem's
# [SCSI] section.  The driver just has to be an NT driver that creates the port devices kbdclass
# and mouclass open -- this one creates \\Device\\KeyboardPort and \\Device\\PointerPort --
# and nothing in SETUPLDR minds that it is not a SCSI miniport.  The file and key are usbadb,
# not i8042prt, because the CD's real i8042prt.sys is loaded by name as well and two services
# cannot share one.
OEM_SCSI = """
[SCSI]
adbport = "Apple Desktop Bus keyboard and mouse, and the OEM disk (powermac-nt-hal)"

[Files.SCSI.adbport]
driver = d1, adbport.sys, adbport

[Config.adbport]
"""


# ---- the Open Firmware boot script ------------------------------------------------------------
#
# What the user runs instead of typing the layout by hand:
#
#     0 > load fd:,\boot.of
#     0 > load-base loadsize eval
#
# Two lines, and `fd` is a devalias the ROM already ships.  The comma matters: `fd:\boot.of`
# fails `PARTITION is not a number`, because Open Firmware parses what follows `:` as a
# partition number -- the syntax is device:partition,path.
#
# Three things about this file are load-bearing and were each measured at the 0 > prompt:
#
#  * **CRLF line endings.**  `\` comments run to end of *line*; with LF only, Open Firmware never
#    sees a line end and swallows the whole file as one comment.  It evaluates in silence and
#    does nothing, which looks exactly like `eval` not working.
#  * **Everything is wrapped in one colon definition.**  `load` puts this text at `load-base`,
#    which is 3E00000 here -- the very address the veneer is moved to.  Compiling first and
#    running afterwards means the text has already been consumed when the move destroys it, and
#    `go` never returns to read more.  Setting `load-base` elsewhere is not an option:
#    `3E00000 to load-base` answers `invalid use of TO`.
#  * **`map-space` is called as a method, not through `dev`.**  `dev` is interpret-only, and
#    `map-space` exists only inside `/packages/pe-loader`; `" map-space" pe $call-method` reaches
#    it from inside a definition, which is the same shape the read already uses.
CHUNK = 0x20                     # blocks per read-blocks call, as the firmware's own transcript does


def oemdisk_constants():
    """OEMDISK_* from include/oemdisk.h -- the contract the HAL and drivers/adbport are compiled
    against.  Read at run time so the three cannot drift apart silently; the fallbacks are the
    values as of the first draft, used only when this tool runs away from its checkout."""
    defaults = dict(OEMDISK_PHYS=0x03B97000, OEMDISK_HEADER_SIZE=0x1000, OEMDISK_MAGIC=0x4F534E41,
                    OEMDISK_VERSION=1, OEMDISK_BLOCK=512)
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'include', 'oemdisk.h')
    try:
        text = open(path).read()
    except OSError:
        return defaults
    out = dict(defaults)
    for k in defaults:
        m = re.search(r'#define\s+%s\s+(0x[0-9A-Fa-f]+|\d+)u?' % k, text)
        if m: out[k] = int(m.group(1), 0)
    return out



def boot_script(veneer_block, veneer_blocks, veneer_bytes, fd_dev, cd_dev, disk_blocks,
                stage=0x3D00000, load=0x3E00000):
    """disk_blocks: the whole floppy, 2880 for 1.44 MB.  It is read into RAM behind the veneer so
    that drivers/adbport can serve it to Setup as \\Device\\Floppy0 once NT is running --
    the copy of OEM files onto the hard disk happens under NT, and NT has no SWIM3 driver.
    The contract is include/oemdisk.h; the HAL checks the header and fences the pages."""
    C = oemdisk_constants()
    hdr, img = C['OEMDISK_PHYS'], C['OEMDISK_PHYS'] + C['OEMDISK_HEADER_SIZE']
    img_bytes = disk_blocks * C['OEMDISK_BLOCK']
    # Claim whole pages for the veneer's two areas.  The file is 0x27800 bytes, 39 1/2 pages; claim
    # exactly that and the firmware's free list acquires a boundary at 0x3D27800, which the veneer's
    # own descriptor code decodes as page 0x80003D27 and rejects ("is not in installed memory",
    # E24).  pe-loader rounds its own claims the same way (`loadsize fff + -1000 and`).
    claim = (veneer_bytes + 0xFFF) & ~0xFFF
    L = ['\\ powermac-nt-hal -- start Windows NT Setup on an Apple Network Server 500/700',
         '\\',
         '\\ Run this at the 0 > prompt with:',
         '\\     load fd:,\\boot.of',
         '\\     load-base loadsize eval',
         '\\',
         '\\ Everything below is Open Firmware\'s own; nothing is patched at run time.  The veneer',
         '\\ on this disk already carries its patches as bytes.',
         '',
         '0 value nt-pe',
         '0 value nt-fd',
         '',
         ': nt-boot',
         f'   " /packages/pe-loader" open-dev to nt-pe',
         f'   {stage:X} {claim:X} " map-space" nt-pe $call-method',
         f'   " {fd_dev}" open-dev to nt-fd']
    addr, blk, left = stage, veneer_block, veneer_blocks
    while left > 0:
        n = min(CHUNK, left)
        L.append(f'   {addr:X} {blk:X} {n:X} " read-blocks" nt-fd $call-method drop')
        addr += n * 512
        blk += n
        left -= n
    L += [f'   {load:X} {claim:X} " map-space" nt-pe $call-method',
          f'   {stage:X} {load:X} {veneer_bytes:X} move',
          f'   {veneer_bytes:X} to loadsize',
          '   init-program',
          # Not diagnostics.  mkcoldboot.py maps this low page after init-program and the boot
          # works; leave it out and the veneer dies at its own 0x52FC8 with a DSI
          # (`DEFAULT CATCH!, code=FFF00300`).  Ledger row 1 nops the veneer's own `claim` of the
          # SYSTEM PARAMETER BLOCK and RESTART BLOCK, which live down here -- so with the claim
          # skipped, somebody still has to map the page, and this is who.
          f'   4000 1000 " map-space" nt-pe $call-method']
    # The OEM disk: map header page + image, read every block, sum what read-blocks returned.
    # This comes *after* the three small mappings on purpose.  pe-loader's map-space is
    # `claim-mem claim-virt do-map`, and with the 0x169000-byte region claimed first, the claim of
    # the veneer's load address fails ("CLAIM failed") and the definition aborts before `go` --
    # silently, from the outside, since nothing else is printed.  Claimed last, all four go
    # through (probe of 2026-09-16; the boot-floppy note has the transcript).
    # A full 1.44 MB read is a minute or two of a real drive's time and, on the emulator, most of
    # the boot -- with nothing on the console meanwhile it is indistinguishable from a hang, and
    # was read as one the first time.  So: say what is happening, and a dot per ten chunks.
    L += ['   \\ the whole disk again, for Windows NT: see include/oemdisk.h',
          '   ." powermac-nt-hal: reading the OEM disk into RAM (this takes a while) " ',
          f'   {hdr:X} {C["OEMDISK_HEADER_SIZE"] + img_bytes:X} " map-space" nt-pe $call-method',
          '   0']
    addr, blk, left, k = img, 0, disk_blocks, 0
    while left > 0:
        n = min(CHUNK, left)
        L.append(f'   {addr:X} {blk:X} {n:X} " read-blocks" nt-fd $call-method +' + ('  ." ."' if k % 10 == 9 else ''))
        addr += n * 512
        blk += n
        left -= n
        k += 1
    L += ['   dup cr ." powermac-nt-hal: " . ." blocks of the OEM disk are in RAM" cr',
          f'   {hdr + 0x14:X} !                     \\ BlocksRead: the sum',
          f'   {C["OEMDISK_MAGIC"]:X} {hdr:X} !         \\ Magic',
          f'   {C["OEMDISK_VERSION"]:X} {hdr + 4:X} !       \\ Version',
          f'   {img:X} {hdr + 8:X} !               \\ ImagePhys',
          f'   {img_bytes:X} {hdr + 0xC:X} !          \\ ImageBytes',
          f'   {C["OEMDISK_BLOCK"]:X} {hdr + 0x10:X} !      \\ BlockBytes',
          f'   0 {hdr + 0x18:X} !   0 {hdr + 0x1C:X} !',
          '   nt-fd close-dev',
          f'   " {cd_dev}" encode-string " bootpath" _chosen (property)',
          '   ." powermac-nt-hal: starting Windows NT Setup" cr',
          '   go',
          ';',
          '',
          'nt-boot']
    return '\r\n'.join(L) + '\r\n'


def arc_environment(disk_arc='multi(0)scsi(1)disk(0)rdisk(0)', sys_part=1, os_part=2,
                    osloader=r'\os\winnt40\osloader.exe', winnt=r'\WINNT', options='NODEBUG',
                    identifier='Windows NT Workstation Version 4.00'):
    """The ten ARC variables a boot of the installed system reads, as (NAME, value)."""
    return [
        ('SYSTEMPARTITION', f'{disk_arc}partition({sys_part})'),
        ('OSLOADER',        f'{disk_arc}partition({sys_part}){osloader}'),
        ('OSLOADPARTITION', f'{disk_arc}partition({os_part})'),
        ('OSLOADFILENAME',  winnt),
        ('OSLOADOPTIONS',   options),
        ('LOADIDENTIFIER',  identifier),
        ('AUTOLOAD',        'YES'),
        ('COUNTDOWN',       '5'),
        ('LASTKNOWNGOOD',   'FALSE'),
        ('PROCESSORS',      '1'),
    ]


def boot_disk_script(veneer_block, veneer_blocks, veneer_bytes, fd_dev, disk_dev,
                     loader_base=0x80600000, loader_size=0x4A800, stage=0x3D00000, load=0x3E00000):
    """\\BOOTDISK.OF: boot the system text-mode Setup installed on the hard disk.

    The same shape as \\BOOT.OF with the OEM disk left out, a `--for disk` veneer read instead of
    the CD one and /chosen bootpath aimed at the disk.  The ARC environment is not here: it is
    NVRAM, set once by \\SETUP.OF (see setup_script), which is where the veneer reads it from.

    loader_base/loader_size are OSLOADER.EXE's ImageBase and SizeOfImage.  The veneer's load_file
    claims exactly SizeOfImage bytes at ImageBase's physical address, and NT 4.0's loader is
    0x4A800 bytes -- not a page multiple.  The firmware's free list then starts at 0x64A800, and
    the veneer's own decoder (page = base >> 12, plus the low twelve bits rotated to the top)
    turns that into page 0x8000064A, which "is not in installed memory": a fatal, and EXIT back
    to the prompt.  SETUPLDR is 0x62000 bytes, 98 pages exactly, which is why the CD boot never
    met this.  So the sliver from the end of the image to the next page boundary is claimed here,
    before `go`; the veneer's claim still fits, and the free list stays page-aligned (E24)."""
    # Claim whole pages for the veneer's two areas.  The file is 0x27800 bytes, 39 1/2 pages; claim
    # exactly that and the firmware's free list acquires a boundary at 0x3D27800, which the veneer's
    # own descriptor code decodes as page 0x80003D27 and rejects ("is not in installed memory",
    # E24).  pe-loader rounds its own claims the same way (`loadsize fff + -1000 and`).
    claim = (veneer_bytes + 0xFFF) & ~0xFFF
    L = ['\\ powermac-nt-hal -- boot the Windows NT system installed on the hard disk',
         '\\',
         '\\ Run this at the 0 > prompt with:',
         '\\     load fd:,\\bootdisk.of',
         '\\     load-base loadsize eval',
         '\\',
         '\\ The veneer read here is the `--for disk` one: no CD-era patches, and the loader path',
         '\\ \\OS\\WINNT40\\OSLOADER.EXE baked in.  The ARC environment is in NVRAM, from setup.of;',
         '\\ nothing is patched at run time.',
         '',
         '0 value nt-pe',
         '0 value nt-fd',
         '',
         ': nt-boot-disk',
         f'   " /packages/pe-loader" open-dev to nt-pe',
         f'   {stage:X} {claim:X} " map-space" nt-pe $call-method',
         f'   " {fd_dev}" open-dev to nt-fd']
    addr, blk, left = stage, veneer_block, veneer_blocks
    while left > 0:
        n = min(CHUNK, left)
        L.append(f'   {addr:X} {blk:X} {n:X} " read-blocks" nt-fd $call-method drop')
        addr += n * 512
        blk += n
        left -= n
    L += ['   nt-fd close-dev',
          f'   {load:X} {claim:X} " map-space" nt-pe $call-method',
          f'   {stage:X} {load:X} {veneer_bytes:X} move',
          f'   {veneer_bytes:X} to loadsize',
          '   init-program',
          # the same low page boot.of maps, for the same reason (ledger row 1)
          f'   4000 1000 " map-space" nt-pe $call-method']
    pad = (-loader_size) % 0x1000
    if pad:
        end = (loader_base & 0x7FFFFFFF) + loader_size
        L += ['   \\ the loader is not a whole number of pages; claim the rest of its last page (E24)',
              f'   {end:X} {pad:X} " map-space" nt-pe $call-method']
    L += [f'   " {disk_dev}" encode-string " bootpath" _chosen (property)',
          '   ." powermac-nt-hal: starting the installed Windows NT" cr',
          '   go',
          ';',
          '',
          'nt-boot-disk']
    return '\r\n'.join(L) + '\r\n'


def setup_script(env=None, real_base=0x3F00000, load_base=0x3E00000):
    """The once-per-machine half.  `little-endian?` is firmware NVRAM and `reset-all` is what
    applies it, so no medium can set it before the firmware has read the medium -- this cannot be
    folded into boot.of, and any claim of "insert and go" on a virgin machine is false.

    `env` is the ARC environment (arc_environment()), stored the same way: the veneer reads ARC
    variables as properties of /options, which is this firmware's NVRAM -- `setenv NAME value`
    creates one and it survives reset-all (probed 2026-09-16).  A real ARC machine keeps these in
    NVRAM too, written by ARCINST; this is that, and it retires ledger rows 11, 12 and 16."""
    L = ['\\ powermac-nt-hal -- configure this machine for Windows NT.  Run once:',
         '\\     load fd:,\\setup.of',
         '\\     load-base loadsize eval',
         '\\ The machine resets at the end; then run boot.of the same way, every boot.',
         '',
         '." powermac-nt-hal: setting little-endian mode, then resetting" cr',
         'setenv little-endian? true',
         'setenv real-mode? false',
         f'setenv real-base {real_base:X}',
         f'setenv load-base {load_base:X}']
    if env:
        L.append('\\ the ARC environment of the installed system, where the veneer reads it: NVRAM')
        L += [f'setenv {name} {value}' for name, value in env]
    L.append('reset-all')
    return '\r\n'.join(L) + '\r\n'



def txtsetup_oem(display, adb=False):
    defaults = ['\n[Defaults]', 'computer = shiner_up']
    if display:
        defaults.append('display = ans_cirrus')
    if adb:
        defaults.append('scsi = adbport')
    body = OEM_HEADER + '\n'.join(defaults) + '\n' + OEM_COMPUTER
    if display:
        body += OEM_DISPLAY
    if adb:
        body += OEM_SCSI
    return body + '\n[Strings]\n'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--veneer', required=True, help='VENEER.EXE, already patched by mkveneer.py')
    ap.add_argument('--setupldr', required=True, help="PPC/SETUPLDR from the user's CD")
    ap.add_argument('--hal', required=True, help='build/hal.dll — installed as HALSHINR.DLL')
    ap.add_argument('--adb-driver', metavar='PATH',
                    help='the ADB keyboard/mouse/OEM-disk driver, installed as ADBPORT.SYS and '
                         'offered under the SCSI prompt (S, Other) -- the one OEM class SETUPLDR '
                         'loads any number of drivers for. Default: build/adbport.sys, this '
                         "project's own; pass another binary to try it in the same slot")
    ap.add_argument('--no-adb-driver', action='store_true', help='leave the [SCSI] class off the disk')
    ap.add_argument('--boot-script', metavar='PATH',
                    help='use this file as \\BOOT.OF instead of the generated one')
    ap.add_argument('--disk-veneer', metavar='PATH',
                    help='VENEER.EXE patched by `mkveneer.py --for disk`; placed as \\PPC\\VENEERD.EXE, '
                         'contiguous, with \\BOOTDISK.OF to boot the installed system from it')
    ap.add_argument('--osloader-exe', metavar='PATH',
                    help="OSLOADER.EXE (the CD's PPC/OSLOADER.EXE, or the installed disk's), for its "
                         "ImageBase and SizeOfImage; default: NT 4.0's, 0x80600000 and 0x4A800")
    ap.add_argument('--disk-dev', default='/bandit/53c825@12/sd@0,0',
                    help='the Open Firmware path of the hard disk, for /chosen bootpath')
    ap.add_argument('--disk-arc', default='multi(0)scsi(1)disk(0)rdisk(0)',
                    help='the same disk as an ARC path, for the environment')
    ap.add_argument('--system-partition', type=int, default=1)
    ap.add_argument('--os-partition', type=int, default=2)
    ap.add_argument('--osloader', default=r'\os\winnt40\osloader.exe')
    ap.add_argument('--winnt', default=r'\WINNT')
    ap.add_argument('--os-options', default='NODEBUG')
    ap.add_argument('--identifier', default='Windows NT Workstation Version 4.00')
    ap.add_argument('--fd-dev', default='/bandit/gc/swim3',
                    help='Open Firmware path of this drive, for boot.of to read the veneer from')
    ap.add_argument('--cd-dev', default='/bandit/53c825@11/sd@0,0',
                    help="Open Firmware path of the CD, which boot.of makes /chosen bootpath")
    ap.add_argument('--no-scripts', action='store_true',
                    help='leave \\BOOT.OF and \\SETUP.OF off the disk')
    ap.add_argument('--tag', metavar='PATH',
                    help="the distribution's media tag file, e.g. the CD's own CDROM_W.40, "
                         'placed in this disk\'s root under the same name. `[SourceDisksNames]` '
                         'in TXTSETUP.SIF identifies each source medium by such a file, and a '
                         'floppy boot stops asking for "the disk labeled Windows NT Workstation '
                         "CD-ROM\" once it finds one. Experimental: it makes Setup treat this "
                         'disk as the distribution, which a 1.44 MB disk is not')
    ap.add_argument('--sif', help="the CD's PPC/TXTSETUP.SIF, placed at \\PPC\\TXTSETUP.SIF. Only "
                                  'needed to boot *from* the floppy (the plan\'s section 2.2): '
                                  'SETUPLDR reads its INF from the device it booted from, and '
                                  'without one it stops at "INF file txtsetup.sif is corrupt or '
                                  'missing". Not needed for the OEM-disk arrangement of 2.1')
    ap.add_argument('--kbd', help='(no longer needed) a driver to place as I8042PRT.SYS; the '
                                  'keyboard now arrives as adbport.sys under [SCSI]')
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

    adb_driver = None
    if not a.no_adb_driver:
        adb_driver = a.adb_driver or os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'adbport.sys')
        if not os.path.exists(adb_driver):
            sys.exit(f'{adb_driver}: not found -- `make` builds it, or --adb-driver PATH, or --no-adb-driver')

    fs = Fat12(**GEOM)
    veneer = read(a.veneer)
    vcluster, vlba = fs.alloc(veneer)
    if vcluster != 2:
        sys.exit('the veneer did not land on cluster 2 — it must be allocated first')
    vblocks = (len(veneer) + SECTOR - 1) // SECTOR
    dveneer = read(a.disk_veneer) if a.disk_veneer else None
    if dveneer is not None:
        dcluster, dlba = fs.alloc(dveneer)          # second, so contiguous as well
        dblocks = (len(dveneer) + SECTOR - 1) // SECTOR

    # `\\PPC` mirrors the CD, because the veneer's boot-file path is `\\PPC\\SETUPLDR` and it
    # resolves that on whatever device it booted from.  The OEM files go in the **root**: a
    # `[Disks]` line's third field is the directory Setup prefixes to every filename it reads
    # from the disk, and Setup asked for `\\halshinr.dll` when that field was `\\`.
    ppc = [fs.dirent('VENEER.EXE', vcluster, len(veneer))]
    if dveneer is not None:
        ppc.append(fs.dirent('VENEERD.EXE', dcluster, len(dveneer)))
    root = []
    for name, path, where in (('SETUPLDR', a.setupldr, ppc),
                              ('TXTSETUP.SIF', a.sif, ppc),
                              ('HALSHINR.DLL', a.hal, root),
                              ('I8042PRT.SYS', a.kbd, root),
                              ('CIRRUS.SYS', a.display_driver, root),
                              ('CIRRUS.DLL', a.display_dll, root),
                              ('ADBPORT.SYS', adb_driver, root)):
        if path is None:
            print(f'  ({name} omitted -- no path given)')
            continue
        data = read(path)
        if name == 'CIRRUS.SYS' and a.vga_aperture is not None:
            data = move_vga_aperture(data, a.vga_aperture)
        c, _ = fs.alloc(data)
        where.append(fs.dirent(name, c, len(data)))

    display = bool(a.display_driver and a.display_dll)
    # Booting *from* the floppy means SETUPLDR resolves `\\PPC\\I8042PRT.SYS` -- the name it
    # hardcodes for the keyboard port driver -- on this device rather than on the CD.  That is
    # the only route by which an ADB keyboard driver reaches text-mode Setup without writing to
    # the user's disc: SETUPLDR has OEM prompts for SCSI, Computer and Display and none for the
    # keyboard, so `txtsetup.oem`'s `[Keyboard]` section is read by `setupdd.sys` under NT, long
    # after Setup needs a keyboard.  The root copy stays for the OEM-disk arrangement.
    if not a.no_scripts:
        boot = (read(a.boot_script) if a.boot_script
                else boot_script(vlba, vblocks, len(veneer), a.fd_dev, a.cd_dev,
                                 GEOM['total']).encode('ascii'))
        c, _ = fs.alloc(boot)
        root.append(fs.dirent('BOOT.OF', c, len(boot)))
        env = arc_environment(a.disk_arc, a.system_partition, a.os_partition, a.osloader,
                              a.winnt, a.os_options, a.identifier)
        setup = setup_script(env).encode('ascii')
        c2, _ = fs.alloc(setup)
        root.append(fs.dirent('SETUP.OF', c2, len(setup)))
        print(f'  \\SETUP.OF  {len(setup)} bytes   once per machine: load fd:,\\setup.of  '
              f'then  load-base loadsize eval')
        print(f'  \\BOOT.OF   {len(boot)} bytes   every boot:       load fd:,\\boot.of   '
              f'then  load-base loadsize eval')
        if dveneer is not None:
            lb, ls = 0x80600000, 0x4A800
            if a.osloader_exe:
                h = read(a.osloader_exe)
                lb, ls = struct.unpack_from('<I', h, 20 + 28)[0], struct.unpack_from('<I', h, 20 + 56)[0]
            bd = boot_disk_script(dlba, dblocks, len(dveneer), a.fd_dev, a.disk_dev, lb, ls).encode('ascii')
            c3, _ = fs.alloc(bd)
            root.append(fs.dirent('BOOTDISK.OF', c3, len(bd)))
            print(f'  \\BOOTDISK.OF   {len(bd)} bytes   the installed system: load fd:,\\bootdisk.of  '
                  f'then  load-base loadsize eval')

    if a.tag:
        data = read(a.tag)
        c, _ = fs.alloc(data)
        root.append(fs.dirent(os.path.basename(a.tag).upper(), c, len(data)))
        print(f'  \\{os.path.basename(a.tag).upper()}   media tag, {len(data)} bytes')

    if a.sif and a.kbd:
        data = read(a.kbd)
        c, _ = fs.alloc(data)
        ppc.append(fs.dirent('I8042PRT.SYS', c, len(data)))
        print('  \\PPC\\I8042PRT.SYS   a second copy, for booting from this disk')

    oem = read(a.oem) if a.oem else txtsetup_oem(display, adb_driver is not None).encode('ascii')
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
    if dveneer is not None:
        print(f'  \\PPC\\VENEERD.EXE  {len(dveneer)} bytes at block {dlba} ({dlba:#x}), '
              f'{dblocks} blocks ({dblocks:#x}) — contiguous, for \\BOOTDISK.OF')


if __name__ == '__main__':
    main()
