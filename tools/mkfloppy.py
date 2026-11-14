#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
"""mkfloppy.py --iso <windows-nt-4.0-ppc.iso> — build the boot floppy, start to finish.

One command, one input you have to own.  It takes the one file the floppy still needs off your
CD, Microsoft's ARC veneer, patches it for the CD boot, and lays the image out with the HAL, the
ADB driver and the ARC firmware this repository builds.  `make floppy ISO=<path>` is this with
the compile in front of it.

WHAT COMES OFF YOUR CD, AND WHY IT MATTERS.  One of the eight files on the finished floppy is
Microsoft's -- the veneer, patched for the install boot -- and it is modified.  **A built floppy
image therefore cannot be redistributed.**  Nothing of theirs is stored in this repository; it is
read from the image you name on the command line, which is why this tool needs one.  See
PROVENANCE.md.  The installed system boots through this project's own firmware, src/arcfw
(docs/2026-11-05-the-arc-firmware.md), so the disk-patched veneer is gone since 5 November 2026.
SETUPLDR stays on the CD, where the veneer loads it; the Cirrus display pair stays on the CD too,
since the same day: the HAL translates the legacy VGA window a stock cirrus.sys asks for, so
Setup's own copy works and ledger row 6 is retired (docs/2026-11-05-the-vga-window.md).
"""
import argparse, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def run(argv, what):
    print(f'  {what}')
    r = subprocess.run(argv, cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit(f'{what}: failed')
    return r.stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--iso', help='ignored: nothing comes off the CD any more')
    ap.add_argument('--out', default='build/boot-floppy.img', help='the floppy to write')
    ap.add_argument('--hal', default='build/hal.dll')
    ap.add_argument('--adb-driver', default='build/adbport.sys')
    ap.add_argument('--mesh-driver', default='build/mesh.sys')
    ap.add_argument('--control-driver', default='build/pmctrl.sys')
    ap.add_argument('--arc-firmware', default='build/arcfw.xcf')
    a = ap.parse_args()

    for path, what in ((a.hal, 'the HAL'), (a.adb_driver, 'the ADB driver'), (a.mesh_driver, 'the MESH driver'),
                       (a.control_driver, 'the Control driver'),
                       (a.arc_firmware, 'the ARC firmware')):
        if not os.path.exists(os.path.join(ROOT, path)):
            raise SystemExit(f'{path} is missing -- run `make` first ({what})')

    if a.iso:
        print(f'  (--iso {a.iso} ignored: nothing comes off the CD any more)')

    print('Laying out the floppy:')
    out = run([sys.executable, 'tools/mkbootfloppy.py',
               '--out', a.out,
               '--arc-firmware', a.arc_firmware,
               '--hal', a.hal,
               '--adb-driver', a.adb_driver,
               '--mesh-driver', a.mesh_driver,
               '--control-driver', a.control_driver], 'mkbootfloppy')
    for line in out.splitlines():
        if 'ARCFW.XCF' in line or 'ARCFW.EXE' in line or 'bytes, FAT12' in line or 'redistributed' in line:
            print(f'  {line.strip()}')

    print(f'\n{a.out} is ready: this project\'s own firmware, HAL and drivers, and nothing else.')


if __name__ == '__main__':
    main()
