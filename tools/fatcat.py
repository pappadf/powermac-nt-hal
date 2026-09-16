#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
"""fatcat.py <disk.img> <partno> <\\PATH\\TO\\FILE> <out> — extract one file from a FAT16
partition of an MBR disk image, following its cluster chain.  See fatls.py."""
import os, struct, sys
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'fatls.py')).read()
src = src.split("f = open(sys.argv[1], 'rb')")[0]
mod = type(sys)('fatls'); exec(compile(src, 'fatls', 'exec'), mod.__dict__)
u32 = mod.u32

img, partno, path, out = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
f = open(img, 'rb'); f.seek(0); mbr = f.read(512)
e = mbr[0x1be + (partno-1)*16: 0x1be + partno*16]
fat = mod.Fat(f, u32(e, 8))
cluster, size, isdir = 0, 0, True
for comp in [c for c in path.split('\\') if c]:
    hit = [x for x in fat.read_dir(cluster) if x['name'].upper() == comp.upper()]
    if not hit: sys.exit(f'not found: {comp}')
    cluster, size, isdir = hit[0]['cluster'], hit[0]['size'], hit[0]['dir']
data = b''
c = cluster
while 2 <= c < 0xfff8 and len(data) < size:
    lba = fat.data_start + (c - 2) * fat.spc
    f.seek(lba * 512); data += f.read(fat.spc * fat.bps)
    c = fat.next_cluster(c)
open(out, 'wb').write(data[:size])
print(f'{path}: {size} bytes -> {out}')
