#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
"""mkcoldboot.py — generate the script that boots Setup from a **cold machine** and a patched
CD, with no memory pokes and no breakpoints.

Every other rig here starts from a checkpoint taken at Setup's computer-type menu, which hides
how much of the setup is in the checkpoint rather than in any file: ledger row 3 turns out to be
baked into that checkpoint's RAM, not into an image.  This starts from power-on, so the only
thing that can make the boot work is what `mkveneer.py` and `mkoem.py` put in the files.

What it types at Open Firmware is only the four steps that are the firmware's own business:

    1. little-endian? true, real-mode? false, real-base/load-base, reset-all
    2. read the veneer off the staging disk with the ROM's pe-loader package
    3. point /chosen bootpath at the CD
    4. go

Stages 20, 25 and 27 of the hand-driven recipe -- the claim nops, the SETUPLDR path, the
ARC-name byte pokes -- are **absent on purpose**: they are in the veneer image now.  If this
reaches Setup's computer-type menu, the images are self-sufficient.

    mkcoldboot.py --rom ROM --cd patched.iso --staging staging.img --out tmp/cold.gs

Nothing from Microsoft or Apple is stored here: the ROM, the CD and the veneer are all the
user's own, and the generator only types firmware commands.
"""
import argparse, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkbootscript import adb_char_statements          # per-character down/wait/up on ADB

OF_STAGE_00 = ['setenv little-endian? true', 'setenv real-mode? false',
               'setenv real-base 3F00000', 'setenv load-base 3E00000']

