#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
# A PowerPC disassembler, enough of one to read an NT 4.0 PowerPC PE or an AIX kernel
# extension.  Vendored from this project author's own research tools; see PROVENANCE.md.
import struct

SPR = {1:'xer', 8:'lr', 9:'ctr', 18:'dsisr', 19:'dar', 22:'dec', 25:'sdr1',
       26:'srr0', 27:'srr1', 272:'sprg0', 273:'sprg1', 274:'sprg2', 275:'sprg3',
       268:'tbl', 269:'tbu', 282:'ear', 287:'pvr', 528:'ibat0u', 529:'ibat0l', 1008:'hid0', 1009:'hid1'}

def _spr(n):  # split field, halves swapped
    v = ((n >> 5) & 0x1F) | ((n & 0x1F) << 5)
    return SPR.get(v, f'spr{v}')

def _s16(v): return v - 0x10000 if v & 0x8000 else v

BO_HINT = {12: '', 4: 'n', 16: 'dnz', 18: 'dz'}

CRB = ['lt', 'gt', 'eq', 'so']

CR_OPS = {257: 'crand', 449: 'cror', 193: 'crxor', 33: 'crnor', 129: 'crandc',
          289: 'creqv', 225: 'crnand', 417: 'crorc'}

def _cond(bo, bi):
    # Returns a mnemonic suffix + the CR field, for the common BO forms.
    fld, bit = bi >> 2, bi & 3
    cr = '' if fld == 0 else f'cr{fld},'
    if bo & 0x10:                      # branch always / ctr forms
        if bo & 0x04:
            return ('b', '')
        return ('bdnz' if not (bo & 0x02) else 'bdz', cr)
    true = (bo & 0x08) != 0
    name = {'lt': ('blt', 'bge'), 'gt': ('bgt', 'ble'),
            'eq': ('beq', 'bne'), 'so': ('bso', 'bns')}[CRB[bit]]
    return (name[0] if true else name[1], cr)

D_FORM = {14: 'addi', 15: 'addis', 12: 'addic', 13: 'addic.', 8: 'subfic',
          7: 'mulli', 28: 'andi.', 29: 'andis.', 24: 'ori', 25: 'oris',
          26: 'xori', 27: 'xoris'}
LS = {32: ('lwz', 0), 33: ('lwzu', 0), 34: ('lbz', 0), 35: ('lbzu', 0),
      36: ('stw', 1), 37: ('stwu', 1), 38: ('stb', 1), 39: ('stbu', 1),
      40: ('lhz', 0), 41: ('lhzu', 0), 42: ('lha', 0), 43: ('lhau', 0),
      44: ('sth', 1), 45: ('sthu', 1), 46: ('lmw', 0), 47: ('stmw', 1),
      48: ('lfs', 0), 50: ('lfd', 0), 52: ('stfs', 1), 54: ('stfd', 1)}
X_31 = {23: 'lwzx', 87: 'lbzx', 151: 'stwx', 215: 'stbx', 279: 'lhzx',
        343: 'lhax', 407: 'sthx', 55: 'lwzux', 119: 'lbzux', 183: 'stwux',
        247: 'stbux', 311: 'lhzux', 439: 'sthux',
        534: 'lwbrx', 662: 'stwbrx', 790: 'lhbrx', 918: 'sthbrx',
        20: 'lwarx', 150: 'stwcx.', 1014: 'dcbz', 54: 'dcbst', 86: 'dcbf',
        982: 'icbi', 470: 'dcbi', 278: 'dcbt', 246: 'dcbtst', 598: 'sync',
        306: 'tlbie', 566: 'tlbsync', 370: 'tlbia', 854: 'eieio',
        533: 'lswx', 661: 'stswx', 4: 'tw'}
XO_31 = {266: 'add', 10: 'addc', 138: 'adde', 40: 'subf', 8: 'subfc',
         136: 'subfe', 104: 'neg', 235: 'mullw', 75: 'mulhw', 11: 'mulhwu',
         491: 'divw', 459: 'divwu', 234: 'addme', 202: 'addze',
         232: 'subfme', 200: 'subfze'}
LOGIC_31 = {28: 'and', 444: 'or', 316: 'xor', 476: 'nand', 124: 'nor',
            60: 'andc', 412: 'orc', 284: 'eqv', 24: 'slw', 536: 'srw',
            792: 'sraw', 824: 'srawi', 954: 'extsb', 922: 'extsh',
            26: 'cntlzw'}

