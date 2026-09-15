#!/usr/bin/env python3
"""vpdis.py <pe> <start> <end> <r2> — pe-dis with TOC-relative loads resolved."""
import sys, os, struct, subprocess, re
TOOLS='/workspaces/granny-smith/local/powermac-nt-hal/tools'
img, start, end, R2 = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4],16)
d=open(img,'rb').read()
pe=struct.unpack_from('<I',d,0x3c)[0]; optsz=struct.unpack_from('<H',d,pe+20)[0]
base=struct.unpack_from('<I',d,pe+24+28)[0]
secs=[]; o=pe+24+optsz
for i in range(struct.unpack_from('<H',d,pe+6)[0]):
    s=d[o+i*40:o+(i+1)*40]; nm=s[:8].rstrip(b'\0').decode()
    vs,va,rs,ro=struct.unpack_from('<IIII',s,8); secs.append((nm,base+va,vs,ro))
def va2off(va):
    for nm,sva,vs,ro in secs:
        if sva<=va<sva+vs: return ro+(va-sva)
def rd(va):
    o=va2off(va); return None if o is None else struct.unpack_from('<I',d,o)[0]
def cstr(va):
    o=va2off(va)
    if o is None: return None
    try: e=d.index(b'\0',o)
    except ValueError: return None
    s=d[o:e]
    if not (1<=len(s)<=60) or not all(9<=b<127 for b in s): return None
    return s.decode('latin1')
out=subprocess.run([sys.executable, f'{TOOLS}/pe-dis.py', img, start, end],
                   capture_output=True, text=True).stdout
pat=re.compile(r'lwz\s+(r\d+),(-?\d+)\(r2\)')
for line in out.split('\n'):
    m=pat.search(line)
    if m:
        p=rd(R2+int(m.group(2)))
        if p is not None:
            s=cstr(p)
            line += f'   ; "{s}"' if s else f'   ; -> {p:#x}'
    print(line)
