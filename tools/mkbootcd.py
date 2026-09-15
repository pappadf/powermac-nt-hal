#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
"""mkbootcd.py — PROTOTYPE. One CD that boots NT Setup on an Apple Network Server with as
little typing as the firmware allows, and no second image.

This supersedes the staging-disk arrangement.  Open Firmware can load the veneer **off the CD by
name**, through Apple's `disk-label` interposing the ISO-9660 filesystem package:

    load /bandit/53c825@11/sd@0,0:,\\PPC\\VENEER.EXE
    init-program                      ->  Loading PE/COFF, image_base 50000

which replaces twenty-eight lines of `pe-loader` / `read-blocks` and the 8 MB disk they read
from.  (Raw `read-blocks` against the CD fails with `bad address to DMA-MAP-IN`; the filesystem
route is the one that works, and it is the same interposition ledger row 3 blanks `":0"` to
*avoid* when it wants raw sectors.)

What the disc carries, all written in place, no ISO authoring, nothing moved:

  * `\\PPC\\VENEER.EXE`  — patched by `mkveneer.py` (ledger rows 1-5), so no pokes are needed
  * `\\PPC\\HALSHINR.DLL` — our HAL, with `TXTSETUP.SIF` and the `\\PPC` directory record from
    `mkoem.py` giving it a name and a menu entry of its own
  * `\\PPC\\I8042PRT.SYS` — optionally the ADB keyboard driver (not ours to redistribute)
  * a small Open Firmware **boot script**, written over a spare extent, holding the whole
    sequence so a human types one word instead of thirty-five lines

## What cannot be moved onto the disc

`little-endian? true` is a firmware NVRAM setting and `reset-all` is what applies it.  No disc
can set it before the firmware has read the disc, so the first boot always costs the user that
one configuration step.  After it, the machine stays little-endian until someone changes it
back, and the disc is a single `boot` command.

That is the honest floor: **one-time setup, then one command.**  Anything claiming zero typing on
a virgin machine is claiming the CD can reach into NVRAM before it is read.

## Status — what is proven and what is not

**Proven.** Open Firmware loads the veneer off the CD by name, and lays it out:
`load <dev>:,\\PPC\\VENEER.EXE` then `init-program` prints `Loading PE/COFF, image_base 50000`
and 0x50000 holds the veneer. That alone retires the 8 MB staging disk and twenty-eight lines of
`pe-loader` / `read-blocks`. The firmware also has `nvramrc` and `use-nvramrc?` (false by
default) alongside `auto-boot?` and `boot-command`, so parking phase 2 in NVRAM -- the thing that
would make this disc insert-and-go -- is available rather than wishful.

**Not proven.** `go` after the CD load faults: `DEFAULT CATCH! code=FFF00600` (alignment) at
`SRR0 00050014`, with `SRR1 00003071` where a healthy boot shows `0001B071`. The LE bits are
clear, so the CPU enters the veneer big-endian and reads its little-endian instructions as
rubbish. Adding the working path's `4000 1000 map-space` / `4000 do-translate` step did not
change it. The difference that remains is that the working path does
`3E00000 27800 map-space` **inside the pe-loader's package** before moving the image in and
calling `init-program`, whereas `load` writes to load-base without that mapping ever being
established. That is the next thing to try, and until it works the route that boots is still
`mkcoldboot.py`'s.

Prototype.  The in-place patching and the `load`/`init-program` route are tested; the boot
script written to a spare extent is **not** yet executed by the firmware automatically -- that
needs an Apple Partition Map wrapped around the ISO so Open Firmware finds a blessed `tbxi`
file, which is real ISO authoring and the one place the "never change a length" discipline has
to give.  Until then the script is a file the user can `load` and evaluate by hand.
"""
import argparse, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SECTOR = 2048
CD_DEV = '/bandit/53c825@11/sd@0,0'

# Phase 2 is what has to happen on every boot.  Four words, not one -- which is why it cannot
# live in `boot-command` and goes into `nvramrc` instead.
#
# `boot <path>` looked like it would do the lot (load + init-program + go) and it does load the
# image, but it sets /chosen bootpath to the *file* it booted.  The veneer's find_boot_dev reads
# that property expecting a device, so the boot dies with `can't OPEN` a moment later.  Setting
# bootpath to the bare device between the load and init-program is the whole difference.
def phase2(dev=CD_DEV):
    return [f'load {dev}:,\\PPC\\VENEER.EXE',
            f'" {dev}" encode-string " bootpath" _chosen (property)',
            'init-program',
            'go']

# Phase 1 is the one-time firmware configuration.  little-endian? is a NVRAM variable and
# reset-all is what applies it, so no disc can set it before the firmware has read the disc --
# this step exists once per machine and cannot be moved onto the media.
def phase1(dev=CD_DEV):
    """The one-time firmware configuration.  Pure `setenv` plus `reset-all`, deliberately:

    * no `load`, because this script is evaluated out of **load-base** and `load` writes there --
      a script that loads anything overwrites the text the interpreter is still reading;
    * no nested `"` strings, because the phase-2 line contains `"` of its own and embedding it
      would terminate the outer string early.

    Both of those are why phase 2 is not in here.  See `of_script`."""
    return ['setenv little-endian? true', 'setenv real-mode? false',
            'setenv real-base 3F00000', 'setenv load-base 3E00000',
            'reset-all']


