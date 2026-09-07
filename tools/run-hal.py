#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
"""run-hal.py — boot the freshly built HAL on the emulated Network Server and capture ttya.

Starting point: a Granny Smith checkpoint taken at NT Setup's "Select the computer type" menu
with the veneer's OFClose fix applied and the *patched* ISO attached (tools/patch-iso.py puts
hal.dll in HALEAGLE.DLL's place; re-patch the same file before each run).  The run picks
MOTOROLA PowerStack, presses Enter at the
mass-storage screen, picks the Cirrus entry on the video menu, and runs until the console goes
quiet or the firmware prompt returns.  Every key goes one byte per call (the firmware drops the
rest of a burst).

    run-hal.py --ckpt tmp/nt-hal-menu-fixed.ckpt --iso tmp/nt-hal.iso --out tmp/run.log [--quiet 40]
Writes the generated shell script next to the log; talks to the daemon on GS_PORT (6820)."""
import argparse, glob, os, re, subprocess, sys

BPS = []      # breakpoint addresses, armed once after the checkpoint load
PEEKS = []
ONBREAK = []  # extra shell statements to run inside every breakpoint block
DEREFS = []   # `r1+2472`-style: follow the pointer stored there and dump 12 words

def bpcheck(indent='    '):
    """shell lines that report a breakpoint stop after a scheduler.run"""
    if not BPS: return []
    def addr(p):
        """a --peek argument: a KSEG0 address, or `r1+0x9bc` to follow the stack pointer"""
        return p if not p.lower().startswith('r') else 'machine.cpu.' + p.lower()
    # KSEG0 or a low physical address: masking bit 31 covers both, and the
    # little-endian munge on a word access is address ^ 4.
    peeks = ' '.join(f'[{p}]=${{machine.memory.peek.l((({addr(p)}) & 0x7fffffff) ^ 4)}}' for p in PEEKS)
    lines = [f'{indent}let bpc = machine.cpu.pc']
    for b in BPS:
        lines += [f'{indent}if $bpc == {b} {{', f'{indent}    $hits = $hits + 1',
                  f'{indent}    echo "=== BP {b} hit #${{$hits}}: r3=${{machine.cpu.r3}} r4=${{machine.cpu.r4}} r5=${{machine.cpu.r5}} r6=${{machine.cpu.r6}} r7=${{machine.cpu.r7}} r8=${{machine.cpu.r8}} r9=${{machine.cpu.r9}} r10=${{machine.cpu.r10}} r11=${{machine.cpu.r11}} r12=${{machine.cpu.r12}} lr=${{machine.cpu.lr}} r1=${{machine.cpu.r1}} msr=${{machine.cpu.msr}} {peeks}"',
                  f'{indent}    let dd = try(debug.disasm(machine.cpu.pc, 6), "n/a")', f'{indent}    echo "${{$dd}}"']
        for d in DEREFS:
            lines += [f'{indent}    let dp = machine.memory.peek.l((({addr(d)}) & 0x7fffffff) ^ 4)',
                      f'{indent}    let dj = 0', f'{indent}    let ds = ""',
                      f'{indent}    while $dj < 12 {{',
                      f'{indent}        $ds = "${{$ds}} ${{machine.memory.peek.l((($dp + $dj * 4) & 0x7fffffff) ^ 4)}}"',
                      f'{indent}        $dj = $dj + 1', f'{indent}    }}',
                      f'{indent}    echo "    *{d} = ${{$dp}}:${{$ds}}"']
        lines += [f'{indent}    {stmt}' for stmt in ONBREAK]
        lines += [f'{indent}}}']
    return lines

def run(out, n, indent=''):
    out.append(f'{indent}scheduler.run {n}')
    out += bpcheck(indent)

def keys(out, n_up):
    for _ in range(n_up):
        out.append('machine.scc.a.receive("\x9b")'); run(out, 30000000)
        out.append('machine.scc.a.receive("A")'); run(out, 300000000)
    out.append('machine.scc.a.receive("\\r")'); run(out, 300000000)

