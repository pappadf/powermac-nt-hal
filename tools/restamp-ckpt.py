#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
"""restamp-ckpt.py <gs-headless> <checkpoint.ckpt>...

A Granny Smith checkpoint carries the build ID (__DATE__ " " __TIME__, 20 bytes at offset 8)
of the binary that wrote it, and checkpoint.c refuses a mismatch.  Rebuilding the emulator
therefore invalidates the NT boot checkpoints, which cost ten minutes each to regenerate.
When the rebuild changed no checkpointed structure — a device's register semantics, say —
restamping the header is safe and saves the boot.  Verify that yourself before using it."""
import re, subprocess, sys

exe, ckpts = sys.argv[1], sys.argv[2:]
blob = open(exe, 'rb').read()
m = re.search(rb'[A-Z][a-z]{2} [ 0-9]\d \d{4} \d\d:\d\d:\d\d\x00', blob)
if not m: sys.exit('restamp-ckpt: no build id string found in ' + exe)
bid = m.group(0)[:20]
for path in ckpts:
    with open(path, 'r+b') as f:
        magic = f.read(8)
        if magic not in (b'GSCHKPT2', b'GSCHKPT3'): sys.exit(f'{path}: not a checkpoint ({magic!r})')
        old = f.read(20)
        if old == bid: print(f'{path}: already {bid.decode()}'); continue
        f.seek(8); f.write(bid)
        print(f'{path}: {old.decode()} -> {bid.decode()}')
