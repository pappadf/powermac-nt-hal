#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
"""fatls.py <disk.img> — list the FAT16 partitions of an MBR disk, and walk their directories.

Reads what the emulated machine wrote, from the host, with nothing mounted: `mount -o loop` is
not available in a container and `mtools` is not a dependency worth adding.  This is how an
install is checked after the fact — that `\\OS\\WINNT40\\HAL.DLL` really is the HAL that was
built, and (wall 47) that the `SYSTEM` hive text-mode Setup left behind was only partly written.

`fatcat.py` extracts one file; `fatput.py` writes one back.
"""
import struct, sys


def u16(b, o): return struct.unpack_from('<H', b, o)[0]
def u32(b, o): return struct.unpack_from('<I', b, o)[0]


class Fat:
    def __init__(self, f, start):
        f.seek(start * 512)
        bs = f.read(512)
        self.f, self.start = f, start
        self.bps = u16(bs, 0x0b)
        self.spc = bs[0x0d]
        self.res = u16(bs, 0x0e)
        self.nfat = bs[0x10]
        self.roote = u16(bs, 0x11)
        self.spf = u16(bs, 0x16)
        self.total = u16(bs, 0x13) or u32(bs, 0x20)
        self.label = bs[0x2b:0x36].decode('latin1').strip()
        self.fstype = bs[0x36:0x3e].decode('latin1').strip()
        self.fat_start = start + self.res
        self.root_start = self.fat_start + self.nfat * self.spf
        self.root_sectors = (self.roote * 32 + self.bps - 1) // self.bps
        self.data_start = self.root_start + self.root_sectors
        f.seek(self.fat_start * 512)
        self.fat = f.read(self.spf * 512)

    def next_cluster(self, c):
        return u16(self.fat, c * 2)

    def read_dir(self, cluster):
        """directory entries, following the cluster chain (cluster 0 = the root)."""
        out = []
        if cluster == 0:
            self.f.seek(self.root_start * 512)
            out.append(self.f.read(self.root_sectors * self.bps))
        else:
            seen = 0
            while 2 <= cluster < 0xfff8 and seen < 4096:
                lba = self.data_start + (cluster - 2) * self.spc
                self.f.seek(lba * 512)
                out.append(self.f.read(self.spc * self.bps))
                cluster = self.next_cluster(cluster)
                seen += 1
        blob = b''.join(out)
        ents = []
        for i in range(0, len(blob), 32):
            e = blob[i:i + 32]
            if not e or e[0] == 0: break
            if e[0] == 0xe5 or e[11] == 0x0f: continue      # deleted, or a long-name slot
            name = e[0:8].decode('latin1').rstrip()
            ext = e[8:11].decode('latin1').rstrip()
            ents.append({'name': name + ('.' + ext if ext else ''),
                         'dir': bool(e[11] & 0x10), 'vol': bool(e[11] & 0x08),
                         'cluster': u16(e, 26), 'size': u32(e, 28)})
        return ents

    def walk(self, cluster=0, path='', depth=0, limit=3):
        for e in self.read_dir(cluster):
            if e['vol'] or e['name'] in ('.', '..'): continue
            p = path + '\\' + e['name']
            yield depth, p, e
            if e['dir'] and depth < limit:
                yield from self.walk(e['cluster'], p, depth + 1, limit)


f = open(sys.argv[1], 'rb')
f.seek(0)
mbr = f.read(512)
for i in range(4):
    e = mbr[0x1be + i*16: 0x1be + (i+1)*16]
    if e[4] == 0: continue
    start, length = u32(e, 8), u32(e, 12)
    print(f'\n=== partition {i+1}: type 0x{e[4]:02x}, LBA {start}, {length} sectors '
          f'({length//2048} MB) ===')
    try:
        fat = Fat(f, start)
    except Exception as ex:
        print('   not readable as FAT:', ex); continue
    print(f'   {fat.fstype!r} label {fat.label!r}  {fat.spc} sec/cluster  '
          f'{fat.spf} sec/FAT  root at LBA {fat.root_start}')
    n = 0
    for depth, p, e2 in fat.walk(limit=int(sys.argv[2]) if len(sys.argv) > 2 else 2):
        kind = '<DIR>' if e2['dir'] else f"{e2['size']:>9}"
        print(f'   {kind}  {p}')
        n += 1
        if n > 120:
            print('   ... (truncated)'); break
    if n == 0: print('   (empty)')
