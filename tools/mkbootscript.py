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


# The ADB virtual key codes, as the emulator's debug_mac_resolve_ascii numbers them (Inside
# Macintosh: Toolbox Essentials, "Virtual Key Codes").  We need the codes, not the characters,
# because keyboard.down()/up() resolve only named keys and hex keycodes -- and down()/up() is the
# only form that lets a keystroke be spread over guest time.
ADB_CODE = {
    'a': 0x00, 'b': 0x0B, 'c': 0x08, 'd': 0x02, 'e': 0x0E, 'f': 0x03, 'g': 0x05, 'h': 0x04,
    'i': 0x22, 'j': 0x26, 'k': 0x28, 'l': 0x25, 'm': 0x2E, 'n': 0x2D, 'o': 0x1F, 'p': 0x23,
    'q': 0x0C, 'r': 0x0F, 's': 0x01, 't': 0x11, 'u': 0x20, 'v': 0x09, 'w': 0x0D, 'x': 0x07,
    'y': 0x10, 'z': 0x06,
    '0': 0x1D, '1': 0x12, '2': 0x13, '3': 0x14, '4': 0x15, '5': 0x17, '6': 0x16, '7': 0x1A,
    '8': 0x1C, '9': 0x19,
    ' ': 0x31, '`': 0x32, '-': 0x1B, '=': 0x18, '[': 0x21, ']': 0x1E, '\\': 0x2A, ';': 0x29,
    "'": 0x27, ',': 0x2B, '.': 0x2F, '/': 0x2C, '\n': 0x24, '\r': 0x24, '\t': 0x30,
}
SHIFTED = {'!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8',
           '(': '9', ')': '0', '~': '`', '_': '-', '+': '=', '{': '[', '}': ']', '|': '\\',
           ':': ';', '"': "'", '<': ',', '>': '.', '?': '/'}
ADB_SHIFT = 0x38

# Long enough that each transition lands in its own auto-poll.  **This is the whole point.**
# `keyboard.type()` paces its events 4 ms apart while Cuda auto-polls every 11 ms, so a
# character's press and release arrive in one ADB Register 0 packet -- two key events in two
# bytes -- and only the first of them is acted on.  That silently ate the second letter of every
# doubled pair ("Granny" -> "Grany"), swallowed roughly half of a run of digits, and left a
# shift stuck down often enough that a digits-only field rejected everything that followed.
# Spreading the transitions over guest time gives each one a packet of its own.
ADB_GAP = 3000000


def adb_char_statements(ch):
    """down / wait / up for one character, with shift held around it when the key needs it."""
    shift = ch.isupper() or ch in SHIFTED
    base = ch.lower() if ch.isupper() else SHIFTED.get(ch, ch)
    code = ADB_CODE.get(base)
    if code is None:
        sys.exit(f'--adb-then: no ADB key code for {ch!r}')
    out = []
    if shift:
        out += [f'let kS = try(machine.adb.keyboard.down("{ADB_SHIFT:#04x}"), false)',
                f'scheduler.run {ADB_GAP}']
    out += [f'let kd = try(machine.adb.keyboard.down("{code:#04x}"), false)',
            f'scheduler.run {ADB_GAP}',
            f'let ku = try(machine.adb.keyboard.up("{code:#04x}"), false)',
            f'scheduler.run {ADB_GAP}']
    if shift:
        out += [f'let kE = try(machine.adb.keyboard.up("{ADB_SHIFT:#04x}"), false)',
                f'scheduler.run {ADB_GAP}']
    return out


