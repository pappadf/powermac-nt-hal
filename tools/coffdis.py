#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
"""coffdis.py <image> <start-va> <end-va> — disassemble a raw NT PowerPC COFF image, labelling
functions and branch targets from its COFF symbol table.

`pe-dis.py`'s counterpart for the two symbol-bearing COFF images this project reads:
`VENEER.EXE` and `OSLOADER.EXE`.  Every finding in
`docs/2026-09-07-booting-the-installed-disk.md` — which argument `BlOsLoader` looks up, where
`BlLoadSystemHive` gives up, how `VrOpen` turns an ARC `partition(N)` into an Open Firmware
`:N` — came out of reading those two with this.
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ppcdis import dis
from coffsyms import Coff

c = Coff(sys.argv[1])
start, end = int(sys.argv[2], 16), int(sys.argv[3], 16)
byva = {}
for a, n, s in c.funcs():
    # '..name' is the code, 'name' the function descriptor; section symbols ('.text') are noise
    if n.startswith('..') or not n.startswith('.'): byva.setdefault(a, n)
for va in range(start, end, 4):
    off = c.va2off(va)
    if off is None: continue
    w = struct.unpack_from('<I', c.d, off)[0]
    lbl = byva.get(va)
    if lbl: print(f'\n{va:08x} <{lbl}>:')
    ann = ''
    if (w >> 26) == 18:                       # b/bl: name the target if we know it
        li = w & 0x03fffffc
        if li & 0x02000000: li -= 0x04000000
        tgt = (0 if w & 2 else va) + li
        if tgt in byva: ann = f'   ; {byva[tgt]}'
    print(f'  {va:08x}  {w:08x}  {dis(w, va)}{ann}')
