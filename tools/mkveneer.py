#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
"""mkveneer.py <VENEER.EXE> --out <patched.exe> — bake this project's veneer patches into the
image, instead of poking them into memory after Open Firmware has loaded it.

Ledger rows 1-5 and 17 are all one-, two- or few-byte edits to Microsoft's ARC shim, and every
one of them was typed into Open Firmware (or poked by the test rig) *after* the veneer was in
RAM.  That is fine for a development loop and useless for anybody else: `CHARTER.md` §3.1 says
so outright —
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

# Row 3 is CD-only too: an MBR disk needs 'partition(N)' on its ARC paths and ':N' on the Open
# Firmware argument -- they are what give NT partition-relative sectors (wall 46).  mkbootscript.py
# undoes both for a disk boot; a `--for disk` veneer simply never gets them.
BYTE_PATCHES = [
    (0x5D0C0, ord('p'), 0, 3, "blank the 'partition(1)' the veneer appends to every ARC path"),
    (0x5E168, ord(':'), 0, 3, "blank the ':0' appended to the Open Firmware argument"),
]
# Wall 47 -- the veneer's shipped boot-file buffer holding '\\os\\winnt\\osloader.exe' -- needs no
# patch for a disk: read_ARC_env_vars overwrites every argv slot with the NVRAM variable of the
# same name when one exists, and setup.of stores OSLOADER there (mkbootfloppy.py).  The old rig
# had no NVRAM environment, which is the only reason it had to relocate the path.
SCSI_MODEL = (0x5F420, b'NCR,53C810\0', b'NCR,825A\0\0\0', 5,
              "the SCSI identifier Setup's mass-storage detection matches to symc810.sys")
# Row 17 is the floppy, and it is one string.  `convert_name` classifies a node by its Open
# Firmware `device_type` and `name`: device_type `block` gives ControllerClass, and then the name
# decides -- `disk` and `floppy` both give DiskController, `cdrom` gives CdromController,
# anything else gives OtherController.  Apple's node is `device_type block`, `name swim3`, so it
# falls through to *other*, `convert_controller` hangs an OtherPeripheral off it, and the ARC
# path comes out `multi(0)other(0)other(0)` instead of the `multi(0)disk(0)fdisk(%d)` SETUPLDR
# spells floppies with.  The same string is read at all three places that matter -- the
# classification, the child `convert_controller` adds (`fdisk`), and the `convert_config` special
# case that calls `convert_config_floppy` -- so renaming it to the name this machine's firmware
# actually uses turns all three on at once.  Nothing on an ANS is called `floppy`, so nothing is
# lost by the rename.  ANS-only, like every veneer patch: see the plan's section 7.
FLOPPY_NAME = (0x5F398, b'floppy\0', b'swim3\0\0', 17,
               "the OBP node name the veneer recognises as a floppy drive")


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

    for va, want, new, row, why in (BYTE_PATCHES if a.target == 'cd' else []):
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

    for va, want, new, row, why in (SCSI_MODEL, FLOPPY_NAME):
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
