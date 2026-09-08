#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
"""fatput.py <disk.img> <partno> <\\PATH\\TO\\FILE> <src> — overwrite one FAT16 file's data
in place.  The replacement must be exactly the same size, so the existing cluster chain still
fits and no directory entry or FAT chain has to change.

This exists for one job (wall 47): restoring a hive from its own `.SAV` copy, which NT's repair
option does the same way.  See fatls.py."""
import os, sys, struct
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'fatls.py')).read().split("f = open(sys.argv[1], 'rb')")[0]
mod = type(sys)('fatls'); exec(compile(src, 'fatls', 'exec'), mod.__dict__)
u32 = mod.u32

img, partno, path, newf = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
data = open(newf, 'rb').read()
f = open(img, 'r+b'); f.seek(0); mbr = f.read(512)
e = mbr[0x1be + (partno-1)*16: 0x1be + partno*16]
fat = mod.Fat(f, u32(e, 8))
cluster, size = 0, 0
for comp in [c for c in path.split('\\') if c]:
    hit = [x for x in fat.read_dir(cluster) if x['name'].upper() == comp.upper()]
    if not hit: sys.exit(f'not found: {comp}')
    cluster, size = hit[0]['cluster'], hit[0]['size']
if size != len(data):
    sys.exit(f'size mismatch: {path} is {size}, {newf} is {len(data)}')
per = fat.spc * fat.bps
c, written = cluster, 0
while 2 <= c < 0xfff8 and written < size:
    f.seek((fat.data_start + (c - 2) * fat.spc) * 512)
    f.write(data[written:written + per].ljust(per, b'\0'))
    written += per
    c = fat.next_cluster(c)
f.flush(); f.close()
print(f'{path}: wrote {size} bytes from {newf}')
