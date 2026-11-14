/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * thunk.S — the NT PowerPC descriptor-call glue of src/thunk.S, for pmctrl.sys: every call into
 * VIDEOPRT.SYS, the kernel or the HAL goes through a { entry, toc } descriptor and a 64-byte frame the callee
 * may write into.  mkstubs.py's import stubs jump here with the descriptor address in r11. */
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
