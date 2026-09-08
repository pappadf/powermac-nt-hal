#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
"""mkbootscript.py --veneer <VENEER.EXE> --ckpt <pre-go.ckpt> [--out boot.gs] — generate the
Granny Smith script that boots the *installed* system from disk.

Setup leaves a bootable system behind, but this machine cannot start it unaided: it has no ARC
NVRAM (`STORY.md` ledger rows 11 and 12), so a reboot begins with no `OSLOADER` path, and two
veneer patches this project carries for the CD are wrong for a disk (walls 46 and 47). This
writes the script that makes up the difference — see
`docs/2026-09-07-booting-the-installed-disk.md`, whose every trace came from its output.

Everything it emits is either this project's own, or read out of **the veneer image you supply**:
two ranges are restored to exactly what your `VENEER.EXE` ships, and no Microsoft code is stored
here. That is the same arrangement `mkoem.py` uses for a user's CD image.

    python3 tools/mkbootscript.py \
        --veneer /path/to/PPC/VENEER.EXE \
        --ckpt   tmp/nt-pre-go-big2.ckpt \
        --out    tmp/hal/boot-installed.gs

then run it with `tools/run-boot.py`, which splices the installed disk image into the
checkpoint's copy-on-write delta first.

The checkpoint must be taken at the Open Firmware prompt with the veneer loaded and `go` not yet
typed — the same pre-`go` checkpoint the OEM patches need (wall 28), because anything applied at
a later checkpoint is applied after the veneer has already read it.
"""
import argparse, struct, sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from coffsyms import Coff

# ---- the veneer's fixed addresses, all image VAs (base 0x50000) --------------------------------
OFCLOSE_NRET   = {0x52254: 0x39200000,          # li r9,0 / stw r9,8(r3): give OFClose an nret
                  0x52258: 0x91230008}          #   slot, so it stops leaking ihandles (ledger 2)
PARTITION_KEY  = 0x5d0c0                        # "partition(1)", blanked by a CD-era patch
COLON_ZERO     = 0x5e168                        # ":0", likewise
VROPEN_RESTORE = (0x54744, 0x5486c)             # wall 46: VrOpen's partition/filepath assembly
PATHBUF_RESTORE = (0x5cd30, 0x5cd50)            # wall 47: the boot-file buffer *and* 'OsLoader'
SCSI_MODEL     = 0x5f420                        # wall 16: the ARC tree's SCSI model string
ARGV_SLOT0_VAL = 0x60c1c                        # argv table slot 0 ("OsLoader") .value
FREE_TEXT      = 0x5c228                        # .text tail padding: 472 bytes to 0x5c400
VRENVC         = 0x5cad4                        # the veneer's local environment count
VRENVP         = 0x5cae0                        #   ... and the array itself (not a pointer to it)
VRDEBUG        = 0x60c08                        # the debug bitmask; see the doc's table
GETENV_ENTRY   = 0x5a350                        # ..VrGetEnvironmentVariable

SCSI_MODEL_TEXT = b'NCR,825A\0\0\0'             # what Setup's [Map.SCSI] and symc810 bind

# Values are stored NAME-then-value with **no separator**: the veneer's FindInLocalEnv compares
# strlen(name) - 1 characters and VrGetEnvironmentVariable returns entry + strlen(name), so an
# '=' between them comes back as the first character of every value.
def arc_environment(sys_part, os_part, osloader, winnt, options, identifier):
    return [
        ('SYSTEMPARTITION', f'multi(0)scsi(1)disk(0)rdisk(0)partition({sys_part})'),
        ('OSLOADER',        f'multi(0)scsi(1)disk(0)rdisk(0)partition({sys_part}){osloader}'),
        ('OSLOADPARTITION', f'multi(0)scsi(1)disk(0)rdisk(0)partition({os_part})'),
        ('OSLOADFILENAME',  winnt),
        ('OSLOADOPTIONS',   options),
        ('LOADIDENTIFIER',  identifier),
        ('AUTOLOAD',        'YES'),
        ('COUNTDOWN',       '5'),
        ('LASTKNOWNGOOD',   'FALSE'),
        ('PROCESSORS',      '1'),
    ]


