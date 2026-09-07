#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
"""gen-probe.py <ups> <name> <quiet_chunks> > file.gs
From the HAL-menu checkpoint: press Up <ups> times (one byte per call, verified
at SlGetChar's exit 0x806040d8), press Enter, then run in 50M-instruction chunks
until the firmware prompt / EXIT / a long silence, and dump CPU state."""
import sys
ups, name, quiet = int(sys.argv[1]), sys.argv[2], int(sys.argv[3])
out = []
w = out.append
w('checkpoint.load "tmp/nt-hal-menu.ckpt"')
w('let junk = machine.scc.a.sent()')
w('debug.breakpoints.clear')
w('debug.breakpoints.add 0x806040d8')
w(f'echo "=== probe {name}: {ups} x Up, Enter ==="')
for i in range(ups):
    w('machine.scc.a.receive("\x9b")')
    w('scheduler.run 30000000')
    w('machine.scc.a.receive("A")')
    w('scheduler.run 300000000')
    w(f'echo "key {i+1}: pc=${{machine.cpu.pc}} r3=${{machine.cpu.r3}} (expect 0x10000)"')
    w('scheduler.run 30000000')
    w('echo "${machine.scc.a.sent()}"')
w('machine.scc.a.receive("\\r")')
w('scheduler.run 300000000')
w('echo "enter: pc=${machine.cpu.pc} r3=${machine.cpu.r3} (expect 0xd)"')
w('debug.breakpoints.clear')
w('let acc = ""')
w('let i = 0')
w('let quiet = 0')
w('let lastlen = 0')
w('while $i < 400 {')
w('    scheduler.run 50000000')
w('    let chunk = machine.scc.a.sent()')
w('    $acc = "${$acc}${$chunk}"')
w('    if len($chunk) == 0 {')
w('        $quiet = $quiet + 1')
w('    } else {')
w('        $quiet = 0')
w('    }')
w('    if contains($acc, "EXIT called") { break }')
w('    if contains($acc, "0 > ") { break }')
w(f'    if $quiet >= {quiet} {{ break }}')
w('    $i = $i + 1')
w('}')
w('echo "=== output after Enter (chunks=${$i}, quiet=${$quiet}) ==="')
w('echo "${$acc}"')
w('echo "=== state: pc=${machine.cpu.pc} msr=${machine.cpu.msr} lr=${machine.cpu.lr} srr0=${machine.cpu.srr0} srr1=${machine.cpu.srr1} dar=${machine.cpu.dar} dsisr=${machine.cpu.dsisr} ==="')
w('echo "r1=${machine.cpu.r1} r2=${machine.cpu.r2} r3=${machine.cpu.r3} r4=${machine.cpu.r4} r5=${machine.cpu.r5} r13=${machine.cpu.r13} r31=${machine.cpu.r31}"')
w('let d = try(debug.disasm(machine.cpu.pc, 12), "disasm unavailable")')
w('echo "${$d}"')
w(f'checkpoint.save "tmp/nt-after-{name}.ckpt"')
w(f'echo "=== saved tmp/nt-after-{name}.ckpt ==="')
sys.stdout.buffer.write(('\n'.join(out) + '\n').encode('latin1'))