def of_script(dev=CD_DEV, name='BOOT.OF'):
    """The Forth this disc carries as \\PPC\\BOOT.OF, to be loaded and evaluated once:

        load <dev>:,\\PPC\\BOOT.OF
        load-base loadsize evaluate

    It configures the machine and reboots it little-endian.  That is all it does, and the two
    reasons are in `phase1`.

    **Not yet automatic.**  The obvious next step is to park phase 2 in `nvramrc` with
    `use-nvramrc? true` so the firmware runs it at every power-on and the disc becomes
    insert-and-go.  Two things block a scripted version of that and neither is hand-waving:
    Apple's `nvedit` is an interactive line editor with no scriptable "set nvramrc to this text"
    word, and phase 2 begins with `load`, which would clobber load-base.  Copying the script out
    of load-base before evaluating it solves the second; the first wants a different mechanism
    (an Apple Partition Map with a blessed `tbxi`, which is real ISO authoring).
    """
    return '\n'.join([
        '\\ Granny Smith / powermac-nt-hal - Windows NT 4.0 on the Apple Network Server.',
        '\\ At the "0 >" prompt:',
        f'\\     load {dev}:,\\PPC\\BOOT.OF',
        '\\     load-base loadsize evaluate',
        '',
        '." powermac-nt-hal: configuring this machine for Windows NT" cr',
        *phase1(dev),
        ''])


def read_dir(f, extent, size):
    f.seek(extent * SECTOR); data = f.read(size)
    out, off = [], 0
    while off < len(data):
        ln = data[off]
        if ln == 0:
            off = (off // SECTOR + 1) * SECTOR
            if off >= len(data): break
            continue
        e = data[off:off + ln]; nl = e[32]
        out.append((e[33:33 + nl].decode('latin1').split(';')[0],
                    int.from_bytes(e[2:6], 'little'),
                    int.from_bytes(e[10:14], 'little'), bool(e[25] & 2)))
        off += ln
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('source'); ap.add_argument('out')
    ap.add_argument('--hal', required=True)
    ap.add_argument('--kbd', default='')
    ap.add_argument('--script-over', default='PINBALL.HL_',
                    help="\\PPC file whose extent the boot script is written over. Adding a file "
                         'would mean rebuilding the ISO, so the script goes over one nothing in '
                         'the boot or install path reads. The default is Pinball\'s help text: '
                         'the whole cost of this prototype is that Pinball has no help if you '
                         'install Games.')
    ap.add_argument('--dev', default=CD_DEV, help='Open Firmware path of the CD drive')
    ap.add_argument('--print-only', action='store_true',
                    help='just print the firmware sequence and exit')
    a = ap.parse_args()

    if a.print_only:
        print(of_script(a.dev)); return

    # Everything mkpatchediso.py already does, minus the staging disk it no longer needs.
    subprocess.run([sys.executable, os.path.join(HERE, 'mkpatchediso.py'), a.source, a.out,
                    '--hal', a.hal] + (['--kbd', a.kbd] if a.kbd else []), check=True)

    f = open(a.out, 'r+b')
    f.seek(16 * SECTOR); pvd = f.read(SECTOR)
    root = pvd[156:190]
    ppc = None
    for n, l, s, d in read_dir(f, int.from_bytes(root[2:6], 'little'),
                               int.from_bytes(root[10:14], 'little')):
        if d and n.upper() == 'PPC':
            ppc = {nn.upper(): (ll, ss) for nn, ll, ss, dd in read_dir(f, l, s) if not dd}
    if ppc is None:
        sys.exit('no \\PPC directory')

    target = a.script_over.upper()
    if target not in ppc:
        sys.exit(f'\\PPC\\{target} is not on this disc; pick another --script-over')
    lba, size = ppc[target]
    body = of_script(a.dev, target).encode('ascii', 'replace')
    if len(body) > size:
        sys.exit(f'the boot script is {len(body)} bytes and \\PPC\\{target} is {size}')
    f.seek(lba * SECTOR); f.write(body.ljust(size, b'\n'))
    f.close()
    print(f'  LBA {lba:<7} {len(body):>7} bytes  Open Firmware boot script over \\PPC\\{target}')
    print(f'{a.out}: written\n')
    print('ONCE per machine, at the "0 >" prompt — configures it and reboots little-endian:')
    print(f'    load {a.dev}:,\\PPC\\{target}')
    print('    load-base loadsize evaluate')
    print('\nThen, after it comes back — four words that boot NT Setup off this disc:')
    for c in phase2(a.dev):
        print(f'    {c}')
    print('\n(Six typed lines, once, against thirty-five before. Making the second group')
    print(' automatic needs nvramrc or a blessed tbxi — see of_script.)')


if __name__ == '__main__':
    main()
