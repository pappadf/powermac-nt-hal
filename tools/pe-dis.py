#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
"""pe-dis.py <pe> [start [end]]  — disassemble a PowerPC PE image (NT4 HAL/driver) with
import-thunk and string annotations. Addresses are image virtual addresses in hex;
default = all of .text.  TOC (r2) is taken from the first `lwz r2,d(r2)`-free heuristic:
the .data word that the entry's descriptor names, else set TOC=hex in the environment."""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ppcdis import dis
d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
machine, nsec, _, _, _, optsz, _ = struct.unpack_from('<HHIIIHH', d, pe + 4)
opt = pe + 24
base = struct.unpack_from('<I', d, opt + 28)[0]
entry = base + struct.unpack_from('<I', d, opt + 16)[0]
exp_rva = struct.unpack_from('<I', d, opt + 96)[0]
imp_rva = struct.unpack_from('<I', d, opt + 104)[0]
secs = []
so = opt + optsz
for i in range(nsec):
    name = d[so+40*i:so+40*i+8].rstrip(b'\0').decode()
    vs, va, rs, rp = struct.unpack_from('<IIII', d, so + 40*i + 8)
    secs.append((name, base + va, vs, rp, rs))
def img(addr, n):
    for name, va, vs, rp, rs in secs:
        if va <= addr < va + max(vs, rs):
            o = addr - va
            return (d[rp+o:rp+min(o+n, rs)] + b'\0'*n)[:n]
    return b'\0'*n
def cstr(a, limit=64):
    b = img(a, limit); e = b.find(b'\0')
    if e <= 0: return None
    s = b[:e]
    if any(c < 9 or c > 126 for c in s): return None
    return s.decode('latin1')
syms = {}
text = [s for s in secs if s[0] == '.text'][0]
# imports: IAT slot address -> name
if imp_rva:
    o = imp_rva
    while True:
        ilt, ts, fc, nm, iat = struct.unpack_from('<IIIII', img(base + o, 20), 0)
        if not nm: break
        dll = cstr(base + nm)
        k = 0
        while True:
            hint = struct.unpack_from('<I', img(base + (ilt or iat) + 4*k, 4), 0)[0]
            if not hint: break
            n = cstr(base + hint + 2) if not (hint & 0x80000000) else f'ord{hint & 0xffff}'
            syms[base + iat + 4*k] = f'{dll}!{n}'
            k += 1
        o += 20
if exp_rva:
    e = img(base + exp_rva, 40)
    _, _, _, _, ordbase, nfunc, nnames, addr_rva, names_rva, ord_rva = struct.unpack_from('<IIIIIIIIII', e, 0)
    for i in range(nnames):
        nr = struct.unpack_from('<I', img(base + names_rva + 4*i, 4), 0)[0]
        oi = struct.unpack_from('<H', img(base + ord_rva + 2*i, 2), 0)[0]
        fr = struct.unpack_from('<I', img(base + addr_rva + 4*oi, 4), 0)[0]
        # exports point at function descriptors {entry, toc} in .data on PPC NT
        desc = base + fr
        ent, toc = struct.unpack_from('<II', img(desc, 8), 0)
        syms.setdefault(desc, f'desc:{cstr(base+nr)}')
        if ent: syms.setdefault(ent, cstr(base + nr))
start = int(sys.argv[2], 16) if len(sys.argv) > 2 else text[1]
end = int(sys.argv[3], 16) if len(sys.argv) > 3 else text[1] + text[2]
# TOC: the entry point's descriptor is the export/entry; try the word after the first
# `lwz r2` pattern; else environment
toc = int(os.environ['TOC'], 16) if os.environ.get('TOC') else None
if toc is None:
    # scan .data for a descriptor {entry, toc} whose entry == entry point
    dsec = [s for s in secs if s[0] == '.data'][0]
    for a in range(dsec[1], dsec[1] + dsec[2] - 8, 4):
        e1, t1 = struct.unpack_from('<II', img(a, 8), 0)
        if e1 == entry: toc = t1; break
if toc is None:
    # fallback: the most common second word among {text-address, data-address} pairs in .data
    from collections import Counter
    dsec = [s for s in secs if s[0] == '.data'][0]
    tsec = text if 'text' in dir() else [s for s in secs if s[0] == '.text'][0]
    c = Counter()
    for a in range(dsec[1], dsec[1] + dsec[2] - 8, 4):
        e1, t1 = struct.unpack_from('<II', img(a, 8), 0)
        if tsec[1] <= e1 < tsec[1] + tsec[2] and dsec[1] <= t1 < dsec[1] + dsec[2]:
            c[t1] += 1
    toc = c.most_common(1)[0][0] if c else 0
print(f'# base {base:08x} entry {entry:08x} toc {toc:08x} sections {[ (s[0], hex(s[1]), hex(s[2])) for s in secs]}')
def note(insn):
    op = insn >> 26; ra = (insn >> 16) & 31; rd = (insn >> 21) & 31
    if ra == 2 and op in (32, 14) and toc:
        dd = insn & 0xffff
        if dd & 0x8000: dd -= 0x10000
        ea = toc + dd
        val = struct.unpack_from('<I', img(ea, 4), 0)[0]
        parts = [f'[toc{dd:+d}]={val:08x}']
        if ea in syms: parts.append(syms[ea])
        if val in syms: parts.append('-> ' + syms[val])
        s = cstr(val)
        if s: parts.append(repr(s))
        return '  ; ' + ' '.join(parts)
    return ''
a = start
while a < end:
    insn = struct.unpack_from('<I', img(a, 4), 0)[0]
    if a in syms: print(f'\n{syms[a]}:')
    txt = dis(insn, a)
    n = note(insn)
    op = insn >> 26
    if op in (18, 16):
        li = insn & (0x03FFFFFC if op == 18 else 0xFFFC)
        if op == 18 and li & 0x02000000: li -= 0x04000000
        if op == 16 and li & 0x8000: li -= 0x10000
        tgt = li if insn & 2 else a + li
        if tgt in syms: n += f'  ; -> {syms[tgt]}'
    print(f'{a:08x}  {insn:08x}  {txt}{n}')
    a += 4
