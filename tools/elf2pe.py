#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
"""elf2pe.py — turn a statically linked PowerPC-LE ELF (lld, --emit-relocs) into a
Windows NT 4.0 PowerPC PE image (machine 0x1F0) with exports, imports and base relocations.

    elf2pe.py hal.elf hal.dll --exports hal.exports --dllname HAL.dll [--entry desc_HalInitSystem]

Conventions the ELF must follow (see src/thunk.S and tools/mkstubs.py):
  * every exported function F has a descriptor symbol `desc_F` (two words: entry, toc);
    the export table points at the descriptor, as the NT PowerPC ABI requires;
  * a data export D (marked `D DATA` in the exports file) is exported by its own address;
  * every kernel import F has a 4-byte slot `__imp_F` in section `.idata`; the import
    directory built here names each slot after the symbol, minus the `__imp_` prefix.
Relocations: ELF R_PPC_ADDR32 -> IMAGE_REL_BASED_HIGHLOW, R_PPC_ADDR16_LO -> LOW,
R_PPC_ADDR16_HI -> HIGH, R_PPC_ADDR16_HA -> HIGHADJ (with the low half as the extra word);
REL24/REL14 need nothing.  The checksum is the standard PE checksum, because the NT boot
loader verifies it.
"""
import argparse, os, struct, sys, time

# Reproducible by default: a PE carries a build timestamp in two places and NT's loader
# reads neither, so stamping the clock into them only makes two identical builds differ.
# SOURCE_DATE_EPOCH overrides it, per the reproducible-builds convention.
TIMESTAMP = int(os.environ.get('SOURCE_DATE_EPOCH', '0'))

R_PPC_ADDR32, R_PPC_ADDR16_LO, R_PPC_ADDR16_HI, R_PPC_ADDR16_HA = 1, 4, 5, 6
R_PPC_REL24, R_PPC_REL14, R_PPC_NONE = 10, 11, 0
R_PPC_ADDR16 = 3