def adb_statements(key):
    """The .gs lines that send one --adb-then key.

    Every call is wrapped in `try(...)`: a script statement that errors aborts the whole run, and
    there is no point losing a twenty-minute boot because one keystroke arrived while the ADB
    queue was full."""
    if key.startswith('#'):
        # NOT press(): it calls system_input_key down-then-up with no guest time in between, so
        # both transitions land in a single ADB Register 0 packet and NT sees no keystroke at
        # all.  Separate them the way type() paces its own, which is what makes them arrive as
        # two polls.  run-hal.py carries the same note -- this cost a run to rediscover.
        return [f'let kd = try(machine.adb.keyboard.down("{key[1:]}"), false)',
                'scheduler.run 3000000',
                f'let ku = try(machine.adb.keyboard.up("{key[1:]}"), false)',
                'echo "    -> ${$kd}/${$ku}"']
    if key.startswith('+'):
        call, fallback = f'machine.adb.keyboard.down("{key[1:]}")', 'false'
    elif key.startswith('-'):
        call, fallback = f'machine.adb.keyboard.up("{key[1:]}")', 'false'
    else:
        out = []
        for ch in key:
            out += adb_char_statements(ch)
        return out
    # Report what the call returned.  "the key went in and nothing happened" and "the call never
    # happened" look identical from a screenshot, and they have nothing in common as bugs.
    return [f'let kr = try({call}, {fallback})', 'echo "    -> ${$kr}"']


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
    ap.add_argument('--adb-then', action='append', default=[], metavar='KEY',
                    help='drive the GUI Setup wizard from the ADB keyboard. Each key waits until '
                         'the screen has been unchanged for --settle chunks, so Setup is asked '
                         'for the next page only once it has finished drawing this one. '
                         '"#NAME" taps a key ("#return", "#tab", "#down", "#0x24"), "+NAME" '
                         'holds one down and "-NAME" releases it (for Alt-accelerators); '
                         'anything else is typed as text, where \\n is Return and \\t is Tab. '
                         'Repeatable, sent in order.')
    ap.add_argument('--save', default='', metavar='CKPT',
                    help='checkpoint the machine once the --adb-then list is exhausted and the '
                         'screen has settled. Replaying the GUI wizard from the pre-go checkpoint '
                         'costs twenty minutes; saving at the page you are stuck on turns the '
                         'next experiment into seconds. Load it with --resume.')
    ap.add_argument('--resume', default='', metavar='CKPT',
                    help='start from a checkpoint saved by --save instead of booting: skips the '
                         'veneer patching, the ARC environment and the whole boot, and goes '
                         'straight to sending keys.')
    ap.add_argument('--keys-after', type=int, default=0, metavar='N',
                    help='hold the --adb-then sequence until chunk N. The HAL console is already '
                         'on the Cirrus long before the GUI wizard is, and a quiet stretch during '
                         'driver load looks exactly like a page waiting for input -- this is the '
                         'floor that keeps the first key out of the boot.')
    ap.add_argument('--settle', type=int, default=10, metavar='N',
                    help='chunks the screen must hold still before the next --adb-then key. '
                         'A wizard page that is still painting, or a file copy with a moving '
                         'progress bar, never settles -- which is exactly the interlock wanted.')
    ap.add_argument('--log', action='append', default=[], metavar='CAT=LEVEL',
                    help='turn on an emulator log category for the run, e.g. --log adb=2 to see '
                         'every key transition the keyboard queues and every packet autopoll '
                         'hands over. Repeatable.')
    ap.add_argument('--bp', action='append', default=[], metavar='ADDR[:COND]',
                    help='diagnostic breakpoint; every hit reports the exception and call '
                         'registers. COND is a shell expression, e.g. '
                         '--bp 0x300:"machine.cpu.dar == 0xEE315C98" to stop only on the one '
                         'data fault that matters. Repeatable.')
    ap.add_argument('--watch', action='append', default=[], metavar='ADDR',
                    help='log every 32-bit write to ADDR with the writing PC. Use it to find '
                         'who corrupts a stack slot (the NT PowerPC saved-TOC slot at 4(r1), '
                         'say). Repeatable.')
    ap.add_argument('--bp-space', default='logical', choices=('logical', 'physical'),
                    help='address space for --bp (the PowerPC exception vectors are physical)')
    a = ap.parse_args()
    bps = []
    for spec in a.bp:
        addr, _, cond = spec.partition(':')
        bps.append((int(addr, 0), cond))

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
    boot_start = len(s.out)                 # everything from here to the loop is the boot
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

    # a .gs double-quoted string takes C escapes, so a Windows path needs its backslashes doubled
    s('echo "=== wall 47: the boot file at {}, where it cannot overrun ==="'
      .format(a.osloader.replace('\\', '\\\\')))
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

    def arm_diagnostics(indent=''):
        for addr, cond in bps:
            s(f'{indent}debug.breakpoints.add {addr:#x} "{cond}" "{a.bp_space}"')

    for w in a.watch:
        s(f'debug.log "memory" 1')
        s(f'debug.logpoints.add addr={int(w, 0):#x} width=l mode=write level=1 '
          f'message="WATCH {int(w, 0):#x} <- ${{$value}} from pc=${{machine.cpu.pc}} '
          f'lr=${{machine.cpu.lr}} r1=${{machine.cpu.r1}}"')
        s('echo "=== inject an ARC environment at OSLOADER\'s first query, then let it run ==="')
    for spec in a.log:
        cat, _, lvl = spec.partition('=')
        s(f'debug.log "{cat}" {int(lvl or 1)}')
    s('debug.breakpoints.clear')
    s(f'debug.breakpoints.add {GETENV_ENTRY:#x}')
    arm_diagnostics()
    s('machine.scc.a.receive("go")')
    s('scheduler.run 6000000')
    s('machine.scc.a.receive("\\r")')
    if a.resume:
        # Replace the entire boot -- veneer patches, ARC environment, `go` -- with one load of a
        # checkpoint taken mid-wizard.  Nothing before the loop is reproducible state we need:
        # it is all in the checkpoint.
        s.out[boot_start:] = [f'checkpoint.load "{a.resume}"',
                              f'echo "=== resumed {a.resume} ==="',
                              'let junk = machine.scc.a.sent()']
    s('let out = ""')
    s('let i = 0')
    s(f'let done = {1 if a.resume else 0}')
    s('let sum = 0')                 # this chunk's screen checksum
    s('let seen = 0')                #   ... the last distinct one, and the one before it
    s('let seen2 = 0')
    s('let still = 0')               # chunks the screen has held those checksums
    s('let shot = 0')                # chunk of the last screenshot, to throttle a busy repaint
    s('let ki = 0')                  # how many --adb-then keys have gone in
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
    arm_diagnostics('            ')
    s('            echo "--- injected, array[0]=${machine.memory.peek.l(' + f'{VRENVP ^ 4:#x}'
      + ')} VrEnvc=${machine.memory.peek.l(' + f'{VRENVC ^ 4:#x}' + ')} ---"')
    s('        }')
    s('    }')
    for addr, _ in bps:
        s(f'    if machine.cpu.pc == {addr:#x} {{')
        s(f'        echo "BP {addr:#x} srr0=${{machine.cpu.srr0}} srr1=${{machine.cpu.srr1}}'
          ' dar=${machine.cpu.dar} dsisr=${machine.cpu.dsisr}"')
        s('        echo "   lr=${machine.cpu.lr} r1=${machine.cpu.r1} r3=${machine.cpu.r3}'
          ' r4=${machine.cpu.r4} r5=${machine.cpu.r5} r6=${machine.cpu.r6}"')
        s('    }')
    s('    $out = "${$out}${machine.scc.a.sent()}"')
    if a.screenshot:
        # machine.screen.save fails outright until the HAL has programmed the Cirrus, and a
        # failed statement aborts the script -- so gate every grab on the HAL saying it is up.
        #
        # Grab on *change*, not on a fixed stride: a wizard page is a still image that can sit
        # there for a thousand chunks, and the interesting frames are the transitions.  The same
        # checksum drives the keyboard below -- "the screen stopped moving" is the only signal
        # this rig has that Setup is waiting for a human.
        s('    if %s {' % ('true' if a.resume else 'contains($out, "54M30 console")'))
        # The visible region, NOT the whole framebuffer: screen.checksum() with no arguments
        # hashes stride*height, and the stride padding is off-screen video memory that NT's
        # display driver uses as scratch.  It churns while the picture is perfectly still, so
        # the no-argument form reports "changed" forever and the interlock below never fires.
        s('        $sum = try(machine.screen.checksum(0, 0, machine.screen.height, '
          'machine.screen.width), $sum)')
        # Two checksums count as "still", not one: a focused edit control blinks its caret, and
        # a screen alternating between exactly two pictures is a page waiting for input just as
        # much as a frozen one.  A progress bar walks through many distinct values and so still
        # reads as moving, which is what keeps a key out of a file copy.
        s('        if $sum == $seen || $sum == $seen2 {')
        s('            $still = $still + 1')
        s('        } else {')
        s('            $seen2 = $seen')
        s('            $seen = $sum')
        s('            $still = 0')
        s('            if $i - $shot >= 3 {')
        s('                $shot = $i')
        s(f'                machine.screen.save "{a.screenshot[:-4]}-c${{$i}}.png"')
        s('                echo "=== screen ${$sum} at chunk ${$i} ==="')
        s('            }')
        s('        }')
        if a.adb_then:
            s(f'        if $still == {a.settle} && $i > {a.keys_after} {{')
            for k, key in enumerate(a.adb_then):
                s(f'            if $ki == {k} {{')
                s(f'                echo "--- key {k}: {key} ---"')
                for line in adb_statements(key):
                    s(f'                {line}')
                s('            }')
            s('            $ki = $ki + 1')
            s('            $still = 0')
            s('        }')
            # Once the last key has gone in and the screen has stayed still for a long while,
            # there is nothing left to wait for: stop rather than burn the rest of the budget.
            s(f'        if $ki >= {len(a.adb_then)} && $still > {8 * a.settle} {{ break }}')
        s('    }')
    s('    if contains($out, "please reboot") { break }')
    s('    if contains($out, "0 > ") { break }')
    s('    if contains($out, "*** STOP") { break }')
    s('    $i = $i + 1')
    s('}')
    if a.save:
        s(f'checkpoint.save "{a.save}"')
        s(f'echo "=== saved {a.save} ==="')
    if a.screenshot:
        s('if %s {' % ('true' if a.resume else 'contains($out, "54M30 console")'))
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
