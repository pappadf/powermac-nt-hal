#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
"""mkarcdisk.py — put an MBR and empty FAT16 partitions on a raw disk image.

NT's text-mode Setup on an ARC machine refuses to start until it finds a FAT partition with at
least 750 KB free on the boot disk: on real hardware the firmware vendor's ARCINST.EXE creates
it, and the Network Server's Open Firmware has no such tool.  This writes the same thing by
hand — primary FAT16 partitions, formatted empty — so Setup has a system partition to put
OSLOADER on, and optionally a second one to install into (which keeps Setup off its own
partition-creation path).  Nothing here is machine-specific; it is a plain MBR and a plain FAT16
BPB.

    mkarcdisk.py disk.img                                   whole disk, one partition from LBA 63
    mkarcdisk.py disk.img --part 4096:65536 --part 69632:0   32 MB, then the rest of the disk
"""
import argparse, os, struct, sys


def fat16_boot_sector(total, spc, spf, root_entries, hidden, spt, heads, label):
    bs = bytearray(512)
    bs[0:3] = b'\xeb\x3c\x90'
    bs[3:11] = b'MSDOS5.0'
    struct.pack_into('<H', bs, 0x0b, 512)             # bytes per sector
    bs[0x0d] = spc                                    # sectors per cluster
    struct.pack_into('<H', bs, 0x0e, 1)               # reserved sectors
    bs[0x10] = 2                                      # number of FATs
    struct.pack_into('<H', bs, 0x11, root_entries)
    struct.pack_into('<H', bs, 0x13, total if total < 0x10000 else 0)
    bs[0x15] = 0xf8                                   # media descriptor: fixed disk
    struct.pack_into('<H', bs, 0x16, spf)
    struct.pack_into('<H', bs, 0x18, spt)
    struct.pack_into('<H', bs, 0x1a, heads)
    struct.pack_into('<I', bs, 0x1c, hidden)
    struct.pack_into('<I', bs, 0x20, 0 if total < 0x10000 else total)
    bs[0x24] = 0x80                                   # BIOS drive number
    bs[0x26] = 0x29                                   # extended boot signature
    struct.pack_into('<I', bs, 0x27, 0x4e544653)      # volume serial
    bs[0x2b:0x36] = label.ljust(11)[:11].encode('ascii')
    bs[0x36:0x3e] = b'FAT16   '
    bs[0x1fe:0x200] = b'\x55\xaa'
    return bs


def fat16_geometry(sectors):
    """(sectors per cluster, sectors per FAT, root-directory sectors, cluster count) for FAT16.

    Grow the cluster until the count is inside FAT16's window, then size the FAT to the clusters
    it must describe — the FAT's own sectors come out of the data area, so it settles by
    iteration rather than in one step."""
    root_entries, spc = 512, 1
    root = (root_entries * 32 + 511) // 512
    while True:
        spf = 1
        for _ in range(64):
            clusters = (sectors - 1 - 2 * spf - root) // spc
            need = ((clusters + 2) * 2 + 511) // 512
            if need <= spf:
                break
            spf = need
        clusters = (sectors - 1 - 2 * spf - root) // spc
        if clusters > 65524 and spc < 64:
            spc *= 2
            continue
        if clusters < 4085 or clusters > 65524:
            sys.exit(f'mkarcdisk: {clusters} clusters at {spc} sectors/cluster is not FAT16')
        return spc, spf, root, clusters


def chs(lba, spt, heads):
    c, h, s = lba // (spt * heads), (lba // spt) % heads, (lba % spt) + 1
    if c > 1023:
        c, h, s = 1023, heads - 1, spt
    return bytes((h, s | ((c >> 2) & 0xc0), c & 0xff))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('image')
    ap.add_argument('--spt', type=int, default=63)
    ap.add_argument('--heads', type=int, default=16)
    ap.add_argument('--part', action='append', default=[], metavar='START:SECTORS[:LABEL]',
                    help='a primary FAT16 partition; SECTORS 0 means "to the end of the disk". '
                         'Repeatable, up to four. Default: 63:0')
    ap.add_argument('--start', type=int, default=63, help='shorthand for a single --part')
    ap.add_argument('--sectors', type=int, default=0, help='shorthand for a single --part')
    ap.add_argument('--label', default='ARCSYSTEM')
    a = ap.parse_args()

    size = os.path.getsize(a.image)
    if size % 512:
        sys.exit('mkarcdisk: image is not a whole number of sectors')
    disk = size // 512

    specs = a.part or [f'{a.start}:{a.sectors}:{a.label}']
    if len(specs) > 4:
        sys.exit('mkarcdisk: at most four primary partitions')
    parts = []
    for spec in specs:
        f = spec.split(':')
        start = int(f[0])
        sectors = int(f[1]) if len(f) > 1 and f[1] else 0
        label = f[2] if len(f) > 2 and f[2] else a.label
        if sectors == 0:
            sectors = disk - start
        if start + sectors > disk:
            sys.exit(f'mkarcdisk: partition {spec} runs off the end of the disk')
        parts.append((start, sectors, label))

    mbr = bytearray(512)
    struct.pack_into('<I', mbr, 0x1b8, 0x4e544844)    # NT disk signature
    mbr[0x1fe:0x200] = b'\x55\xaa'

    with open(a.image, 'r+b') as f:
        for i, (start, sectors, label) in enumerate(parts):
            spc, spf, root, clusters = fat16_geometry(sectors)
            e = 0x1be + i * 16
            mbr[e] = 0x80 if i == 0 else 0x00         # the system partition is the active one
            mbr[e + 1:e + 4] = chs(start, a.spt, a.heads)
            mbr[e + 4] = 0x06                         # FAT16 "huge"
            mbr[e + 5:e + 8] = chs(start + sectors - 1, a.spt, a.heads)
            struct.pack_into('<I', mbr, e + 8, start)
            struct.pack_into('<I', mbr, e + 12, sectors)

            fat = bytearray(spf * 512)
            fat[0:4] = b'\xf8\xff\xff\xff'
            f.seek(start * 512)
            f.write(fat16_boot_sector(sectors, spc, spf, 512, start, a.spt, a.heads, label))
            f.seek((start + 1) * 512)
            f.write(fat)
            f.seek((start + 1 + spf) * 512)
            f.write(fat)
            f.seek((start + 1 + 2 * spf) * 512)
            f.write(bytearray(root * 512))
            print(f'{a.image}: partition {i + 1} FAT16 lba {start} len {sectors} '
                  f'({sectors // 2048} MB), {spc} sec/cluster, {clusters} clusters, {spf} sec/FAT')
        f.seek(0)
        f.write(mbr)
    print(f'{a.image}: {disk} sectors, {len(parts)} partition(s)')


main()