ADB_KEY = ['']   # a key tapped once per chunk of the kernel phase, to prove the ADB path

def run_until_quiet(out, quiet, chunks=800, tag='kernel'):
    out += [f'let acc_{tag} = ""', f'let i_{tag} = 0', f'let q_{tag} = 0', f'while $i_{tag} < {chunks} {{']
    if ADB_KEY[0] and tag == 'kernel':
        out.append(f'    let kp = try(machine.adb.keyboard.type("{ADB_KEY[0]}"), 0)')
    run(out, 50000000, '    ')
    out += [f'    let ch_{tag} = machine.scc.a.sent()', f'    $acc_{tag} = "${{$acc_{tag}}}${{$ch_{tag}}}"',
            f'    if len($ch_{tag}) == 0 {{', f'        $q_{tag} = $q_{tag} + 1', '    } else {', f'        $q_{tag} = 0', '    }',
            f'    if contains($acc_{tag}, "0 > ") {{ break }}', f'    if contains($acc_{tag}, "EXIT called") {{ break }}',
            f'    if contains($acc_{tag}, "*** STOP") {{ break }}',
            f'    if $q_{tag} >= {quiet} {{ break }}', f'    $i_{tag} = $i_{tag} + 1', '}',
            f'echo "=== {tag}: chunks=${{$i_{tag}}} quiet=${{$q_{tag}}} ==="', f'echo "${{$acc_{tag}}}"']


