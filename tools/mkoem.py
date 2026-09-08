#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
"""mkoem.py — give this HAL its own entry in NT's Setup, instead of impersonating another.

Until now the HAL was delivered by overwriting `\\PPC\\HALEAGLE.DLL`, the HAL for MOTOROLA's
PowerStack, and selected by picking *MOTOROLA PowerStack* at Setup's computer-type menu.  That
works for booting but breaks as soon as Setup tries to *install*: it copies the whole
209,440-byte extent the CD's directory record claims for HALEAGLE.DLL, then checksums it, and
our 42 KB image plus 167 KB of Microsoft's leftovers is not a valid PE.

This produces the two patches that give the HAL a name and a menu entry of its own:

  * TXTSETUP.SIF — the `bigbend_up` machine (MOTOROLA Big Bend, which also used haleagle.dll)
    becomes `shiner_up`, described as "Apple Network Server 500/700", loading and installing
    `halshinr.dll`.  The substitutions net to zero bytes — the longer menu description is paid
    for by trimming alignment spaces elsewhere — so the file stays exactly the size its ISO
    directory record claims, and no ISO rebuild is needed.
  * The `\\PPC` directory block — HALEAGLE.DLL's record is renamed in place to HALSHINR.DLL
    (both are 12 bytes, so no record moves) and its length field is set to the real size of our
    HAL, which is what makes Setup's post-copy checksum agree.

Both are written as files to `--out-dir`, with the LBA to place each one at, ready for
`run-hal.py --delta-patch <file>@<lba>`.

    mkoem.py <cd.iso> <hal.dll> [--out-dir tmp] [--sif <patched-sif-to-start-from>]
"""
import argparse, os, struct, sys

# Every pair is length-neutral: see the module docstring.  The keys are the machine identifiers
# TXTSETUP.SIF uses internally; only the quoted description is ever shown to the user.
# The description is six bytes longer than what it replaces; the [Map.Computer] line gives those
# six back by losing alignment spaces, so the file's length is unchanged.
SIF_SUBS = [
    ('bigbend_up     = "MOTOROLA Big Bend",files.none',
     'shiner_up = "Apple Network Server 500/700",files.none'),
    ('bigbend_up      = haleagle.dll',
     'shiner_up       = halshinr.dll'),
    ('bigbend_up      = haleagle.dll, ,hal.dll',
     'shiner_up       = halshinr.dll, ,hal.dll'),
    ('haleagle.dll = 1,,,,,,_x,1,3',
     'halshinr.dll = 1,,,,,,_x,1,3'),
    ('bigbend_up      = "MOTOROLA-Big Bend"',
     'shiner_up = "MOTOROLA-Big Bend"'),
]
OLD_NAME, NEW_NAME = b'HALEAGLE.DLL', b'HALSHINR.DLL'


