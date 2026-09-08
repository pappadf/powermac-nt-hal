#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
"""coffsyms.py <image> [pattern...] — read a raw NT PowerPC COFF image's header, sections and
symbol table.

Two of the binaries this project has to reason about are not PEs at all: the Network Server's
`VENEER.EXE` and NT's own `OSLOADER.EXE` are raw COFF images, no MZ stub, and both were shipped
**with their symbol tables intact**.  That is what makes them readable: `..BlOsLoader`,
`..VrOpen`, `..IsFatFileStructure`, and every string constant's mangled `??_C@` symbol, all
named.  `pe-dis.py` cannot open them because it looks for `e_lfanew`; this can.

With no pattern it lists every code symbol by address; with patterns it lists the matches.
Import it (`Coff`) for `va2off`/`off2va`/`owner`, which is how `coffdis.py` labels branches."""
import struct, sys

class Coff:
    def __init__(self, path):
        self.d = d = open(path, 'rb').read()
        (self.mach, nsec, _tim, symptr, self.nsyms, optsz, _fl) = struct.unpack_from('<HHIIIHH', d, 0)
        self.base = struct.unpack_from('<I', d, 20 + 0x1c)[0]
        self.entry = struct.unpack_from('<I', d, 20 + 0x10)[0]
        o = 20 + optsz
        self.secs = []
        for i in range(nsec):
            s = d[o + i*40: o + (i+1)*40]
            name = s[:8].rstrip(b'\0').decode('latin1')
            _pa, va, sz, ro = struct.unpack_from('<IIII', s, 8)
            self.secs.append((name, va, sz, ro))
        strtab = symptr + self.nsyms * 18
        strlen = struct.unpack_from('<I', d, strtab)[0] if strtab + 4 <= len(d) else 4
        self.strs = d[strtab: strtab + strlen]
        self.syms = []
        i = 0
        while i < self.nsyms:
            e = d[symptr + i*18: symptr + (i+1)*18]
            if e[:4] == b'\0\0\0\0':
                off = struct.unpack_from('<I', e, 4)[0]
                end = self.strs.find(b'\0', off)
                name = self.strs[off:end].decode('latin1')
            else:
                name = e[:8].rstrip(b'\0').decode('latin1')
            val, scn, typ, cls, naux = struct.unpack_from('<IhHBB', e, 8)
            self.syms.append((name, val, scn, typ, cls))
            i += 1 + naux
    def va2off(self, va):
        r = va - self.base
        for name, sva, sz, ro in self.secs:
            if sva <= r < sva + sz: return ro + (r - sva)
        return None
    def off2va(self, off):
        for name, sva, sz, ro in self.secs:
            if ro <= off < ro + sz: return self.base + sva + (off - ro)
        return None
    def funcs(self):
        """external/static symbols in a code section, sorted by VA."""
        out = []
        for name, val, scn, typ, cls in self.syms:
            if scn > 0 and cls in (2, 3):
                out.append((self.base + val, name, self.secs[scn-1][0]))
        return sorted(out)
    def owner(self, va):
        fs = self.funcs(); best = None
        for a, n, s in fs:
            if a <= va: best = (a, n, s)
            else: break
        return best

if __name__ == '__main__':
    c = Coff(sys.argv[1])
    print(f'base={c.base:08x} entry={c.entry:08x} syms={c.nsyms}')
    if len(sys.argv) > 2:
        for pat in sys.argv[2:]:
            for a, n, s in c.funcs():
                if pat.lower() in n.lower(): print(f'  {a:08x} {s:9} {n}')
    else:
        for a, n, s in c.funcs(): print(f'{a:08x} {s:9} {n}')