def delta_geometry(path):
    """(data_offset, block_count) of a Granny Smith copy-on-write delta.

    Layout: a 24-byte header, then two 1-bit-per-block bitmaps, then the blocks themselves, so
    size = 24 + 2 * ceil(blocks / 8) + blocks * 512 — solve it for the block count."""
    dsize = os.path.getsize(path)
    n = (dsize - 24) * 8 // (512 * 8 + 2)
    while 24 + 2 * ((n + 7) // 8) + n * 512 < dsize: n += 1
    data_off = 24 + 2 * ((n + 7) // 8)
    assert data_off + n * 512 == dsize, f'run-hal: unexpected delta geometry in {path}'
    return data_off, n

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ckpt', required=True); ap.add_argument('--iso', required=True); ap.add_argument('--out', required=True)
    ap.add_argument('--quiet', type=int, default=40); ap.add_argument('--cd-id', type=int, default=0)
    ap.add_argument('--save', default='')
    ap.add_argument('--screenshot', default='')
    ap.add_argument('--scsi-log', action='store_true', help='turn on the emulator SCSI log for the run')
    ap.add_argument('--delta-patch', action='append', default=[], help='file[@lba] to write into the restored CD scratch delta after checkpoint.load (default lba 98069 = \\PPC\\HALEAGLE.DLL on the OEM 000-48303 CD)')
    ap.add_argument('--computer-ups', type=int, default=5,
                    help='how many Up presses reach the computer-type entry to select, counting\n'
                         'from the menu default ("Other", last).  5 = MOTOROLA PowerStack on the\n'
                         'stock CD; 3 = the slot mkoem.py repurposes for the Network Server')
    ap.add_argument('--scratch-dir', default='/tmp/gs-image-ro')
    ap.add_argument('--disk-delta', action='append', default=[],
                    help='delta.file|dir=raw.img: splice a raw disk image into a writable '
                         "image's copy-on-write delta after checkpoint.load. A directory picks "
                         'the newest delta in it whose block count matches the image — '
                         'checkpoint.load creates a fresh delta for every writable image and '
                         'seeds it from the checkpoint, so the base image on disk is never read')
    ap.add_argument('--poke', action='append', default=[], help='addr=value: poke a 32-bit word after the checkpoint load (address already LE-munged, i.e. image^4)')
    ap.add_argument('--bp', action='append', default=[], help='virtual address to break at during the kernel phase (repeatable)')
    ap.add_argument('--peek', action='append', default=[], help='KSEG0 virtual address (or r1+0x9bc) whose LE word to print at each break')
    ap.add_argument('--adb-then', action='append', default=[], help='after the kernel settles, type this on the ADB keyboard and keep running (repeatable)')
    ap.add_argument('--adb-key', default='', help='tap this ADB key once per kernel-phase chunk')
    ap.add_argument('--onbreak', action='append', default=[], help='shell statement to run at every breakpoint (repeatable)')
    ap.add_argument('--deref', action='append', default=[], help='address holding a pointer; dump 12 words from the target at each break')
    a = ap.parse_args()
    # The checkpoint already has the ISO attached (by path); the ISO was patched in place, so the
    # emulator reads the new HAL when SETUPLDR opens HALEAGLE.DLL after the menu selection.
    BPS[:] = a.bp; PEEKS[:] = a.peek; DEREFS[:] = a.deref; ONBREAK[:] = a.onbreak; ADB_KEY[0] = a.adb_key
    out = [f'checkpoint.load "{a.ckpt}"', 'let junk = machine.scc.a.sent()', f'echo "=== iso {a.iso} ==="', 'let hits = 0']
    for spec in a.poke:
        addr, _, val = spec.partition('=')
        out.append(f'echo "=== poke {addr} was ${{machine.memory.peek.l({addr})}} -> {val} ==="')
        out.append(f'machine.memory.poke.l {addr} {val}')
    if a.scsi_log: out.append('debug.log "scsi" 10')
    if a.bp:
        out.append('debug.breakpoints.clear')
        for b in a.bp: out.append(f'debug.breakpoints.add {b}')
        out.append('echo "=== armed ${len(debug.breakpoints.entries)} breakpoints ==="')
    keys(out, a.computer_ups)       # Other -> the computer type this HAL is registered as
    run_until_quiet(out, 20, 400, 'files')
    keys(out, 0)                    # Enter at the mass-storage screen
    run_until_quiet(out, 20, 400, 'video')
    keys(out, 2)                    # Other -> Motorola Power Stack (cirrus 54xx)
    run_until_quiet(out, a.quiet, 1200, 'kernel')

    def shoot(tag):
        if not a.screenshot: return
        shot = a.screenshot.replace('.png', f'-{tag}.png')
        out.append(f'machine.screen.save "{shot}"')
        out.append(f'echo "=== screenshot {shot} checksum ${{machine.screen.checksum()}} ==="')

    if a.adb_then:
        shoot('00')                       # the screen Setup settled on before any key
        quiet = max(6, a.quiet // 4)
        for i, key in enumerate(a.adb_then, 1):
            # Setup takes its input from the ADB keyboard now, not ttya.  A '#' prefix sends a
            # raw ADB key code (F8 = 100, Page Down = 121), anything else is typed as text.
            if key.startswith('#'):
                # press() sends down and up back to back, so both land in one ADB packet and
                # Setup sees nothing; separate them in guest time the way type() does.
                out.append(f'let kd{i} = try(machine.adb.keyboard.down({key[1:]}), false)')
                out.append('scheduler.run 3000000')
                out.append(f'let ku{i} = try(machine.adb.keyboard.up({key[1:]}), false)')
            else:
                out.append(f'let k{i} = try(machine.adb.keyboard.type("{key}"), 0)')
            run_until_quiet(out, quiet, 400, f'adb{i}')
            shoot(f'{i:02d}')
    out.append('echo "=== state: pc=${machine.cpu.pc} msr=${machine.cpu.msr} lr=${machine.cpu.lr} srr0=${machine.cpu.srr0} srr1=${machine.cpu.srr1} dar=${machine.cpu.dar} dsisr=${machine.cpu.dsisr} ==="')
    out.append('echo "r1=${machine.cpu.r1} r2=${machine.cpu.r2} r3=${machine.cpu.r3} r4=${machine.cpu.r4} r5=${machine.cpu.r5} r6=${machine.cpu.r6} r7=${machine.cpu.r7} r11=${machine.cpu.r11} r12=${machine.cpu.r12} r31=${machine.cpu.r31}"')
    out.append('let d = try(debug.disasm(machine.cpu.pc, 12), "n/a")'); out.append('echo "${$d}"')
    if a.screenshot: out.append(f'machine.screen.save "{a.screenshot}"'); out.append(f'echo "=== screenshot {a.screenshot} checksum ${{machine.screen.checksum()}} ==="')
    if a.save: out.append(f'checkpoint.save "{a.save}"')
    script = a.out + '.gs'
    env = dict(os.environ, GS_TIMEOUT=os.environ.get('GS_TIMEOUT', '1800'), GS_IDLE=os.environ.get('GS_IDLE', '1800'))
    here = os.path.dirname(os.path.abspath(__file__))
    gsh = os.path.join(here, 'gsh.py')
    log = open(a.out, 'w')
    if a.delta_patch or a.disk_delta:
        # load first, then overwrite the restored scratch delta (512-byte blocks after a 24-byte
        # header and two bitmaps), then run the rest without reloading
        subprocess.run([sys.executable, gsh, out[0]], stdout=log, stderr=subprocess.STDOUT, env=env)
        deltas = sorted(glob.glob(os.path.join(a.scratch_dir, '*.delta')), key=os.path.getmtime)
        if not deltas: sys.exit('run-hal: no scratch delta found after checkpoint.load')
        dpath = deltas[-1]
        with open(dpath, 'r+b') as df:
            data_off, n = delta_geometry(dpath)
            for spec in a.delta_patch:
                path, _, lba = spec.partition('@')
                lba = int(lba) if lba else 98069
                data = open(path, 'rb').read()
                df.seek(data_off + lba * 2048)
                df.write(data)
                log.write(f'=== delta patched: {dpath} lba {lba} <- {path} ({len(data)} bytes) ===\n')
        for spec in a.disk_delta:
            dp, _, raw = spec.partition('=')
            want = (os.path.getsize(raw) + 511) // 512
            if os.path.isdir(dp):
                # checkpoint.load makes a *fresh* delta for every writable image (this checkpoint
                # is consolidated: it carries the disk inline and writes every block out again),
                # so the one to patch is the newest in the directory whose geometry matches.
                cands = [f for f in glob.glob(os.path.join(dp, '*.delta'))
                         if delta_geometry(f)[1] == want]
                if not cands: sys.exit(f'run-hal: no {want}-block delta in {dp}')
                dp = max(cands, key=os.path.getmtime)
            doff, blocks = delta_geometry(dp)
            image = open(raw, 'rb').read()
            with open(dp, 'r+b') as df:
                written = 0
                for lba in range(min(blocks, (len(image) + 511) // 512)):
                    block = image[lba * 512:(lba + 1) * 512]
                    if not any(block): continue
                    df.seek(doff + lba * 512)
                    df.write(block.ljust(512, b'\0'))
                    written += 1
            log.write(f'=== disk delta {dp} <- {raw}: {written} non-empty sectors ===\n')
        log.flush()
        out = out[1:]
    open(script, 'wb').write(('\n'.join(out) + '\n').encode('latin1'))
    subprocess.run([sys.executable, gsh, f'include "{script}"'], stdout=log, stderr=subprocess.STDOUT, env=env)
    log.close()
    text = open(a.out, encoding='latin1').read()
    text = re.sub(r'\x1b\[[0-9;]*[A-Za-z@]', '', text)
    text = re.sub(r'\\x9B[0-9;]*[A-Za-z@]', ' ', text)
    text = re.sub(r'^# running.*$', '', text, flags=re.M)
    text = re.sub(r'^\[scsi\].*$', '', text, flags=re.M)
    text = re.sub(r'(\\xCD){3,}', '=', text)
    text = re.sub(r' {3,}', '  ', text)
    text = re.sub(r'\n{3,}', '\n\n', text)
    open(a.out + '.txt', 'w').write(text)
    i = text.find('=== kernel:')
    print(text[i:] if i >= 0 else text[-6000:])

if __name__ == '__main__':
    main()
