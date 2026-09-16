#!/usr/bin/env python3
"""tocdis.py <image> <start> <end> — coffdis with TOC-relative loads resolved to strings."""
import sys, os, struct, subprocess, re
TOOLS='/workspaces/granny-smith/local/powermac-nt-hal/tools'
sys.path.insert(0, TOOLS)
from coffsyms import Coff
img, start, end = sys.argv[1], sys.argv[2], sys.argv[3]
c = Coff(img); R2 = 0x691b8
def rd(va):
    o=c.va2off(va); return None if o is None else struct.unpack_from('<I',c.d,o)[0]
def cstr(va):
    o=c.va2off(va)
    if o is None: return None
    try: e=c.d.index(b'\0',o)
    except ValueError: return None
    s=c.d[o:e]
    if len(s)>60 or not all(9<=b<127 for b in s): return None
    return s.decode('latin1')
out = subprocess.run([sys.executable, f'{TOOLS}/coffdis.py', img, start, end],
                     capture_output=True, text=True).stdout
pat = re.compile(r'lwz\s+(r\d+),(-?\d+)\(r2\)')
for line in out.split('\n'):
    m = pat.search(line)
    if m:
        p = rd(R2 + int(m.group(2)))
        s = cstr(p) if p else None
        if s is not None: line += f'   ; "{s}"'
        elif p: line += f'   ; -> {p:#x}'
    print(line)
