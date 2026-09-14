#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
"""fatput.py <disk.img> <partno> <\\PATH\\TO\\FILE> <src> — replace one file's contents in a
FAT16 partition of an MBR disk image, growing or shrinking its cluster chain as needed.

Two jobs, both from the host with nothing mounted (see fatls.py):

  * restoring a hive from its own `.SAV` copy, which is what NT's repair option does (wall 48);
  * **putting a freshly built `hal.dll` onto the installed system** so the next boot runs it,
    which is the iteration loop for everything after text-mode Setup.

The second is why this resizes. A rebuilt HAL is rarely the same size as the one Setup copied,
and padding it to match would change the PE the loader checksums.
"""
import os, struct, sys

src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'fatls.py')).read()
src = src.split("f = open(sys.argv[1], 'rb')")[0]
mod = type(sys)('fatls'); exec(compile(src, 'fatls', 'exec'), mod.__dict__)
u16, u32 = mod.u16, mod.u32

FREE, EOC = 0x0000, 0xFFFF


class Fat16(mod.Fat):
    """fatls.Fat plus the writes: locate a directory entry, and edit the allocation chain."""

    def dir_regions(self, cluster):
        """[(image byte offset, length)] holding a directory's entries, in order."""
        if cluster == 0:
            return [(self.root_start * 512, self.root_sectors * self.bps)]
        out, seen = [], 0
        while 2 <= cluster < 0xfff8 and seen < 4096:
            lba = self.data_start + (cluster - 2) * self.spc
            out.append((lba * 512, self.spc * self.bps))
            cluster = self.next_cluster(cluster); seen += 1
        return out

    def find_entry(self, path):
        """(image offset of the 32-byte entry, first cluster, size) for a \\-separated path."""
        cluster, off, first, size = 0, None, 0, 0
        for comp in [c for c in path.split('\\') if c]:
            found = None
            for base, length in self.dir_regions(cluster):
                self.f.seek(base); blob = self.f.read(length)
                for i in range(0, len(blob), 32):
                    e = blob[i:i + 32]
                    if not e or e[0] == 0: break
                    if e[0] == 0xe5 or e[11] == 0x0f: continue
                    name = e[0:8].decode('latin1').rstrip()
                    ext = e[8:11].decode('latin1').rstrip()
                    if (name + ('.' + ext if ext else '')).upper() == comp.upper():
                        found = (base + i, u16(e, 26), u32(e, 28)); break
                if found: break
            if not found: sys.exit(f'not found: {comp}')
            off, first, size = found
            cluster = first
        return off, first, size

    def chain(self, first):
        out, c = [], first
        while 2 <= c < 0xfff8 and len(out) < 1 << 20:
            out.append(c); c = self.next_cluster(c)
        return out

    def set_next(self, c, v):
        struct.pack_into('<H', self.fat, c * 2, v)

    def free_clusters(self, want):
        """`want` clusters that are currently free, lowest first."""
        total = (self.total - (self.data_start - self.start)) // self.spc + 2
        out = []
        for c in range(2, min(total, len(self.fat) // 2)):
            if u16(self.fat, c * 2) == FREE:
                out.append(c)
                if len(out) == want: return out
        sys.exit(f'only {len(out)} free clusters, need {want}')

    def flush_fat(self):
        for i in range(self.nfat):
            self.f.seek((self.fat_start + i * self.spf) * 512)
            self.f.write(self.fat)

    def write_file(self, path, data):
        entry_off, first, old_size = self.find_entry(path)
        per = self.spc * self.bps
        need = max(1, (len(data) + per - 1) // per)
        have = self.chain(first)
        if need > len(have):
            extra = self.free_clusters(need - len(have))
            for a, b in zip(have[-1:] + extra, extra):
                self.set_next(a, b)
            self.set_next(extra[-1], EOC)
            have += extra
        elif need < len(have):
            for c in have[need:]:
                self.set_next(c, FREE)
            self.set_next(have[need - 1], EOC)
            have = have[:need]
        self.flush_fat()
        for k, c in enumerate(have):
            self.f.seek((self.data_start + (c - 2) * self.spc) * 512)
            self.f.write(data[k * per:(k + 1) * per].ljust(per, b'\0'))
        self.f.seek(entry_off + 28)
        self.f.write(struct.pack('<I', len(data)))
        self.f.flush()
        return old_size, len(data), len(have)


def main():
    img, partno, path, newf = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
    data = open(newf, 'rb').read()
    f = open(img, 'r+b'); f.seek(0); mbr = f.read(512)
    e = mbr[0x1be + (partno - 1) * 16: 0x1be + partno * 16]
    fat = Fat16(f, u32(e, 8))
    fat.fat = bytearray(fat.fat)
    old, new, clusters = fat.write_file(path, data)
    f.close()
    print(f'{path}: {old} -> {new} bytes ({clusters} clusters) from {newf}')


if __name__ == '__main__':
    main()
