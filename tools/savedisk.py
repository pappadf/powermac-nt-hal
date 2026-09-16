#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
"""savedisk.py <delta|dir> <out.img> [--sectors N] — write out the disk a checkpointed run left
behind, from its copy-on-write delta.

`checkpoint.load` seeds a fresh delta for every writable image and the guest writes into that,
so the base image on disk is never touched and the delta's data area *is* the disk. This is the
inverse of `run-boot.py`'s splice, and it is how an installed system gets captured after
text-mode Setup.

**Capture after the guest has flushed.** Wall 48: an image taken while Setup still had writes
outstanding had 49 files whose cluster chain was shorter than their directory entry claimed —
the FAT caught mid-update. Let Setup reach its restart prompt, press Enter, and give NT time to
shut down before saving. `--check` walks the result and says whether any file is short.
"""
import argparse, glob, os, struct, sys


def delta_geometry(path):
    """(data offset, sector count): a 24-byte header, a two-bits-per-sector map, the sectors."""
    size = os.path.getsize(path)
    n = (size - 24) * 8 // (512 * 8 + 2)
    while 24 + 2 * ((n + 7) // 8) + n * 512 < size:
        n += 1
    if 24 + 2 * ((n + 7) // 8) + n * 512 != size:
        return None, None
    return 24 + 2 * ((n + 7) // 8), n


def short_files(img):
    """[(path, allocated, claimed)] for every file whose chain runs out before its size."""
    here = os.path.dirname(os.path.abspath(__file__))
    src = open(os.path.join(here, 'fatls.py')).read().split("f = open(sys.argv[1], 'rb')")[0]
    mod = type(sys)('fatls'); exec(compile(src, 'fatls', 'exec'), mod.__dict__)
    out = []
    with open(img, 'rb') as f:
        f.seek(0); mbr = f.read(512)
        for p in range(4):
            e = mbr[0x1be + p * 16: 0x1be + (p + 1) * 16]
            if e[4] == 0: continue
            fat = mod.Fat(f, mod.u32(e, 8))
            top = len(fat.fat) // 2
            for depth, path, ent in fat.walk(limit=8):
                if ent['dir'] or not ent['size']: continue
                got, c, per = 0, ent['cluster'], fat.spc * fat.bps
                while 2 <= c < min(0xfff8, top) and got < ent['size']:
                    got += per; c = fat.next_cluster(c)
                if got < ent['size']:
                    out.append((f'p{p + 1}{path}', got, ent['size']))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('delta', help='a .delta file, or a directory to pick the newest from')
    ap.add_argument('out')
    ap.add_argument('--sectors', type=int, default=0, help='stop after this many (default: all)')
    ap.add_argument('--check', action='store_true', help='walk the result for truncated files')
    a = ap.parse_args()

    path = a.delta
    if os.path.isdir(path):
        cands = [f for f in glob.glob(os.path.join(path, '*.delta')) if delta_geometry(f)[1]]
        if not cands: sys.exit(f'no usable delta in {path}')
        path = max(cands, key=os.path.getmtime)
    off, n = delta_geometry(path)
    if off is None: sys.exit(f'{path}: not a delta')
    if a.sectors: n = min(n, a.sectors)
    with open(path, 'rb') as src, open(a.out, 'wb') as dst:
        src.seek(off)
        left = n * 512
        while left:
            chunk = src.read(min(1 << 22, left))
            if not chunk: break
            dst.write(chunk); left -= len(chunk)
    print(f'{a.out}: {n} sectors ({n * 512} bytes) from {os.path.basename(path)}')

    if a.check:
        bad = short_files(a.out)
        if not bad:
            print('check: every file\'s cluster chain covers its size')
        else:
            print(f'check: {len(bad)} truncated files — the guest had not flushed:')
            for p, got, want in bad[:12]:
                print(f'   {p}: {got} of {want}')


if __name__ == '__main__':
    main()
