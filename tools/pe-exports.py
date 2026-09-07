#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
"""pe-exports.py <pe file>: print machine, sections and the export table of a PE image."""
import struct, sys
d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
assert d[pe:pe+4] == b'PE\0\0', 'no PE signature'
machine, nsec, _, _, _, optsz, chars = struct.unpack_from('<HHIIIHH', d, pe + 4)
opt = pe + 24
magic = struct.unpack_from('<H', d, opt)[0]
imagebase = struct.unpack_from('<I', d, opt + 28)[0]
entry = struct.unpack_from('<I', d, opt + 16)[0]
ndd = struct.unpack_from('<I', d, opt + 92)[0]
exp_rva, exp_sz = struct.unpack_from('<II', d, opt + 96)
imp_rva, imp_sz = struct.unpack_from('<II', d, opt + 104)
secs = []
so = opt + optsz
for i in range(nsec):
    name = d[so+40*i:so+40*i+8].rstrip(b'\0').decode()
    vs, va, rs, rp = struct.unpack_from('<IIII', d, so + 40*i + 8)
    secs.append((name, va, vs, rp, rs))
def rva2off(r):
    for n, va, vs, rp, rs in secs:
        if va <= r < va + max(vs, rs): return rp + (r - va)
    raise ValueError(hex(r))
def cstr(r):
    o = rva2off(r); e = d.index(b'\0', o); return d[o:e].decode('latin1')
print(f'machine 0x{machine:04x} base 0x{imagebase:08x} entry 0x{entry:08x} sections {[s[0] for s in secs]}')
if exp_rva:
    o = rva2off(exp_rva)
    _, _, _, name_rva, base, nfunc, nnames, addr_rva, names_rva, ord_rva = struct.unpack_from('<IIIIIIIIII', d, o)
    print(f'export name: {cstr(name_rva)}  functions {nfunc} names {nnames}')
    names = {}
    for i in range(nnames):
        nr = struct.unpack_from('<I', d, rva2off(names_rva) + 4*i)[0]
        oi = struct.unpack_from('<H', d, rva2off(ord_rva) + 2*i)[0]
        names[oi] = cstr(nr)
    for i in range(nfunc):
        fr = struct.unpack_from('<I', d, rva2off(addr_rva) + 4*i)[0]
        if fr: print(f'  {base+i:4d} 0x{fr:08x} {names.get(i, "(noname)")}')
if imp_rva:
    o = rva2off(imp_rva)
    while True:
        ilt, ts, fc, nm, iat = struct.unpack_from('<IIIII', d, o)
        if not nm: break
        n = 0; p = rva2off(ilt or iat)
        while struct.unpack_from('<I', d, p)[0]: n += 1; p += 4
        print(f'imports {cstr(nm)}: {n}')
        o += 20
