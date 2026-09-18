#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
"""savedisk.py <delta|dir> <out.img> [--base disk.img] [--check] — write out the disk a run left
behind, from the emulator's copy-on-write delta.

Every writable image is opened read-only as the base and the guest writes into a delta, so the
base image on disk is never touched: after text-mode Setup the *delta* is the installed disk.
This is the inverse of `run-boot.py`'s splice.

The delta (`GSDL`, version 1): a 24-byte header (magic, version, 64-bit block count, block
size), a current bitmap and a committed bitmap of one bit per block, then a sparse data area of
block_count * block_size bytes, written in place and cut off after the last block written.  The
bitmaps are flushed only at checkpoint time, so a run that never checkpointed leaves them all
zero; then a block counts as written when it is not all zeros, which is right for everything but
a block the guest deliberately zeroed over non-zero base content -- say so with a warning.  With
`--base`, unwritten blocks come from that image (the disk the run was started with); without it
they are zeros, which is what the old blank-disk runs had anyway.

**Capture after the guest has flushed.** Wall 48: an image taken while Setup still had writes
outstanding had 49 files whose cluster chain was shorter than their directory entry claimed --
the FAT caught mid-update.  Let Setup reach its restart prompt, press Enter, and give NT time to
shut down before saving.  `--check` walks the result and says whether any file is short.
"""
import argparse, glob, os, struct, sys


def delta_geometry(path):
    """(data offset, block count, block size, current bitmap) of a GSDL delta, or Nones."""
    with open(path, 'rb') as f:
        h = f.read(24)
        if len(h) < 24 or h[:4] != b'GSDL': return None, None, None, None
        version, count, bs = struct.unpack('<IQI', h[4:20])
        if version != 1 or bs not in (512, 2048) or not count: return None, None, None, None
        bm = (count + 7) // 8
        cur = f.read(bm)
    return 24 + 2 * bm, count, bs, cur


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
    ap.add_argument('--base', help='the image the run was started with; unwritten blocks come from it')
    ap.add_argument('--check', action='store_true', help='walk the result for truncated files')
    a = ap.parse_args()

    path = a.delta
    if os.path.isdir(path):
        cands = [f for f in glob.glob(os.path.join(path, '*.delta')) if delta_geometry(f)[1]]
        if not cands: sys.exit(f'no usable delta in {path}')
        path = max(cands, key=os.path.getmtime)
    off, n, bs, bitmap = delta_geometry(path)
    if off is None: sys.exit(f'{path}: not a GSDL delta')
    mapped = sum(bin(b).count('1') for b in bitmap)
    if not mapped:
        print('warning: the delta\'s bitmap is empty (no checkpoint was taken) -- taking every '
              'non-zero block of the data area as written')
    if a.base and os.path.getsize(a.base) != n * bs:
        sys.exit(f'{a.base}: {os.path.getsize(a.base)} bytes, the delta describes {n * bs}')

    taken = 0
    with open(path, 'rb') as src, open(a.out, 'wb') as dst:
        base = open(a.base, 'rb') if a.base else None
        for lba in range(0, n, 4096):
            m = min(4096, n - lba)
            src.seek(off + lba * bs); chunk = src.read(m * bs)
            chunk += bytes(m * bs - len(chunk))              # past EOF: never written
            out = bytearray(base.read(m * bs) if base else bytes(m * bs))
            out += bytes(m * bs - len(out))
            for i in range(m):
                blk = chunk[i * bs:(i + 1) * bs]
                written = (bitmap[(lba + i) >> 3] >> ((lba + i) & 7)) & 1 if mapped else any(blk)
                if written:
                    out[i * bs:(i + 1) * bs] = blk; taken += 1
            dst.write(out)
    print(f'{a.out}: {n} blocks of {bs}, {taken} from {os.path.basename(path)}'
          + (f', the rest from {a.base}' if a.base else ', the rest zero'))

    if a.check:
        bad = short_files(a.out)
        if not bad:
            print('check: every file\'s cluster chain covers its size')
        else:
            print(f'check: {len(bad)} truncated files -- the guest had not flushed:')
            for p, got, want in bad[:12]:
                print(f'   {p}: {got} of {want}')


if __name__ == '__main__':
    main()
