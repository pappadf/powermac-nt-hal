#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
"""run-boot.py <script.gs> <installed-disk.img> [--log out.log] — run a boot script from
`mkbootscript.py` against an installed disk image.

The one thing this does that the script cannot: a checkpoint's disk is a copy-on-write delta
created fresh by `checkpoint.load`, and edits to the base image are never read. So load the
checkpoint first, find the delta that has just appeared with the right geometry, splice the
installed image into it, and only then run the rest of the script. Ledger row 14.

Only non-empty sectors are written, which is why the disk under the checkpoint must be blank (or
the same disk) — zero sectors keep whatever the base had.
"""
import argparse, glob, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
GSH = os.path.join(HERE, 'gsh.py')


def delta_geometry(path):
    """A delta is a 24-byte header, a two-bits-per-sector map, then the sectors themselves."""
    size = os.path.getsize(path)
    n = (size - 24) * 8 // (512 * 8 + 2)
    while 24 + 2 * ((n + 7) // 8) + n * 512 < size:
        n += 1
    if 24 + 2 * ((n + 7) // 8) + n * 512 != size:
        return None, None
    return 24 + 2 * ((n + 7) // 8), n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('script')
    ap.add_argument('disk')
    ap.add_argument('--log', default='')
    ap.add_argument('--delta-dir', default='tmp/ckpt-daemon')
    a = ap.parse_args()

    log = open(a.log, 'w') if a.log else sys.stdout
    env = dict(os.environ, GS_TIMEOUT='5400', GS_IDLE='5400')
    run = lambda cmd: subprocess.run([sys.executable, GSH, cmd], stdout=log,
                                     stderr=subprocess.STDOUT, env=env)

    lines = open(a.script).read().split('\n')
    i = next(n for n, l in enumerate(lines) if l.startswith('checkpoint.load'))
    run(lines[i])                                    # load first, so the delta exists

    want = os.path.getsize(a.disk) // 512
    cands = [f for f in glob.glob(os.path.join(a.delta_dir, '*.delta'))
             if delta_geometry(f)[1] == want]
    if not cands:
        sys.exit(f'no {want}-sector delta in {a.delta_dir}: is the disk attached and writable?')
    dp = max(cands, key=os.path.getmtime)
    off, n = delta_geometry(dp)
    img = open(a.disk, 'rb').read()
    written = 0
    with open(dp, 'r+b') as df:
        for lba in range(min(n, len(img) // 512)):
            blk = img[lba * 512:(lba + 1) * 512]
            if not any(blk):
                continue
            df.seek(off + lba * 512)
            df.write(blk)
            written += 1
    print(f'=== spliced {a.disk} into {dp}: {written} non-empty sectors ===', file=log)
    log.flush()

    rest = os.path.join(os.path.dirname(a.script) or '.', '.rest-' + os.path.basename(a.script))
    open(rest, 'w').write('\n'.join(lines[:i] + lines[i + 1:]))
    run(f'include "{rest}"')
    if a.log:
        log.close()


if __name__ == '__main__':
    main()
