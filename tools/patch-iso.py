#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
"""patch-iso.py <source.iso> <dest.iso> <path-in-iso> <replacement-file>

Copy an ISO 9660 image and overwrite one file's extent in place with the replacement (zero-padded;
the replacement must not be larger than the original).  The directory record is left alone, so the
consumer reads the original length — fine for a PE image, whose headers say how much matters.
Used to put a freshly built HAL where SETUPLDR expects the shipped one, without ISO authoring tools.
Nothing from the source ISO is redistributed by this script; it needs the user's own CD image."""
import shutil, struct, sys

def read_dir(f, extent, size):
    f.seek(extent * 2048)
    data = f.read(size)
    off = 0; entries = []
    while off < len(data):
        ln = data[off]
        if ln == 0:
            off = (off // 2048 + 1) * 2048
            continue
        rec = data[off:off + ln]
        ext = struct.unpack_from('<I', rec, 2)[0]
        sz = struct.unpack_from('<I', rec, 10)[0]
        flags = rec[25]
        nl = rec[32]
        name = rec[33:33 + nl].decode('latin1')
        if name == '\x00': name = '.'
        elif name == '\x01': name = '..'
        name = name.split(';')[0]
        entries.append((name, ext, sz, flags))
        off += ln
    return entries

def find(f, path):
    f.seek(16 * 2048)
    pvd = f.read(2048)
    assert pvd[1:6] == b'CD001', 'not an ISO 9660 image'
    root = pvd[156:190]
    ext = struct.unpack_from('<I', root, 2)[0]; sz = struct.unpack_from('<I', root, 10)[0]
    parts = [p for p in path.replace('\\', '/').split('/') if p]
    for i, part in enumerate(parts):
        entries = read_dir(f, ext, sz)
        match = [e for e in entries if e[0].upper() == part.upper()]
        if not match: sys.exit(f'patch-iso: {part} not found under {"/".join(parts[:i])}')
        _, ext, sz, flags = match[0]
        if i < len(parts) - 1 and not (flags & 2): sys.exit(f'patch-iso: {part} is not a directory')
    return ext, sz

def main():
    src, dst, path, repl = sys.argv[1:5]
    with open(src, 'rb') as f:
        ext, sz = find(f, path)
    data = open(repl, 'rb').read()
    if len(data) > sz: sys.exit(f'patch-iso: replacement is {len(data)} bytes, the original {sz}')
    if src != dst: shutil.copyfile(src, dst)
    with open(dst, 'r+b') as f:
        f.seek(ext * 2048)
        f.write(data + b'\0' * (sz - len(data)))
    print(f'{dst}: {path} at LBA {ext} ({sz} bytes) <- {repl} ({len(data)} bytes)')

if __name__ == '__main__':
    main()