def find_dir(iso, path_name):
    """(lba, length) of a directory in the root, by name."""
    iso.seek(16 * 2048)
    pvd = iso.read(2048)
    lba, length = struct.unpack_from('<I', pvd, 158)[0], struct.unpack_from('<I', pvd, 166)[0]
    for b in range(length // 2048 + 1):
        iso.seek((lba + b) * 2048)
        blk = iso.read(2048)
        i = 0
        while i < len(blk) and blk[i]:
            rec = blk[i:i + blk[i]]
            n = rec[32]
            if rec[33:33 + n].upper().startswith(path_name):
                return struct.unpack_from('<I', rec, 2)[0], struct.unpack_from('<I', rec, 10)[0]
            i += blk[i]
    sys.exit(f'mkoem: no {path_name.decode()} directory in the root')


def find_file_record(iso, dir_lba, dir_len, name):
    """(lba of the block, offset of the record within it) for one file's directory record."""
    for b in range(dir_len // 2048):
        iso.seek((dir_lba + b) * 2048)
        blk = iso.read(2048)
        i = 0
        while i < len(blk) and blk[i]:
            rec = blk[i:i + blk[i]]
            if rec[33:33 + rec[32]] == name:
                return dir_lba + b, i
            i += blk[i]
    sys.exit(f'mkoem: no {name.decode()} in the directory')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('iso')
    ap.add_argument('hal')
    ap.add_argument('--out-dir', default='tmp')
    ap.add_argument('--sif', help='start from this TXTSETUP.SIF instead of the one in the ISO '
                                  '(use the copy that already carries other patches)')
    a = ap.parse_args()

    iso = open(a.iso, 'rb')
    ppc_lba, ppc_len = find_dir(iso, b'PPC')
    hal_size = os.path.getsize(a.hal)

    # ---- the \PPC directory block, with the record renamed and resized ----
    blk_lba, off = find_file_record(iso, ppc_lba, ppc_len, OLD_NAME)
    iso.seek(blk_lba * 2048)
    block = bytearray(iso.read(2048))
    was = struct.unpack_from('<I', block, off + 10)[0]
    block[off + 33:off + 33 + len(OLD_NAME)] = NEW_NAME
    struct.pack_into('<I', block, off + 10, hal_size)
    dir_out = os.path.join(a.out_dir, 'oem-ppc-dir.bin')
    open(dir_out, 'wb').write(block)

    # ---- TXTSETUP.SIF ----
    if a.sif:
        sif = open(a.sif, 'rb').read()
        sif_lba, sif_len = find_file_record(iso, ppc_lba, ppc_len, b'TXTSETUP.SIF')
        iso.seek(sif_lba * 2048)
        rec = iso.read(2048)
        claimed = struct.unpack_from('<I', rec, sif_len + 10)[0]
        sif_extent = struct.unpack_from('<I', rec, sif_len + 2)[0]
    else:
        blk, off2 = find_file_record(iso, ppc_lba, ppc_len, b'TXTSETUP.SIF')
        iso.seek(blk * 2048)
        rec = iso.read(2048)
        sif_extent = struct.unpack_from('<I', rec, off2 + 2)[0]
        claimed = struct.unpack_from('<I', rec, off2 + 10)[0]
        iso.seek(sif_extent * 2048)
        sif = iso.read(claimed)

    text = sif.decode('latin1')
    # Individual substitutions may change length — the longer menu description is paid for by
    # trimming alignment spaces on the [Map.Computer] line — but the *total* must be zero,
    # because the file has to stay the size its ISO directory record claims.
    net = 0
    # Longest pattern first: [hal] appears before [Hal.Load] in the file, and the [Hal.Load]
    # pattern is a prefix of the [hal] line, so a naive order rewrites the wrong one.
    for old, new in sorted(SIF_SUBS, key=lambda p: -len(p[0])):
        if old not in text:
            sys.exit(f'mkoem: TXTSETUP.SIF does not contain:\n  {old}')
        net += len(new) - len(old)
        text = text.replace(old, new, 1)
    if net != 0:
        sys.exit(f'mkoem: the substitutions change TXTSETUP.SIF by {net:+d} bytes; they must net to zero')
    patched = text.encode('latin1')
    if len(patched) != len(sif):
        sys.exit(f'mkoem: SIF changed size {len(sif)} -> {len(patched)}')
    if len(patched) > claimed:
        sys.exit(f'mkoem: SIF is {len(patched)} bytes but its record claims {claimed}')
    sif_out = os.path.join(a.out_dir, 'oem-txtsetup.sif')
    open(sif_out, 'wb').write(patched)

    print(f'{dir_out}  -> LBA {blk_lba}   (\\PPC directory block: {OLD_NAME.decode()} renamed to '
          f'{NEW_NAME.decode()}, length {was} -> {hal_size})')
    print(f'{sif_out}  -> LBA {sif_extent}   (TXTSETUP.SIF, {len(patched)} bytes, unchanged size; '
          f'menu entry "Apple Network Server 500/700" loading halshinr.dll)')
    print()
    print('Then, from the Granny Smith checkout:')
    print(f'  run-hal.py ... --delta-patch {a.hal} --delta-patch {dir_out}@{blk_lba} '
          f'--delta-patch {sif_out}@{sif_extent}')


main()
