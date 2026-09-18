import sys, os, struct, subprocess, re
TOOLS='/workspaces/granny-smith/local/powermac-nt-hal/tools'
sys.path.insert(0, TOOLS)
from coffsyms import Coff
img, start, end, R2 = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4], 16)
c = Coff(img)
def rd(va):
    o=c.va2off(va); return None if o is None else struct.unpack_from('<I',c.d,o)[0]
def cstr(va):
    o=c.va2off(va)
    if o is None: return None
    try: e=c.d.index(b'\0',o)
    except ValueError: return None
    s=c.d[o:e]
    if len(s)>80 or not all(9<=b<127 for b in s): return None
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
    print(line)
