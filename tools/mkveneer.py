#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
"""mkveneer.py <VENEER.EXE> --out <patched.exe> — bake this project's veneer patches into the
image, instead of poking them into memory after Open Firmware has loaded it.

Ledger rows 1-5 are all one- or two-word edits to Microsoft's ARC shim, and every one of them is
currently typed into Open Firmware (or poked by the test rig) *after* the veneer is in RAM.  That
is fine for a development loop and useless for anybody else: `CHARTER.md` §3.1 says so outright —
"a published project needs a better answer than 'type these into Open Firmware'".

Every one of those pokes is at an image virtual address, and the veneer is a raw COFF with base
0x50000, so each maps to a file offset.  Applied here they survive being loaded, which means a
cold boot needs only the four legitimate firmware steps: little-endian reboot, `pe-loader` read,
set `/chosen bootpath`, `go`.

This does **not** make the patches defensible -- they still patch a proprietary binary, and they
stay in the ledger.  It moves them from "instructions a human follows" to "bytes in an image",
which is the difference between a story and a deliverable.

    mkveneer.py PPC/VENEER.EXE --out tmp/veneer-cd.exe --for cd
    mkveneer.py PPC/VENEER.EXE --out tmp/veneer-disk.exe --for disk --stage tmp/staging.img

Nothing from Microsoft is stored here: the patch table is addresses and replacement words, and
the image comes from the user's own CD.
"""
import argparse, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from coffsyms import Coff

NOP = 0x60000000

# (va, expected shipped word, replacement, ledger row, why).  `expected` is what refuses to patch
# a veneer that is not the one these addresses were derived from -- the same check the eventual
# ISO tool needs before it touches a stranger's CD.
WORD_PATCHES = [
    (0x514E0, 0x48000605, NOP,        1, "claim SYSTEM PARAMETER BLOCK: skip the failure path"),
    (0x51E3C, 0x4BFFFCA9, NOP,        1, "claim RESTART BLOCK: likewise"),
    (0x52254, 0x91430008, 0x39200000, 2, "OFClose nret: li r9,0 ..."),
    (0x52258, 0x39200000, 0x91230008, 2, "   ... stw r9,8(r3), so closes stop leaking ihandles"),
]
# Row 4 is CD-only (wall 46): Apple's OF answers ':N' on an ISO with the root directory *as a
# file*, so the CD needs VrOpen's whole-device route -- and an MBR disk needs the shipped branch,
# because ':N' is the only thing that gives NT partition-relative sectors.
VROPEN = (0x54748, 0x4086003C, NOP, 4, "VrOpen: take the raw/whole-device route (CD only)")
# The veneer ships aimed at an installed system -- its boot-file buffer holds
# '\\os\\winnt\\osloader.exe'.  Booting the CD means aiming it at SETUPLDR instead, and one
# instruction at 0x53db0 that has to stop deriving a path from what was there.  Both are CD-only
# for the same reason row 4 is: a disk boot wants the shipped behaviour back (wall 47).
SETUPLDR_PATH = (0x5CD30, b'\\os\\winnt\\osloader.exe', b'\\PPC\\SETUPLDR' + b'\0' * 10, 4,
                 "the boot file the veneer opens on the CD")
SETUPLDR_INSN = (0x53DB0, 0x554AA016, 0x39400000, 4, "li r10,0 in the path derivation")

BYTE_PATCHES = [
    (0x5D0C0, ord('p'), 0, 3, "blank the 'partition(1)' the veneer appends to every ARC path"),
    (0x5E168, ord(':'), 0, 3, "blank the ':0' appended to the Open Firmware argument"),
]
SCSI_MODEL = (0x5F420, b'NCR,53C810\0', b'NCR,825A\0\0\0', 5,
              "the SCSI identifier Setup's mass-storage detection matches to symc810.sys")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('veneer')
    ap.add_argument('--out', required=True)
    ap.add_argument('--for', dest='target', choices=('cd', 'disk'), default='cd',
                    help="'cd' adds the VrOpen patch (ledger row 4); 'disk' leaves the shipped "
                         "assembly alone, because that patch is wrong for an MBR disk (wall 46)")
    ap.add_argument('--stage', metavar='IMG[@BLOCK]',
                    help='also write the patched image into a raw disk at BLOCK (default 0x800), '
                         "which is where the ROM's pe-loader is told to read it from")
    a = ap.parse_args()

    ven = Coff(a.veneer)
    if ven.base != 0x50000:
        sys.exit(f'{a.veneer}: image base {ven.base:#x}, expected 0x50000 — not this veneer?')
    d = bytearray(ven.d)

    patches = list(WORD_PATCHES) + ([VROPEN, SETUPLDR_INSN] if a.target == 'cd' else [])
    print(f'{a.veneer}: {len(d)} bytes, base {ven.base:#x}, target {a.target}')
    for va, want, new, row, why in patches:
        off = ven.va2off(va)
        if off is None:
            sys.exit(f'{va:#x} is not in this image')
        got = struct.unpack_from('<I', d, off)[0]
        if got != want:
            sys.exit(f'{va:#x} (file {off:#x}) holds {got:#010x}, expected {want:#010x} — this is '
                     f'not the veneer these patches were derived from; refusing to patch')
        struct.pack_into('<I', d, off, new)
        print(f'  row {row}  VA {va:#07x}  {want:#010x} -> {new:#010x}   {why}')

    for va, want, new, row, why in BYTE_PATCHES:
        off = ven.va2off(va)
        if d[off] != want:
            sys.exit(f'{va:#x} holds {d[off]:#04x}, expected {want:#04x} — refusing to patch')
        d[off] = new
        print(f'  row {row}  VA {va:#07x}  {want:#04x} -> {new:#04x}         {why}')

    if a.target == 'cd':
        va, want, new, row, why = SETUPLDR_PATH
        off = ven.va2off(va)
        if bytes(d[off:off + len(want)]) != want:
            sys.exit(f'{va:#x} holds {bytes(d[off:off+len(want)])!r}, expected {want!r} — refusing')
        d[off:off + len(new)] = new
        print(f'  row {row}  VA {va:#07x}  {want!r} -> {new!r}   {why}')

    va, want, new, row, why = SCSI_MODEL
    off = ven.va2off(va)
    if bytes(d[off:off + len(want)]) != want:
        sys.exit(f'{va:#x} holds {bytes(d[off:off+len(want)])!r}, expected {want!r} — refusing')
    d[off:off + len(new)] = new
    print(f'  row {row}  VA {va:#07x}  {want!r} -> {new!r}   {why}')

    open(a.out, 'wb').write(bytes(d))
    print(f'{a.out}: written')

    if a.stage:
        img, _, blk = a.stage.partition('@')
        block = int(blk, 0) if blk else 0x800
        with open(img, 'r+b') as f:
            f.seek(block * 512)
            f.write(bytes(d))
        print(f'{img}: patched veneer written at block {block:#x} ({len(d)} bytes)')


if __name__ == '__main__':
    main()
