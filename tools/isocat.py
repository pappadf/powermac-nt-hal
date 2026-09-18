#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
"""isocat.py <image.iso> <\\PATH\\TO\\FILE> <out> — copy one file out of an ISO 9660 image.

The four files the boot floppy needs live on the user's own NT CD, and getting them off it was
the one step of the build with no tool behind it.  This is that tool.

Two things beyond a plain directory walk:

* **Version suffixes.**  ISO 9660 stores `VENEER.EXE;1`.  A name given without `;` matches any
  version, which is what every caller wants.

* **Compressed files.**  NT's distribution compresses most of what it ships and renames the last
  character of the extension to `_`: `CIRRUS.DLL` ships as `CIRRUS.DL_`.  On this CD those are
  **Microsoft Cabinet** files (`MSCF`), one file per cabinet, MSZIP — not the older SZDD that
  `expand.exe` is usually associated with.  MSZIP is raw deflate per block, each block prefixed
  `CK`, with the previous block's output carried in as the dictionary; that history is what makes
  it more than a loop of zlib calls.  Ask for `CIRRUS.DLL` and you get the decompressed file; ask
  for `CIRRUS.DL_` and you get the cabinet as it sits on the disc.
"""
import argparse, struct, sys, zlib

SECTOR = 2048


def _records(f, lba, length):
    """Every directory record in the extent at `lba`, as (name, lba, size, flags)."""
    f.seek(lba * SECTOR)
    data = f.read(length)
    out, i = [], 0
    while i < len(data):
        rl = data[i]
        if rl == 0:  # padding to the end of this sector; the next record starts at the next one
            i = (i // SECTOR + 1) * SECTOR
            if i >= len(data):
                break
            continue
        extent = struct.unpack('<I', data[i + 2:i + 6])[0]
        size = struct.unpack('<I', data[i + 10:i + 14])[0]
        flags = data[i + 25]
        nlen = data[i + 32]
        name = data[i + 33:i + 33 + nlen].decode('ascii', 'replace')
        out.append((name, extent, size, flags))
        i += rl
    return out


def _match(record_name, wanted):
    """ISO 9660 names carry a `;1` version suffix the caller does not write."""
    return record_name.upper().split(';')[0] == wanted.upper()


def find(f, path):
    """(lba, size) of `path` inside the image, or None.  Path separator is \\ or /."""
    f.seek(16 * SECTOR)
    pvd = f.read(SECTOR)
    if pvd[1:6] != b'CD001':
        raise SystemExit('not an ISO 9660 image (no CD001 at sector 16)')
    root = pvd[156:190]
    lba = struct.unpack('<I', root[2:6])[0]
    size = struct.unpack('<I', root[10:14])[0]
    parts = [p for p in path.replace('/', '\\').split('\\') if p]
    for depth, part in enumerate(parts):
        hit = next((r for r in _records(f, lba, size) if _match(r[0], part)), None)
        if not hit:
            return None
        _, lba, size, flags = hit
        is_dir = bool(flags & 0x02)
        if depth < len(parts) - 1 and not is_dir:
            return None
        if depth == len(parts) - 1:
            return None if is_dir else (lba, size)
    return None


def cab_extract(blob):
    """The single file out of a one-file MSZIP cabinet."""
    if blob[:4] != b'MSCF':
        raise SystemExit('not a Microsoft Cabinet')
    coff_files = struct.unpack('<I', blob[16:20])[0]
    n_folders, n_files, flags = struct.unpack('<HHH', blob[26:32])
    if flags:
        raise SystemExit(f'cabinet uses header extensions (flags {flags:#x}) this reader does not')
    if n_folders != 1 or n_files != 1:
        raise SystemExit(f'expected one folder and one file, found {n_folders} and {n_files}')
    data_start, n_blocks, compress = struct.unpack('<IHH', blob[36:44])
    if compress & 0x0F != 1:
        raise SystemExit(f'cabinet compression {compress & 0x0F} is not MSZIP')
    want = struct.unpack('<I', blob[coff_files:coff_files + 4])[0]

    out, history, off = bytearray(), b'', data_start
    for _ in range(n_blocks):
        c_bytes, u_bytes = struct.unpack('<HH', blob[off + 4:off + 8])
        payload = blob[off + 8:off + 8 + c_bytes]
        off += 8 + c_bytes
        if payload[:2] != b'CK':
            raise SystemExit("MSZIP block is missing its 'CK' signature")
        # Raw deflate, with the previous block's output as the preset dictionary: MSZIP keeps
        # one 32 KB history across the whole folder rather than restarting per block.
        d = zlib.decompressobj(-15, zdict=history) if history else zlib.decompressobj(-15)
        chunk = d.decompress(payload[2:]) + d.flush()
        if len(chunk) != u_bytes:
            raise SystemExit(f'MSZIP block gave {len(chunk)} bytes, header said {u_bytes}')
        out += chunk
        history = bytes(out[-32768:])
    if len(out) != want:
        raise SystemExit(f'cabinet holds {len(out)} bytes, its file record says {want}')
    return bytes(out)


def read(iso_path, member):
    """`member` out of `iso_path`, decompressed if it is a cabinet under a `_` name."""
    f = open(iso_path, 'rb')
    hit = find(f, member)
    packed = False
    if hit is None:
        # CIRRUS.DLL is not there under that name; CIRRUS.DL_ is.
        head, _, tail = member.rpartition('.')
        if tail and len(tail) == 3:
            hit = find(f, f'{head}.{tail[:2]}_')
            packed = hit is not None
    if hit is None:
        raise SystemExit(f'{member}: not found in {iso_path}')
    lba, size = hit
    f.seek(lba * SECTOR)
    blob = f.read(size)
    return cab_extract(blob) if (packed or blob[:4] == b'MSCF') else blob


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('iso')
    ap.add_argument('member', help=r'path inside the image, e.g. \PPC\VENEER.EXE')
    ap.add_argument('out')
    a = ap.parse_args()
    blob = read(a.iso, a.member)
    with open(a.out, 'wb') as g:
        g.write(blob)
    print(f'{a.out}: {len(blob)} bytes from {a.member}')


if __name__ == '__main__':
    main()