class Script:
    """Accumulates .gs lines, and knows the two little-endian address munges.

    In the 604's little-endian mode a word at A lives at physical A ^ 4 and a byte at A at
    A ^ 7, and the emulator shell's poke/peek are physical. Every address in this file is the
    address the *guest* sees; these two are the only places the munge appears."""
    def __init__(self): self.out = []
    def __call__(self, line=''): self.out.append(line)
    def word(self, va, value, note=''):
        self.out.append(f'machine.memory.poke.l {va ^ 4:#x} {value:#010x}'
                        + (f'   # {note}' if note else ''))
    def byte(self, va, value, note=''):
        self.out.append(f'machine.memory.poke.b {va ^ 7:#x} {value}'
                        + (f'   # {note}' if note else ''))
    def blob(self, va, data, note=''):
        if note: self.out.append(f'# {note}')
        for i, b in enumerate(data): self.byte(va + i, b)
    def type_of(self, text, spin=6000000):
        """Type a line at the Open Firmware prompt. The SCC's receive FIFO holds about sixteen
        characters, so anything longer loses its *head* unless it goes in with the machine
        running in between."""
        for k in range(0, len(text), 4):
            chunk = text[k:k+4].replace('\\', '\\\\').replace('"', '\\"')
            self.out.append(f'machine.scc.a.receive("{chunk}")')
            self.out.append(f'scheduler.run {spin}')
        self.out.append('machine.scc.a.receive("\\r")')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--veneer', required=True, help='the VENEER.EXE this checkpoint is running')
    ap.add_argument('--ckpt', required=True, help='a pre-`go` checkpoint at the OF prompt')
    ap.add_argument('--out', default='-')
    ap.add_argument('--system-partition', type=int, default=1)
    ap.add_argument('--os-partition', type=int, default=2)
    ap.add_argument('--osloader', default=r'\os\winnt40\osloader.exe')
    ap.add_argument('--winnt', default=r'\WINNT')
    ap.add_argument('--options', default='NODEBUG')
    ap.add_argument('--identifier', default='Windows NT Workstation Version 4.00')
    ap.add_argument('--disk', default='/bandit/53c825@12/sd@0,0',
                    help='the Open Firmware path to set as /chosen bootpath')
    ap.add_argument('--vrdebug', default='0x2060',
                    help='veneer debug bitmask (doc §9): 0x2000 argv, 0x1000 reads, 0x200 opens')
    ap.add_argument('--screenshot', default='tmp/hal/boot-installed.png')
    ap.add_argument('--chunks', type=int, default=900)
    a = ap.parse_args()

    ven = Coff(a.veneer)
    if ven.base != 0x50000:
        sys.exit(f'{a.veneer}: image base {ven.base:#x}, expected 0x50000 — not this veneer?')

    def shipped_word(va):
        off = ven.va2off(va)
        if off is None: sys.exit(f'{va:#x} is not in {a.veneer}')
        return struct.unpack_from('<I', ven.d, off)[0]

    def shipped_bytes(lo, hi):
        return bytes(ven.d[ven.va2off(lo): ven.va2off(lo) + (hi - lo)])

    s = Script()
    s('# Generated by tools/mkbootscript.py — do not edit; regenerate.')
    s('# Boots the system text-mode Setup installed. See')
    s('#   docs/2026-09-07-booting-the-installed-disk.md')
    s('')
    s('def nt_wait_ok(budget) {')
    s('    let out = ""')
    s('    let used = 0')
    s('    while !contains($out, " ok") && !contains($out, "0 > ") && $used < $budget {')
    s('        scheduler.run 20000000')
    s('        $out = "${$out}${machine.scc.a.sent()}"')
    s('        $used = $used + 20000000')
    s('    }')
    s('    scheduler.run 10000000')
    s('    $out = "${$out}${machine.scc.a.sent()}"')
    s('    return $out')
    s('}')
    s(f'checkpoint.load "{a.ckpt}"')
    s('echo "=== loaded pre-go: pc=${machine.cpu.pc} msr=${machine.cpu.msr} ==="')
    s('let junk = machine.scc.a.sent()')
    s('')

    s('echo "=== the veneer fix we always need: OFClose nret (ledger row 2) ==="')
    for va, w in OFCLOSE_NRET.items(): s.word(va, w)
    s('')

    s('echo "=== undo the CD-era string patches: a disk needs partition(N) and \\":0\\" back ==="')
    s.byte(PARTITION_KEY, ord('p'), '0x5d0c0 = "partition(1)"')
    s.byte(COLON_ZERO, ord(':'), '0x5e168 = ":0"')
    s('')

    s(f'echo "=== the veneer\'s own tracing (VrDebug {a.vrdebug}; see the doc\'s table) ==="')
    s.word(VRDEBUG, int(a.vrdebug, 0))
    s('')

    s('echo "=== wall 46: restore VrOpen\'s shipped partition/filepath assembly ==="')
    s('# The wall 22 workaround -- a nop over the branch at 0x54748 -- is CD-only. Apple\'s OF')
    s('# answers ":N" on an ISO with the root directory *as a file*, so the CD needs the')
    s('# whole-device route; an MBR disk needs ":N", which is the only thing that gives NT')
    s('# partition-relative sectors. Ledger row 4.')
    for va in range(*VROPEN_RESTORE, 4): s.word(va, shipped_word(va))
    s('')

    s('echo "=== wall 16: the SCSI model the ARC tree reports, so symc810 binds ==="')
    s.blob(SCSI_MODEL, SCSI_MODEL_TEXT)
    s('')

    s(f'echo "=== wall 47: the boot file at {a.osloader}, where it cannot overrun ==="')
    s('# The shipped buffer at 0x5cd30 holds \'\\os\\winnt\\osloader.exe\' -- 22 characters -- and')
    s('# the argv table\'s slot-0 name string \'OsLoader\' sits immediately after it at 0x5cd48.')
    s('# A 24-character path written in place erases that name, create_argv then emits the')
    s('# loader path as a *bare* argv entry, and OSLOADER dies looking its own path up by name:')
    s('# "The \'osloader\' parameter does not point to a valid file." So put the path in .text')
    s('# tail padding, repoint the table\'s value field, and restore the shipped bytes.')
    path = a.osloader.upper().encode('latin1') + b'\0'
    relocated = FREE_TEXT + 0x160                       # past the environment, below 0x5c400
    s.blob(relocated, path)
    s.blob(*PATHBUF_RESTORE[:1], shipped_bytes(*PATHBUF_RESTORE),
           'restore 0x5cd30..0x5cd4f exactly as shipped, so \'OsLoader\' is a real string')
    s.word(ARGV_SLOT0_VAL, relocated, 'argv slot 0 ("OsLoader") .value -> the relocated path')
    s('')

    s('echo "=== point /chosen bootpath at the hard disk, not the CD ==="')
    s.type_of(f'" {a.disk}" encode-string " bootpath" _chosen (property)')
    s('let p = nt_wait_ok(200000000)')
    s('echo "${$p}"')
    s('')

    # ---- the ARC environment, injected at the veneer's first lookup ----------------------------
    env = arc_environment(a.system_partition, a.os_partition, a.osloader,
                          a.winnt, a.options, a.identifier)
    blob, ptrs, at = b'', [], FREE_TEXT
    for name, value in env:
        ptrs.append(at)
        entry = (name + value).encode('latin1') + b'\0'
        blob += entry
        at += len(entry)
    if at > relocated:
        sys.exit(f'the environment ({len(blob)} bytes) runs into the relocated path at '
                 f'{relocated:#x}; shorten it or move one of them')

    s('echo "=== inject an ARC environment at OSLOADER\'s first query, then let it run ==="')
    s('debug.breakpoints.clear')
    s(f'debug.breakpoints.add {GETENV_ENTRY:#x}')
    s('machine.scc.a.receive("go")')
    s('scheduler.run 6000000')
    s('machine.scc.a.receive("\\r")')
    s('let out = ""')
    s('let i = 0')
    s('let done = 0')
    s(f'while $i < {a.chunks} {{')
    s('    scheduler.run 20000000')
    s(f'    if machine.cpu.pc == {GETENV_ENTRY:#x} {{')
    s('        if $done == 0 {')
    s('            $done = 1')
    s('            echo "--- injecting ---"')
    inner = Script()
    inner.blob(FREE_TEXT, blob)
    for k, p in enumerate(ptrs): inner.word(VRENVP + 4 * k, p)
    inner.word(VRENVP + 4 * len(ptrs), 0, 'NULL terminator')
    inner.word(VRENVC, len(ptrs))
    for line in inner.out: s('            ' + line)
    s('            debug.breakpoints.clear')
    s('            echo "--- injected, array[0]=${machine.memory.peek.l(' + f'{VRENVP ^ 4:#x}'
      + ')} VrEnvc=${machine.memory.peek.l(' + f'{VRENVC ^ 4:#x}' + ')} ---"')
    s('        }')
    s('    }')
    s('    $out = "${$out}${machine.scc.a.sent()}"')
    if a.screenshot:
        # machine.screen.save fails outright until the HAL has programmed the Cirrus, and a
        # failed statement aborts the script -- so gate every grab on the HAL saying it is up.
        s('    if contains($out, "54M30 console") && ($i % 20) == 0 {')
        s(f'        machine.screen.save "{a.screenshot[:-4]}-p${{$i}}.png"')
        s('    }')
    s('    if contains($out, "please reboot") { break }')
    s('    if contains($out, "0 > ") { break }')
    s('    if contains($out, "*** STOP") { break }')
    s('    $i = $i + 1')
    s('}')
    if a.screenshot:
        s('if contains($out, "54M30 console") {')
        s(f'machine.screen.save "{a.screenshot}"')
        s(f'echo "=== screenshot {a.screenshot} checksum ${{machine.screen.checksum()}} ==="')
        s('}')
    s('echo "=== chunks=${$i} injected=${$done} ==="')
    s('echo "${$out}"')

    text = '\n'.join(s.out) + '\n'
    if a.out == '-': sys.stdout.write(text)
    else:
        open(a.out, 'w').write(text)
        print(f'{a.out}: {len(s.out)} lines, {len(env)} environment variables '
              f'({len(blob)} bytes at {FREE_TEXT:#x}), path at {relocated:#x}')


if __name__ == '__main__':
    main()
