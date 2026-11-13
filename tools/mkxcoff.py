#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
"""mkxcoff.py flat.bin out.xcf base image.elf -- wrap a flat big-endian PowerPC image, linked at
`base` with its entry at byte 0, in the XCOFF that Open Firmware's own /packages/xcoff-loader
takes in its factory (big-endian) mode: both Network Server ROMs and every Power Macintosh ROM.

This is how the boot floppy's ARCFW.XCF is built: stage0 (src/stage0) and, inside it, the
little-endian ARC firmware it switches to.  The firmware stays big-endian, so the headers are
big-endian, as the format prescribes.

Three sections.  .text holds the whole image; .data is one page of zeros after it; .bss is
__bss_start..__bss_end of image.elf (the ELF the flat image came from), right after that page
(stage0.ld leaves exactly that gap), which the loader claims and clears.  The empty .data is not decoration: the
big-endian loader claims .data at the end of .text whatever its size, and a claim of nothing
fails ("CLAIM failed").  The a.out entry is a function descriptor {code, toc}, which the loader
jumps through; it follows the image.  (The same shape as powermac-os2's shim/mkxcoff.py.)"""
import struct, sys

flat = open(sys.argv[1], 'rb').read()
base = int(sys.argv[3], 0)
def elf_symbols(path):
    # ELF32, big-endian: the values of the symbols in .symtab
    d = open(path, 'rb').read()
    assert d[:4] == b'\x7fELF' and d[4] == 1 and d[5] == 2, 'not a big-endian ELF32'
    shoff, = struct.unpack('>I', d[32:36])
    shentsize, shnum = struct.unpack('>HH', d[46:50])
    secs = [struct.unpack('>IIIIIIIIII', d[shoff + i * shentsize:shoff + i * shentsize + 40]) for i in range(shnum)]
    out = {}
    for s in secs:
        if s[1] != 2: continue                                    # SHT_SYMTAB
        strtab = secs[s[6]]
        for o in range(s[4], s[4] + s[5], 16):
            name, value = struct.unpack('>II', d[o:o + 8])
            n = d[strtab[4] + name:d.index(b'\0', strtab[4] + name)].decode()
            out[n] = value
    return out
syms = elf_symbols(sys.argv[4])
bss_start = syms['__bss_start']
bss = (syms['__bss_end'] - bss_start + 0xFFF) & ~0xFFF or 0x1000
E = '>'
img = flat.ljust((len(flat) + 7) & ~7, b'\0')
desc = base + len(img)
img += struct.pack(E + 'II', base, 0)
img = img.ljust((len(img) + 0xFFF) & ~0xFFF, b'\0')
PAGE = 0x1000
end = base + len(img)
assert end + PAGE == bss_start, f'.bss must follow .data at {end + PAGE:#x} (src/stage0/stage0.ld), not {bss_start:#x}'
# file header: U802TOCMAGIC, 3 sections, no symbols, a 72-byte a.out header,
# F_RELFLG | F_EXEC | F_LNNO | F_AR32W
fh = struct.pack(E + 'HHIIIHH', 0x01DF, 3, 0, 0, 0, 72, 0x0207)
# a.out: tsize, dsize, bsize, entry, text_start, data_start, toc, then the section numbers of
# the entry (1), .text (1), .data (2), toc (0), loader (0), .bss (3)
ah = struct.pack(E + 'HHIIIIIIIHHHHHHHH2sBBIII', 0x010B, 1, len(img), PAGE, bss, desc, base, end,
                 0, 1, 1, 2, 0, 0, 3, 2, 2, b'1L', 0, 0, 0, 0, 0).ljust(72, b'\0')
def scn(name, vaddr, size, ptr, flags):
    return struct.pack(E + '8sIIIIIIHHI', name, vaddr, vaddr, size, ptr, 0, 0, 0, 0, flags)
hdr = 20 + 72 + 3 * 40
sh = (scn(b'.text', base, len(img), hdr, 0x0020) + scn(b'.data', end, PAGE, hdr + len(img), 0x0040)
      + scn(b'.bss', bss_start, bss, 0, 0x0080))
open(sys.argv[2], 'wb').write(fh + ah + sh + img + bytes(PAGE))
print(f'{sys.argv[2]}: XCOFF, .text {base:#x}+{len(img):#x}, .bss {bss_start:#x}+{bss:#x}, '
      f'entry descriptor {desc:#x}')
