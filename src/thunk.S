/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * UPSTREAM: the descriptor-call idea and the shape of the cache loops come from reading
 *   Wack0/entii-for-workcubes, halartx/source/cache.s (GPL-2.0, commit c9b041da), with
 *   the cache operations themselves from the MPC604 User's Manual.  Rewritten from
 *   reading; no lines copied.  See PROVENANCE.md.
 *
 * thunk.S — the NT PowerPC ABI glue: descriptor calls into the kernel, and small SPR/cache helpers.
 *
 * The kernel is called through a two-word descriptor { entry, toc }.  An NT callee may write the
 * caller's frame at offsets 4 and 8 (glue save slots) and 24..55 (parameter home area), so every
 * call into the kernel goes through a 64-byte frame that our SVR4-compiled code never looks at. */
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

	/* ULONG HalpCallDesc4(PVOID desc, ULONG a1, ULONG a2, ULONG a3, ULONG a4) */
	.globl HalpCallDesc4
	.type HalpCallDesc4,@function
HalpCallDesc4:
	mr      11, 3
	mr      3, 4
	mr      4, 5
	mr      5, 6
	mr      6, 7
	b       hal_import_call

	/* ULONG HalpCallDesc3(PVOID desc, ULONG a1, ULONG a2, ULONG a3) */
	.globl HalpCallDesc3
	.type HalpCallDesc3,@function
HalpCallDesc3:
	mr      11, 3
	mr      3, 4
	mr      4, 5
	mr      5, 6
	b       hal_import_call

	.globl HalpReadMsr
HalpReadMsr:
	mfmsr   3
	blr
	.globl HalpWriteMsr
HalpWriteMsr:
	mtmsr   3
	isync
	blr
	.globl HalpDisableInterrupts
HalpDisableInterrupts:
	mfmsr   3
	rlwinm  4, 3, 0, 17, 15        /* clear MSR[EE] (bit 16) */
	mtmsr   4
	isync
	blr
	.globl HalpEnableInterrupts
HalpEnableInterrupts:
	mfmsr   3
	ori     3, 3, 0x8000
	mtmsr   3
	isync
	blr
	.globl HalpReadDec
HalpReadDec:
	mfdec   3
	blr
	.globl HalpWriteDec
HalpWriteDec:
	mtdec   3
	blr
	.globl HalpReadPvr
HalpReadPvr:
	mfpvr   3
	blr
	.globl HalpReadTimebaseLow
HalpReadTimebaseLow:
	.long   0x7C6C42E6              /* mftb r3 (opcode 371, TBR 268): LLVM would emit mfspr 268, illegal on the 604 */
	blr
	/* VOID HalpReadTimebase(PULARGE_INTEGER out) */
	.globl HalpReadTimebase
HalpReadTimebase:
1:	.long   0x7C8D42E6              /* mftbu r4 */
	.long   0x7CAC42E6              /* mftb  r5 */
	.long   0x7CCD42E6              /* mftbu r6 */
	cmplw   4, 6
	bne     1b
	stw     5, 0(3)
	stw     4, 4(3)
	blr

	/* VOID KeFlushWriteBuffer(VOID) */
	.globl KeFlushWriteBuffer
	.type KeFlushWriteBuffer,@function
KeFlushWriteBuffer:
	eieio
	sync
	blr

	/* VOID HalProcessorIdle(VOID): enable interrupts and wait */
	.globl HalProcessorIdle
	.type HalProcessorIdle,@function
HalProcessorIdle:
	mfmsr   3
	ori     3, 3, 0x8000
	mtmsr   3
	isync
	blr

/* ---- caches: 604 has 16 KB 4-way data and instruction caches with 32-byte lines ------------- */
	.set LINE, 32
	.set DCACHE_LINES, (16*1024)/32

	/* VOID HalSweepDcache(VOID): flush the whole data cache by touching 16 KB of KSEG0 */
	.globl HalSweepDcache
	.type HalSweepDcache,@function
HalSweepDcache:
	lis     3, 0x8000
	li      4, DCACHE_LINES
	mtctr   4
	mr      5, 3
1:	dcbf    0, 5
	addi    5, 5, LINE
	bdnz    1b
	sync
	blr

	/* VOID HalSweepDcacheRange(PVOID Address, ULONG Length) */
	.globl HalSweepDcacheRange
	.type HalSweepDcacheRange,@function
HalSweepDcacheRange:
	cmplwi  4, 0
	beqlr
	rlwinm  5, 3, 0, 27, 31        /* offset within line */
	add     4, 4, 5
	addi    4, 4, LINE-1
	srwi    4, 4, 5
	mtctr   4
1:	dcbf    0, 3
	addi    3, 3, LINE
	bdnz    1b
	sync
	blr

	/* VOID HalSweepIcache(VOID): flash-invalidate via HID0[ICFI] */
	.globl HalSweepIcache
	.type HalSweepIcache,@function
HalSweepIcache:
	mfspr   3, 1008
	ori     4, 3, 0x0800
	isync
	mtspr   1008, 4
	isync
	mtspr   1008, 3
	isync
	blr

	/* VOID HalSweepIcacheRange(PVOID Address, ULONG Length) */
	.globl HalSweepIcacheRange
	.type HalSweepIcacheRange,@function
HalSweepIcacheRange:
	cmplwi  4, 0
	beqlr
	rlwinm  5, 3, 0, 27, 31
	add     4, 4, 5
	addi    4, 4, LINE-1
	srwi    4, 4, 5
	mtctr   4
1:	icbi    0, 3
	addi    3, 3, LINE
	bdnz    1b
	sync
	isync
	blr