class Elf:
    def __init__(self, data):
        self.d = data
        (ei_class, ei_data) = data[4], data[5]
        assert ei_class == 1 and ei_data == 1, 'need ELF32 little-endian'
        (self.e_type, self.e_machine, _, self.e_entry, self.e_phoff, self.e_shoff, _, _, _, _,
         self.e_shentsize, self.e_shnum, self.e_shstrndx) = struct.unpack_from('<HHIIIIIHHHHHH', data, 16)
        assert self.e_machine == 20, 'need EM_PPC'
        self.sections = []
        for i in range(self.e_shnum):
            off = self.e_shoff + i * self.e_shentsize
            (name, typ, flags, addr, offset, size, link, info, align, entsize) = struct.unpack_from('<IIIIIIIIII', data, off)
            self.sections.append(dict(name=name, type=typ, flags=flags, addr=addr, offset=offset, size=size,
                                      link=link, info=info, align=align, entsize=entsize, idx=i))
        shstr = self.sections[self.e_shstrndx]
        for s in self.sections:
            s['sname'] = self.cstr(shstr['offset'] + s['name'])
        self.symtab = [s for s in self.sections if s['type'] == 2][0]
        strtab = self.sections[self.symtab['link']]
        self.symbols = []
        for i in range(self.symtab['size'] // 16):
            (st_name, st_value, st_size, st_info, st_other, st_shndx) = struct.unpack_from('<IIIBBH', data, self.symtab['offset'] + 16 * i)
            self.symbols.append(dict(name=self.cstr(strtab['offset'] + st_name), value=st_value, size=st_size,
                                     info=st_info, shndx=st_shndx))
        self.byname = {s['name']: s for s in self.symbols if s['name']}
    def cstr(self, off):
        e = self.d.index(b'\0', off); return self.d[off:e].decode()
    def relocs(self):
        """Yield (target_section, r_offset(address), type, symbol, addend) for every RELA section."""
        for s in self.sections:
            if s['type'] != 4: continue  # SHT_RELA
            tgt = self.sections[s['info']]
            if not (tgt['flags'] & 2): continue  # SHF_ALLOC
            for i in range(s['size'] // 12):
                (r_offset, r_info, r_addend) = struct.unpack_from('<IIi', self.d, s['offset'] + 12 * i)
                yield tgt, r_offset, r_info & 0xff, self.symbols[r_info >> 8], r_addend

def align(v, a): return (v + a - 1) & ~(a - 1)

def pe_checksum(data):
    # standard PE checksum (16-bit ones-complement style), checksum field assumed zero
    s = 0
    n = len(data) // 2
    for (w,) in struct.iter_unpack('<H', data[:n * 2]):
        s += w
        s = (s & 0xffff) + (s >> 16)
    if len(data) & 1:
        s += data[-1]
        s = (s & 0xffff) + (s >> 16)
    s = (s & 0xffff) + (s >> 16)
    return (s & 0xffff) + len(data)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('elf'); ap.add_argument('out')
    ap.add_argument('--exports', required=True); ap.add_argument('--dllname', default='HAL.dll')
    ap.add_argument('--entry', default='desc_HalInitSystem')
    ap.add_argument('--import-dll', default='ntoskrnl.exe')
    ap.add_argument('--section-align', type=lambda x: int(x, 0), default=0x1000)
    ap.add_argument('--file-align', type=lambda x: int(x, 0), default=0x200)
    a = ap.parse_args()
    elf = Elf(open(a.elf, 'rb').read())
    SA, FA = a.section_align, a.file_align

    # --- gather allocated sections, grouped into PE sections by name -------------------
    alloc = [s for s in elf.sections if (s['flags'] & 2) and s['type'] in (1, 8) and s['size']]
    alloc.sort(key=lambda s: s['addr'])
    base = min(s['addr'] for s in alloc) & ~(SA - 1)
    image_base = base - SA  # headers occupy the first page below the first section
    def rva(addr): return addr - image_base
    # PE section descriptors: name, rva, vsize, data(bytes), chars
    pesecs = []
    for s in alloc:
        name = s['sname']
        if name.startswith('.text'): pname, chars = '.text', 0x60000020
        elif name.startswith('.rodata') or name == '.rdata': pname, chars = '.rdata', 0x40000040
        elif name.startswith('.idata'): pname, chars = '.idata', 0xC0000040
        elif name.startswith('.data') or name.startswith('.sdata') or name.startswith('.got'): pname, chars = '.data', 0xC0000040
        elif name.startswith('.bss') or name.startswith('.sbss'): pname, chars = '.data', 0xC0000040
        else: pname, chars = name[:8], 0xC0000040
        data = b'' if s['type'] == 8 else elf.d[s['offset']:s['offset'] + s['size']]
        if pesecs and pesecs[-1]['name'] == pname:
            p = pesecs[-1]
            gap = s['addr'] - (p['addr'] + len(p['data']) if p['data_end_is_vsize'] else p['addr'] + p['vsize'])
            # pad initialized data up to this section's start (bss after data is fine, it is trailing)
            if data:
                p['data'] += b'\0' * (s['addr'] - (p['addr'] + len(p['data']))) + data
                p['data_end_is_vsize'] = True
            p['vsize'] = s['addr'] + s['size'] - p['addr']
            p['data_end_is_vsize'] = bool(data) and p['data_end_is_vsize']
        else:
            pesecs.append(dict(name=pname, addr=s['addr'], vsize=s['size'], data=data, chars=chars,
                               data_end_is_vsize=bool(data)))
    for p in pesecs:
        p['rva'] = rva(p['addr'])

    syms = elf.byname
    # --- exports ---------------------------------------------------------------------
    exports = []  # (name, address)
    for line in open(a.exports):
        line = line.split('#')[0].strip()
        if not line: continue
        parts = line.split()
        name = parts[0]
        if len(parts) > 1 and parts[1].upper() == 'DATA':
            sym = syms.get(name)
        else:
            sym = syms.get('desc_' + name)
        if sym is None: sys.exit(f'elf2pe: export {name}: no symbol {"desc_" + name if len(parts) == 1 else name}')
        exports.append((name, sym['value']))
    exports.sort(key=lambda e: e[0].encode())  # the loader binary-searches by name
    # --- imports: __imp_* slots in .idata --------------------------------------------
    imps = sorted([s for s in elf.symbols if s['name'].startswith('__imp_') and s['shndx'] not in (0, 0xfff1)], key=lambda s: s['value'])
    if imps:
        lo, hi = imps[0]['value'], imps[-1]['value'] + 4
        for i, s in enumerate(imps):
            assert s['value'] == lo + 4 * i, 'import slots must be contiguous'
    # --- base relocations --------------------------------------------------------------
    fixups = []  # (rva, type, extra)
    for tgt, off, typ, sym, addend in elf.relocs():
        if typ in (R_PPC_REL24, R_PPC_REL14, R_PPC_NONE): continue
        value = (sym['value'] + addend) & 0xffffffff
        if typ == R_PPC_ADDR32: fixups.append((rva(off), 3, None))
        elif typ == R_PPC_ADDR16_LO: fixups.append((rva(off), 2, None))
        elif typ == R_PPC_ADDR16_HI: fixups.append((rva(off), 1, None))
        elif typ == R_PPC_ADDR16_HA: fixups.append((rva(off), 4, value & 0xffff))
        elif typ == R_PPC_ADDR16: sys.exit(f'elf2pe: R_PPC_ADDR16 at {off:#x} cannot be base-relocated; use @l/@ha')
        else: sys.exit(f'elf2pe: unsupported relocation type {typ} at {off:#x} in {tgt["sname"]}')
    fixups.sort()
    # --- lay out the generated sections: .edata, .idata directory, .reloc ---------------
    end_rva = max(p['rva'] + align(p['vsize'], SA) for p in pesecs)
    # .edata
    edata_rva = end_rva
    n = len(exports)
    names_blob = b''; name_rvas = []
    dllname_off = 40 + n * 4 + n * 4 + n * 2
    cur = dllname_off
    dll_bytes = a.dllname.encode() + b'\0'
    cur += len(dll_bytes)
    for name, _ in exports:
        name_rvas.append(edata_rva + cur); nb = name.encode() + b'\0'; names_blob += nb; cur += len(nb)
    edata = struct.pack('<IIHHIIIIIII', 0, TIMESTAMP, 0, 0, edata_rva + dllname_off, 1, n, n,
                        edata_rva + 40, edata_rva + 40 + 4 * n, edata_rva + 40 + 8 * n)
    edata += b''.join(struct.pack('<I', rva(addr)) for _, addr in exports)
    edata += b''.join(struct.pack('<I', r) for r in name_rvas)
    edata += b''.join(struct.pack('<H', i) for i in range(n))
    edata += dll_bytes + names_blob
    pesecs.append(dict(name='.edata', rva=edata_rva, vsize=len(edata), data=edata, chars=0x40000040))
    end_rva = edata_rva + align(len(edata), SA)
    # import directory (the IAT itself lives in the ELF's .idata)
    idir_rva = end_rva
    if imps:
        # layout: descriptor(20) + null(20) | ILT (n+1)*4 | dllname | hint/name entries
        ilt_rva = idir_rva + 40
        cur = 40 + (len(imps) + 1) * 4
        dll2 = a.import_dll.encode() + b'\0'
        if len(dll2) & 1: dll2 += b'\0'          # hint/name entries must be 2-byte aligned (the loader reads the hint as a USHORT)
        dllname_rva = idir_rva + cur; cur += len(dll2)
        hints = b''; hint_rvas = []
        for s in imps:
            hint_rvas.append(idir_rva + cur)
            hb = struct.pack('<H', 0) + s['name'][6:].encode() + b'\0'
            if len(hb) & 1: hb += b'\0'
            hints += hb; cur += len(hb)
        iat_rva = rva(imps[0]['value'])
        idir = struct.pack('<IIIII', ilt_rva, 0, 0, dllname_rva, iat_rva) + b'\0' * 20
        idir += b''.join(struct.pack('<I', r) for r in hint_rvas) + b'\0\0\0\0'
        idir += dll2 + hints
        pesecs.append(dict(name='.idir', rva=idir_rva, vsize=len(idir), data=idir, chars=0x40000040))
        end_rva = idir_rva + align(len(idir), SA)
        # NT's boot loader binds imports by walking the IAT and reading each slot as a hint/name RVA
        # (it never consults OriginalFirstThunk), so the IAT must start out as a copy of the ILT.
        idata = [p for p in pesecs if p['name'] == '.idata'][0]
        buf = bytearray(idata['data'].ljust(idata['vsize'], b'\0'))
        for s_, hr in zip(imps, hint_rvas):
            o = s_['value'] - idata['addr']
            struct.pack_into('<I', buf, o, hr)
        idata['data'] = bytes(buf)
    # .reloc
    reloc = b''
    page = None; entries = []
    def flush():
        nonlocal reloc, entries, page
        if page is None: return
        body = b''.join(entries)
        if len(body) & 3: body += b'\0\0'
        reloc += struct.pack('<II', page, 8 + len(body)) + body
        entries = []
    for r, typ, extra in fixups:
        p = r & ~0xfff
        if p != page:
            flush(); page = p
        entries.append(struct.pack('<H', (typ << 12) | (r & 0xfff)))
        if typ == 4: entries.append(struct.pack('<h', extra - 0x10000 if extra & 0x8000 else extra))
    flush()
    reloc_rva = end_rva
    pesecs.append(dict(name='.reloc', rva=reloc_rva, vsize=len(reloc), data=reloc, chars=0x42000040))
    end_rva = reloc_rva + align(len(reloc), SA)
    # --- headers -----------------------------------------------------------------------
    nsec = len(pesecs)
    size_headers = align(0x80 + 4 + 20 + 0xE0 + 40 * nsec, FA)
    file_off = size_headers
    for p in pesecs:
        p['raw_size'] = align(len(p['data']), FA)
        p['raw_off'] = file_off if p['raw_size'] else 0
        file_off += p['raw_size']
    text = [p for p in pesecs if p['name'] == '.text'][0]
    datas = [p for p in pesecs if p['chars'] & 0x40 and p['name'] != '.text']
    entry_sym = syms.get(a.entry)
    if entry_sym is None: sys.exit(f'elf2pe: no entry symbol {a.entry}')
    size_code = sum(p['raw_size'] for p in pesecs if p['chars'] & 0x20)
    size_idata = sum(p['raw_size'] for p in datas)
    size_udata = sum(max(0, p['vsize'] - len(p['data'])) for p in datas)
    dos = bytearray(0x80)
    dos[0:2] = b'MZ'; struct.pack_into('<I', dos, 0x3c, 0x80)
    coff = struct.pack('<HHIIIHH', 0x1F0, nsec, TIMESTAMP, 0, 0, 0xE0, 0x2102)
    dirs = [0] * 32
    dirs[0], dirs[1] = edata_rva, len(edata)
    if imps:
        dirs[2], dirs[3] = idir_rva, 40
        dirs[24], dirs[25] = iat_rva, 4 * len(imps)
    dirs[10], dirs[11] = reloc_rva, len(reloc)
    opt = struct.pack('<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII', 0x10B, 3, 10, size_code, size_idata, size_udata,
                      rva(entry_sym['value']), text['rva'], datas[0]['rva'] if datas else 0, image_base, SA, FA,
                      4, 0, 4, 0, 4, 0, 0, end_rva, size_headers, 0, 1, 0,
                      0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
    opt += struct.pack('<32I', *dirs)
    sect = b''
    for p in pesecs:
        sect += struct.pack('<8sIIIIIIHHI', p['name'].encode().ljust(8, b'\0'), p['vsize'], p['rva'], p['raw_size'], p['raw_off'], 0, 0, 0, 0, p['chars'])
    hdr = bytes(dos) + b'PE\0\0' + coff + opt + sect
    out = bytearray(hdr.ljust(size_headers, b'\0'))
    for p in pesecs:
        if p['raw_size']:
            assert len(out) == p['raw_off']
            out += p['data'].ljust(p['raw_size'], b'\0')
    cs = pe_checksum(bytes(out))
    struct.pack_into('<I', out, 0x80 + 4 + 20 + 64, cs)
    open(a.out, 'wb').write(out)
    print(f'{a.out}: base {image_base:#x} image {end_rva:#x} entry rva {rva(entry_sym["value"]):#x} '
          f'{len(exports)} exports {len(imps)} imports {len(fixups)} fixups checksum {cs:#x}')
    for p in pesecs:
        print(f'  {p["name"]:7s} rva {p["rva"]:#08x} vsize {p["vsize"]:#07x} raw {p["raw_off"]:#07x}+{p["raw_size"]:#06x}')

if __name__ == '__main__':
    main()
