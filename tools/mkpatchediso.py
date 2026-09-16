#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
"""mkpatchediso.py <source.iso> <out.iso> --hal build/hal.dll — turn a user's NT 4.0 PowerPC CD
into one this HAL can boot and install from, in place.

Every edit is length-neutral and lands inside an extent the CD already has, so no ISO authoring
is involved and no directory record moves.  That is what makes this shape of tool portable: the
same five writes are what a browser would do to an ArrayBuffer.

    source.iso  +  build/hal.dll  ->  out.iso  +  a staging image holding the patched veneer

What goes in:

  * `TXTSETUP.SIF` and the `\\PPC` directory record, from `mkoem.py` — Setup's hardware menu
    gains "Apple Network Server 500/700" and `HALEAGLE.DLL` is renamed `HALSHINR.DLL` with our
    HAL's real length, which is what makes Setup's post-copy checksum agree.
  * `build/hal.dll` into that extent.
  * `\\PPC\\VENEER.EXE`, patched by `mkveneer.py` — ledger rows 1-5 as bytes rather than as
    instructions someone types at Open Firmware.
  * optionally a replacement `\\PPC\\I8042PRT.SYS`, which is how the ADB keyboard driver is
    delivered (SETUPLDR loads the keyboard driver by file name and checksums it over the length
    the ISO directory record gives, so the replacement must be padded and re-checksummed).

Nothing from Microsoft is stored in this repository: every byte written here comes either from
the user's own CD or from our own GPL HAL.  The keyboard driver is not ours to redistribute
either, which is why it is an option and not a default.

    mkpatchediso.py NT.iso tmp/nt-patched.iso --hal build/hal.dll \\
        --kbd tmp/i8042prt.bin --staging tmp/staging.img
"""
import argparse, os, shutil, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SECTOR = 2048


