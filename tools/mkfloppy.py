#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
"""mkfloppy.py --iso <windows-nt-4.0-ppc.iso> — build the boot floppy, start to finish.

One command, one input you have to own.  It takes the four files the floppy needs off your CD,
patches the two veneers, and lays the image out with the HAL and the ADB driver this repository
builds.  `make floppy ISO=<path>` is this with the compile in front of it.

WHAT COMES OFF YOUR CD, AND WHY IT MATTERS.  Five of the eleven files on the finished floppy are
Microsoft's -- the two veneers, SETUPLDR, and the Cirrus driver pair -- and three of those are
modified.  **A built floppy image therefore cannot be redistributed.**  Nothing of theirs is
stored in this repository; every one of them is read from the image you name on the command
line, which is why this tool needs one.  See PROVENANCE.md.
"""
import argparse, os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import isocat  # noqa: E402

# (member on the CD, what we call it, what it is)
NEEDED = [
    (r'\PPC\VENEER.EXE', 'veneer.exe', "the ARC firmware veneer"),
    (r'\PPC\SETUPLDR', 'SETUPLDR', "NT's text-mode Setup loader"),
    (r'\PPC\CIRRUS.SYS', 'cirrus.sys', 'the display miniport'),
    (r'\PPC\CIRRUS.DLL', 'cirrus.dll', 'the display driver'),
]


def run(argv, what):
    print(f'  {what}')
    r = subprocess.run(argv, cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit(f'{what}: failed')
    return r.stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--iso', required=True, help='your Windows NT 4.0 PowerPC CD image')
    ap.add_argument('--out', default='build/boot-floppy.img', help='the floppy to write')
    ap.add_argument('--work', default='build/oemsrc', help='where the extracted files land')
    ap.add_argument('--hal', default='build/hal.dll')
    ap.add_argument('--adb-driver', default='build/adbport.sys')
    ap.add_argument('--vga-aperture', default='0x90000000',
                    help='where the display miniport is told its legacy VGA range lives '
                         '(ledger row 6); 0xA0000 is unreachable across Bandit')
    a = ap.parse_args()

    for path, what in ((a.hal, 'the HAL'), (a.adb_driver, 'the ADB driver')):
        if not os.path.exists(os.path.join(ROOT, path)):
            raise SystemExit(f'{path} is missing -- run `make` first ({what})')

    work = os.path.join(ROOT, a.work)
    os.makedirs(work, exist_ok=True)

    print(f'Taking four files off {a.iso}:')
    for member, name, what in NEEDED:
        blob = isocat.read(a.iso, member)
        with open(os.path.join(work, name), 'wb') as g:
            g.write(blob)
        print(f'  {name:12} {len(blob):8} bytes   {what}')

    # Two veneers, patched differently.  The CD one carries the VrOpen patch (ledger row 4); the
    # disk one deliberately does not, because that patch is wrong for an MBR disk (wall 46).
    print('Patching the veneer, twice:')
    src = os.path.join(work, 'veneer.exe')
    for kind, out in (('cd', 'veneer-fd.exe'), ('disk', 'veneer-disk.exe')):
        run([sys.executable, 'tools/mkveneer.py', src, '--out', os.path.join(work, out),
             '--for', kind], f'for the {kind}: {out}')

    print('Laying out the floppy:')
    out = run([sys.executable, 'tools/mkbootfloppy.py',
               '--out', a.out,
               '--veneer', os.path.join(work, 'veneer-fd.exe'),
               '--disk-veneer', os.path.join(work, 'veneer-disk.exe'),
               '--setupldr', os.path.join(work, 'SETUPLDR'),
               '--hal', a.hal,
               '--adb-driver', a.adb_driver,
               '--display-driver', os.path.join(work, 'cirrus.sys'),
               '--display-dll', os.path.join(work, 'cirrus.dll'),
               '--vga-aperture', a.vga_aperture], 'mkbootfloppy')
    for line in out.splitlines():
        if 'VENEER.EXE' in line or 'bytes, FAT12' in line:
            print(f'  {line.strip()}')

    print(f'\n{a.out} is ready.')
    print('Do not redistribute it: five of its files are Microsoft\'s, three of them modified.')


if __name__ == '__main__':
    main()