def dis(insn, pc):
    op = insn >> 26
    rd = (insn >> 21) & 31
    ra = (insn >> 16) & 31
    rb = (insn >> 11) & 31
    d = insn & 0xFFFF
    rc = insn & 1
    if insn == 0x60000000:
        return 'nop'
    if op in D_FORM:
        m = D_FORM[op]
        if op in (14, 15) and ra == 0:
            return f'li{"s" if op == 15 else ""}    r{rd},{_s16(d) if op==14 else d:#x}'
        imm = d if op in (28, 29, 24, 25, 26, 27) else _s16(d)
        return f'{m:<7}r{rd},r{ra},{imm:#x}' if op not in (28,29,24,25,26,27) else f'{m:<7}r{ra},r{rd},{imm:#x}'
    if op in LS:
        m, st = LS[op]
        return f'{m:<7}r{rd},{_s16(d)}(r{ra})'
    if op == 3:                                     # twi
        return f'twi    {rd},r{ra},{_s16(d):#x}'
    if op == 11 or op == 10:                       # cmpi / cmpli
        fld = rd >> 2
        cr = '' if fld == 0 else f'cr{fld},'
        return f'{"cmpwi" if op==11 else "cmplwi":<7}{cr}r{ra},{_s16(d) if op==11 else d:#x}'
    if op == 18:                                    # b / bl / ba / bla
        li = insn & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        tgt = li if insn & 2 else pc + li
        m = ('b', 'ba', 'bl', 'bla')[(insn & 1) * 2 + (1 if insn & 2 else 0)]
        return f'{m:<7}{tgt:#010x}'
    if op == 16:                                    # bc
        bo, bi = rd, ra
        bd = insn & 0xFFFC
        if bd & 0x8000:
            bd -= 0x10000
        tgt = bd if insn & 2 else pc + bd
        nm, cr = _cond(bo, bi)
        nm += 'l' if insn & 1 else ''
        nm += 'a' if insn & 2 else ''
        return f'{nm:<7}{cr}{tgt:#010x}'
    if op == 19:
        xo = (insn >> 1) & 0x3FF
        if xo == 16:
            nm, cr = _cond(rd, ra)
            return f'{nm + "lr" + ("l" if insn & 1 else ""):<7}{cr}'
        if xo == 528:
            nm, cr = _cond(rd, ra)
            return f'{nm + "ctr" + ("l" if insn & 1 else ""):<7}{cr}'
        if xo == 50:
            return 'rfi'
        if xo == 150:
            return 'isync'
        if xo in CR_OPS:
            return f'{CR_OPS[xo]:<7}{rd},{ra},{rb}'
        if xo == 0:
            return f'mcrf   cr{rd >> 2},cr{ra >> 2}'
        return f'.19.{xo}'
    if op in (20, 21, 23):                          # rlwimi / rlwinm / rlwnm
        sh, mb, me = rb, (insn >> 6) & 31, (insn >> 1) & 31
        m = {20: 'rlwimi', 21: 'rlwinm', 23: 'rlwnm'}[op]
        return f'{m + ("." if rc else ""):<7}r{ra},r{rd},{sh},{mb},{me}'
    if op == 31:
        xo = (insn >> 1) & 0x3FF
        if xo == 0 or xo == 32:
            fld = rd >> 2
            cr = '' if fld == 0 else f'cr{fld},'
            return f'{"cmpw" if xo == 0 else "cmplw":<7}{cr}r{ra},r{rb}'
        if xo in X_31:
            m = X_31[xo]
            if m in ('sync', 'isync', 'tlbsync', 'tlbia'):
                return m
            return f'{m:<7}r{rd},r{ra},r{rb}'
        if xo in XO_31 or (xo & 0x1FF) in XO_31:
            m = XO_31.get(xo, XO_31.get(xo & 0x1FF))
            oe = '' if xo < 512 else 'o'
            return f'{m + oe + ("." if rc else ""):<7}r{rd},r{ra},r{rb}'
        if xo in LOGIC_31:
            m = LOGIC_31[xo]
            if m == 'srawi':
                return f'{m + ("." if rc else ""):<7}r{ra},r{rd},{rb}'
            if m in ('extsb', 'extsh', 'cntlzw'):
                return f'{m + ("." if rc else ""):<7}r{ra},r{rd}'
            if m == 'or' and rd == rb:
                return f'mr     r{ra},r{rd}'
            return f'{m + ("." if rc else ""):<7}r{ra},r{rd},r{rb}'
        if xo == 371:
            return f'mftb   r{rd},{_spr((insn >> 11) & 0x3FF)}'
        if xo == 512:
            return f'mcrxr  cr{rd >> 2}'
        if xo == 597:
            return f'lswi   r{rd},r{ra},{rb}'
        if xo == 725:
            return f'stswi  r{rd},r{ra},{rb}'
        if xo == 339:
            return f'mfspr  r{rd},{_spr((insn >> 11) & 0x3FF)}'
        if xo == 467:
            return f'mtspr  {_spr((insn >> 11) & 0x3FF)},r{rd}'
        if xo == 19:
            return f'mfcr   r{rd}'
        if xo == 144:
            return f'mtcrf  {(insn >> 12) & 0xFF:#x},r{rd}'
        if xo == 83:
            return f'mfmsr  r{rd}'
        if xo == 146:
            return f'mtmsr  r{rd}'
        if xo == 595:
            return f'mfsr   r{rd},{(insn >> 16) & 15}'
        if xo == 210:
            return f'mtsr   {(insn >> 16) & 15},r{rd}'
        return f'.31.{xo}'
    return f'.word  {insn:#010x}'