def read_dir(f, extent, size):
    """[(name, extent LBA, byte length, is_dir)] of one ISO 9660 directory."""
    f.seek(extent * SECTOR)
    data = f.read(size)
    out, off = [], 0
    while off < len(data):
        ln = data[off]
        if ln == 0:
            off = (off // SECTOR + 1) * SECTOR
            if off >= len(data):
                break
            continue
        e = data[off:off + ln]
        nl = e[32]
        name = e[33:33 + nl].decode('latin1').split(';')[0]
        out.append((name, int.from_bytes(e[2:6], 'little'),
                    int.from_bytes(e[10:14], 'little'), bool(e[25] & 2)))
        off += ln
    return out


def ppc_files(f):
    """The \\PPC directory's entries, by upper-case name."""
    f.seek(16 * SECTOR)
    pvd = f.read(SECTOR)
    if pvd[0] != 1 or pvd[1:6] != b'CD001':
        sys.exit('sector 16 is not a Primary Volume Descriptor — not an ISO 9660 image?')
    volume = pvd[40:72].decode('latin1').strip()
    root = pvd[156:190]
    ents = read_dir(f, int.from_bytes(root[2:6], 'little'),
                    int.from_bytes(root[10:14], 'little'))
    for name, lba, size, isdir in ents:
        if isdir and name.upper() == 'PPC':
            return volume, {n.upper(): (l, s) for n, l, s, d in read_dir(f, lba, size) if not d}
    sys.exit('no \\PPC directory — is this the PowerPC edition?')


def put(f, lba, data, limit, label):
    if len(data) > limit:
        sys.exit(f'{label}: {len(data)} bytes does not fit the {limit}-byte extent')
    f.seek(lba * SECTOR)
    f.write(data)
    print(f'  LBA {lba:<7} {len(data):>7} bytes  {label}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('source'); ap.add_argument('out')
    ap.add_argument('--hal', required=True)
    ap.add_argument('--veneer-out', default='', help='also write the patched VENEER.EXE here')
    ap.add_argument('--kbd', default='', help='replacement \\PPC\\I8042PRT.SYS (padded + checksummed)')
    ap.add_argument('--staging', default='', help='write a raw staging disk with the patched '
                                                  'veneer at --staging-block, which is what the '
                                                  "ROM's pe-loader reads")
    ap.add_argument('--staging-block', type=lambda x: int(x, 0), default=0x800)
    ap.add_argument('--staging-size', type=lambda x: int(x, 0), default=8 << 20)
    ap.add_argument('--veneer-into', default='',
                    help="write the patched veneer into an EXISTING disk image at "
                         "--staging-block, leaving the rest of it alone. The block the ROM's "
                         "pe-loader reads (0x800, 316 blocks) lies in the gap between the MBR and "
                         "a first partition at LBA 4096, so NT's own install target can carry the "
                         'veneer too and the machine needs only one disk.')
    a = ap.parse_args()

    print(f'{a.source} -> {a.out}')
    shutil.copyfile(a.source, a.out)
    f = open(a.out, 'r+b')
    volume, files = ppc_files(f)
    print(f'  volume {volume!r}, {len(files)} files in \\PPC')
    for need in ('VENEER.EXE', 'HALEAGLE.DLL', 'TXTSETUP.SIF'):
        if need not in files:
            sys.exit(f'\\PPC\\{need} is missing — this is not a CD these patches were made for')

    # --- the veneer, patched, into its own extent and (optionally) a staging disk -------------
    vl, vs = files['VENEER.EXE']
    vtmp = a.veneer_out or (a.out + '.veneer')
    # mkveneer.py wants a file; hand it the extent we just located, so the veneer that gets
    # patched is the one on this CD rather than one lying about on disk.
    raw = a.out + '.veneer-in'
    f.seek(vl * SECTOR); open(raw, 'wb').write(f.read(vs))
    subprocess.run([sys.executable, os.path.join(HERE, 'mkveneer.py'), raw,
                    '--out', vtmp, '--for', 'cd'], check=True)
    os.remove(raw)
    put(f, vl, open(vtmp, 'rb').read(), vs, '\\PPC\\VENEER.EXE (ledger rows 1-5)')

    # --- mkoem: the menu entry and the renamed HAL record -------------------------------------
    outdir = os.path.dirname(os.path.abspath(a.out)) or '.'
    r = subprocess.run([sys.executable, os.path.join(HERE, 'mkoem.py'), a.out, a.hal,
                        '--out-dir', outdir], check=True, capture_output=True, text=True)
    # mkoem reports each blob as "<path>  -> LBA <n>   (<what>)"; it is the authority on both.
    for line in r.stdout.splitlines():
        if '-> LBA' not in line:
            continue
        path, rest = line.split('-> LBA')
        lba = int(rest.split()[0])
        blob = open(path.strip(), 'rb').read()
        put(f, lba, blob, len(blob), os.path.basename(path.strip()))

    # --- the HAL, into the extent that record now names ---------------------------------------
    hl, hs = files['HALEAGLE.DLL']
    put(f, hl, open(a.hal, 'rb').read(), hs, 'build/hal.dll -> \\PPC\\HALSHINR.DLL')

    # --- the keyboard driver, if the user supplied one -----------------------------------------
    if a.kbd:
        kl, ks = files['I8042PRT.SYS']
        put(f, kl, open(a.kbd, 'rb').read(), ks, '\\PPC\\I8042PRT.SYS')

    f.close()
    print(f'{a.out}: written')

    if a.staging:
        ven = open(vtmp, 'rb').read()
        with open(a.staging, 'wb') as sf:
            sf.truncate(a.staging_size)
            sf.seek(a.staging_block * 512)
            sf.write(ven)
        print(f'{a.staging}: {a.staging_size} bytes, patched veneer at block '
              f'{a.staging_block:#x} ({len(ven)} bytes)')
    if a.veneer_into:
        ven = open(vtmp, 'rb').read()
        need = a.staging_block + (len(ven) + 511) // 512
        with open(a.veneer_into, 'r+b') as df:
            df.seek(0x1BE + 8)
            first = struct.unpack('<I', df.read(4))[0]
            if first and first < need:
                sys.exit(f'{a.veneer_into}: partition 1 starts at LBA {first}, but the veneer '
                         f'needs blocks {a.staging_block}..{need}. Re-create the disk with its '
                         f'first partition at 4096 or later (mkarcdisk.py --part 4096:...).')
            df.seek(a.staging_block * 512)
            df.write(ven)
        print(f'{a.veneer_into}: veneer at block {a.staging_block:#x} '
              f'(blocks {a.staging_block}..{need}; partition 1 at LBA {first})')

    if not a.veneer_out:
        os.remove(vtmp)


if __name__ == '__main__':
    main()