# The pe-loader reads 512-byte blocks; the veneer is 0x27800 bytes = 0x13C blocks, taken 0x20 at
# a time because that is what the firmware's own transcript does and it is known to work.
def stage_10(block, total, dev='/bandit/53c825@12/sd@0,0'):
    out = ['dev /packages/pe-loader', '3D00000 27800 map-space', 'dev /', '0 value diskih',
           f'" {dev}" open-dev to diskih', 'diskih .']
    addr, blk, left = 0x3D00000, block, total
    while left > 0:
        n = min(0x20, left)
        out.append(f'{addr:X} {blk:X} {n:X} " read-blocks" diskih $call-method .')
        addr += 0x20 * 512; blk += n; left -= n
    out += ['diskih close-dev',
            'dev /packages/pe-loader', '3E00000 27800 map-space', 'dev /',
            '3D00000 3E00000 27800 move', '27800 to loadsize', 'init-program',
            'dev /packages/pe-loader', '4000 1000 map-space', 'dev /',
            '4000 do-translate .s', 'drop drop drop']
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom', required=True)
    ap.add_argument('--cd', required=True, help='the patched ISO (mkoem.py + the HAL)')
    ap.add_argument('--staging',
                    help='raw disk holding the patched veneer at --veneer-block. Optional: a '
                         'floppy can hold it instead (--floppy), which is the point of '
                         'docs/2026-09-15-the-boot-floppy.md')
    ap.add_argument('--floppy',
                    help='image to put in the internal drive before the machine is configured. '
                         "The insert survives `reset-all`, so it is done once, up front.")
    ap.add_argument('--veneer-block', type=lambda x: int(x, 0), default=0x800)
    ap.add_argument('--veneer-dev', default='/bandit/53c825@12/sd@0,0',
                    help='Open Firmware path of the disk holding the veneer. The second '
                         'controller by default, but one disk can serve as both the veneer source '
                         "and NT's install target -- the veneer sits at block 0x800 and NT's first "
                         'partition starts at 4096, so they never meet.')
    ap.add_argument('--cd-dev', default='/bandit/53c825@11/sd@0,0',
                    help='Open Firmware path of the CD, for /chosen bootpath')
    ap.add_argument('--veneer-blocks', type=lambda x: int(x, 0), default=0x13C)
    ap.add_argument('--out', default='-')
    ap.add_argument('--screenshot', default='tmp/hal/cold.png')
    ap.add_argument('--chunks', type=int, default=900)
    ap.add_argument('--console', choices=('serial', 'screen'), default='serial',
                    help="'serial' switches the console to ttya and types at the SCC, which is "
                         "what a script can read back as text. 'screen' leaves the console where "
                         "the firmware puts it -- the monitor -- and types on the ADB keyboard, "
                         "pacing on the picture settling instead of on prompt text. The machine "
                         "boots with the screen as its console either way; serial is our choice, "
                         "not the firmware's.")
    ap.add_argument('--model', default='ans500')
    ap.add_argument('--ram', type=int, default=65536)
    a = ap.parse_args()

    o = []
    def s(line=''): o.append(line)
    def type_screen(text):
        """Type one line on the ADB keyboard, one character at a time.

        NOT keyboard.type(): it paces its events 4 ms apart while Cuda auto-polls every 11 ms, so
        a character's press and release share one ADB packet and only the first is acted on --
        which silently drops characters (wall 57).  A dropped character in
        'setenv little-endian? true' is a boot that fails for no visible reason."""
        for ch in text:
            for line in adb_char_statements(ch):
                s(line)
        for line in adb_char_statements('\n'):
            s(line)
        s('let _s = settle(4, 400000000)')

    def type_of(text, spin=6000000):
        """Type one line at the firmware. The SCC receive FIFO holds about sixteen characters, so
        a long line goes in four at a time with the machine running in between -- and '$' is
        escaped, because an unescaped one is spliced by the shell as a binding ('$call-method'
        became '$ca' and killed a run)."""
        s('let drain = machine.scc.a.sent()')
        for k in range(0, len(text), 4):
            c = text[k:k+4].replace('\\', '\\\\').replace('"', '\\"').replace('$', '\\$')
            s(f'machine.scc.a.receive("{c}")'); s(f'scheduler.run {spin}')
        s('machine.scc.a.receive("\\r")'); s(f'scheduler.run {spin}')

    def emit(cmd, label=None):
        """Type one firmware line on whichever console this run uses."""
        if a.console == 'screen':
            s(f'echo "OF> {label or cmd}"'.replace('$', '\\$'))
            type_screen(cmd)
        else:
            type_of(cmd)
            s('echo "OF> %s -> ${wait_prompt(600000000)}"'
              % (label or cmd).replace('"', '\\"').replace('$', '\\$'))

    s('# Generated by tools/mkcoldboot.py — do not edit; regenerate.')
    s('# A cold boot of a patched CD: no pokes, no breakpoints, nothing from a checkpoint.')
    s('')
    # Wait for the PROMPT, not for " ok".  The firmware's own narration contains " ok" all over
    # the place, and -- the bug that cost the first cold boot -- so does whatever is still sitting
    # in the receive buffer from the previous command.  wait_ok returned instantly on that stale
    # text, so every line of stage 10 was typed into a machine that was still rebooting and went
    # nowhere.  Drain first, then wait for "0 > ".
    if a.console == 'screen':
        # No text to match on, so pace on the picture: type a line, wait until the visible region
        # stops changing.  The visible region, not the whole framebuffer -- screen.checksum() with
        # no arguments hashes the stride padding too, which is off-screen scratch that churns while
        # the picture is still.
        s('def settle(quiet, budget) {')
        s('    let seen = 0')
        s('    let still = 0')
        s('    let used = 0')
        s('    while $still < $quiet && $used < $budget {')
        s('        scheduler.run 20000000')
        s('        let sum = try(machine.screen.checksum(0, 0, machine.screen.height, '
          'machine.screen.width), $seen)')
        s('        if $sum == $seen {')
        s('            $still = $still + 1')
        s('        } else {')
        s('            $seen = $sum')
        s('            $still = 0')
        s('        }')
        s('        $used = $used + 20000000')
        s('    }')
        s('    return $still')
        s('}')
    s('def wait_prompt(budget) {')
    s('    let out = ""')
    s('    let used = 0')
    s('    while !contains($out, "0 > ") && $used < $budget {')
    s('        scheduler.run 20000000')
    s('        $out = "${$out}${machine.scc.a.sent()}"')
    s('        $used = $used + 20000000')
    s('    }')
    s('    scheduler.run 10000000')
    s('    $out = "${$out}${machine.scc.a.sent()}"')
    s('    return $out')
    s('}')
    s('')
    # --- power on -------------------------------------------------------------------------
    s('# A virgin non-volatile store, the way pulling the battery leaves one: POST caches its')
    s('# memory configuration there and a stale table hangs the machine at "Jumping To RAM Prog."')
    s('let cleared = try(machine.board.clear_nvram(), none)')
    s(f'machine.boot model="{a.model}" ram={a.ram} rom="{a.rom}"')
    s('let boot = ""')
    s('let spins = 0')
    s('while !contains($boot, "0 > ") && !contains($boot, "install-console>") && $spins < 24 {')
    s('    scheduler.run 250000000')
    s('    $boot = "${$boot}${machine.scc.a.sent()}"')
    s('    $spins = $spins + 1')
    s('}')
    s('let settle = 0')
    s('while !contains($boot, "0 > ") && $settle < 5 {')
    s('    scheduler.run 250000000')
    s('    $boot = "${$boot}${machine.scc.a.sent()}"')
    s('    $settle = $settle + 1')
    s('}')
    # The console may be the screen; Apple's documented terminal setup, typed on the machine's
    # own keyboard, then a power cycle so the setting takes (machine.boot would build a NEW
    # machine with a virgin store and come back on the screen again).
    s('if %s {' % ('false' if a.console == 'screen' else '!contains($boot, "0 > ")'))
    s('    let k1 = machine.adb.keyboard.type("setenv output-device ttya\\n")')
    s('    scheduler.run 400000000')
    s('    let k2 = machine.adb.keyboard.type("setenv input-device ttya\\n")')
    s('    scheduler.run 400000000')
    s('    let ignored = machine.scc.a.sent()')
    s('    machine.restart')
    s('    $boot = "${$boot}${wait_prompt(1500000000)}"')
    s('}')
    s('echo "=== firmware up ==="')
    s('echo "${$boot}"')
    s('')
    s(f'machine.scsi.attach_cdrom("{a.cd}", 0)')
    media = ['cd=${machine.scsi.device[0].type}']
    if a.staging:
        s(f'machine.scsi2.attach_hd("{a.staging}", 0)')
        media.append('staging=${machine.scsi2.device[0].type}')
    if a.floppy:
        s(f'let fdins = try(machine.floppy.drive[0].insert("{a.floppy}"), "ERR")')
        media.append('floppy=${$fdins}')
    s('echo "=== media: ' + ' '.join(media) + ' ==="')
    s('')
    s('echo "=== stage 00: the little-endian configuration reboot ==="')
    for c in OF_STAGE_00:
        emit(c)
    emit('reset-all')
    # The reboot re-narrates from POST; nothing may be typed until a fresh prompt appears.
    s('let rst = machine.scc.a.sent()')
    s('echo "=== reset-all: waiting for the machine to come back ==="')
    if a.console == 'screen':
        s('let _b = settle(20, 3000000000)')
    else:
        s('echo "${wait_prompt(2000000000)}"')
    s('')
    s('echo "=== stage 10: read the patched veneer off the staging disk ==="')
    for c in stage_10(a.veneer_block, a.veneer_blocks, a.veneer_dev):
        emit(c)
    s('echo "=== veneer laid out; check the image actually carries the patches ==="')
    # peek.l(A) reads the guest word at A^4 -- the 604's little-endian address munge.
    for va, want in ((0x514E0, '60000000 nop'), (0x51E3C, '60000000 nop'),
                     (0x52254, '39200000 li r9,0'), (0x54748, '60000000 nop'),
                     (0x53DB0, '39400000 li r10,0')):
        s(f'echo "  {va:#07x} = ${{machine.memory.peek.l({va ^ 4:#x})}}   want {want}"')
    s('echo "  0x5d0c0 = ${machine.memory.peek.b(0x5d0c7)}   want 0x0"')
    s('')
    s('echo "=== stage 26: point /chosen bootpath at the CD ==="')
    emit(f'" {a.cd_dev}" encode-string " bootpath" _chosen (property)', 'bootpath')
    s('')
    s('echo "=== stage 30: go — NO pokes applied, the images carry every patch ==="')
    if a.console == 'screen':
        type_screen('go')
    else:
        type_of('go')
    s('let out = ""')
    s('let i = 0')
    s(f'while $i < {a.chunks} {{')
    s('    scheduler.run 20000000')
    s('    $out = "${$out}${machine.scc.a.sent()}"')
    s('    if contains($out, "computer type") { break }')
    s('    if contains($out, "*** STOP") { break }')
    s('    if contains($out, "EXIT called") { break }')
    s('    $i = $i + 1')
    s('}')
    s('echo "=== chunks=${$i} ==="')
    s('echo "${$out}"')
    if a.screenshot:
        s(f'let shot = try(machine.screen.save("{a.screenshot}"), false)')
        s(f'echo "=== screenshot {a.screenshot} -> ${{$shot}} ==="')

    text = '\n'.join(o) + '\n'
    if a.out == '-':
        print(text)
    else:
        open(a.out, 'w').write(text)
        print(f'{a.out}: {len(o)} lines')


if __name__ == '__main__':
    main()
