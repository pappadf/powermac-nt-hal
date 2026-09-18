/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * thunk.S — the same NT PowerPC descriptor-call glue as the HAL's src/thunk.S, for a driver.
 *
 * Every call out of this module -- into the kernel, the HAL, or kbdclass's class service --
 * goes through a { entry, toc } descriptor and a 64-byte frame the callee may write into at
 * offsets 4, 8 and 24..55.  Our SVR4-compiled code never looks at that frame, which is the whole
 * point.  mkstubs.py's import stubs jump here with the descriptor address in r11. */
	.text
	.p2align 2

	.globl hal_import_call
	.type hal_import_call,@function
hal_import_call:                /* r11 = descriptor address, r3..r10 = arguments */
	mflr    0
	stwu    1, -64(1)
	stw     0, 60(1)
	lwz     0, 0(11)
	mtctr   0
	lwz     2, 4(11)
	bctrl
	lwz     0, 60(1)
	addi    1, 1, 64
	mtlr    0
	blr

	/* ULONG AdbCallDesc4(PVOID desc, ULONG a1, ULONG a2, ULONG a3, ULONG a4) */
	.globl AdbCallDesc4
	.type AdbCallDesc4,@function
AdbCallDesc4:
	mr      11, 3
	mr      3, 4
	mr      4, 5
	mr      5, 6
	mr      6, 7
	b       hal_import_call
