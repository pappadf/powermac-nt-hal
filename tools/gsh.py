#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 powermac-nt-hal contributors
"""gsh.py — send a shell script to a running Granny Smith headless daemon over TCP and print its reply.
   gsh.py 'cmd'            one line (or several, newline-separated)
   gsh.py < file           a script on stdin
   GS_PORT (6820), GS_TIMEOUT (first byte, 600 s), GS_IDLE (gap that ends the reply, 300 s).
Keep scripts under ~2 KB per connection; larger ones go in a file and are sent as `include "file"`."""
import os, socket, sys
port = int(os.environ.get('GS_PORT', '6820'))
cmds = sys.stdin.read() if len(sys.argv) < 2 else '\n'.join(sys.argv[1:])
s = socket.create_connection(('127.0.0.1', port), timeout=10)
s.sendall((cmds.rstrip('\n') + '\n').encode('latin1'))
s.settimeout(float(os.environ.get('GS_TIMEOUT', '600')))
buf = b''
while True:
    try: d = s.recv(65536)
    except socket.timeout: break
    if not d: break
    buf += d
    s.settimeout(float(os.environ.get('GS_IDLE', '300')))
sys.stdout.write(buf.decode('latin1'))
