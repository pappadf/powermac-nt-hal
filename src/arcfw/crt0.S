/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * crt0.S -- how stage0 enters this firmware, and where exceptions land.
 *
 * stage0 (src/stage0) has switched the machine to little-endian itself and left Open Firmware
 * behind: it rfi's here in real mode (IR, DR and IP off; ILE, LE, ME, FP on), every BAT invalid,
 * with r3 = the device-tree snapshot (dtsnap.h), r4 = 'ARC0' and r5 = the RAM size.  The image
 * carries its own stack (Open Firmware lent the old one).  mmu_init builds the page table and
 * turns translation on; arcfw_main does the rest and never returns. */
	.section .text.start,"ax",@progbits
	.globl _start
_start:
	/* the three words live in .data: they are stored before bss is cleared */
	lis	11, arcfw_snapshot@ha
	stw	3, arcfw_snapshot@l(11)
	lis	11, arcfw_ram@ha
	stw	5, arcfw_ram@l(11)
	lis	11, arcfw_magic@ha
	stw	4, arcfw_magic@l(11)
	/* clear .bss */
	lis	11, __bss_start@ha
	addi	11, 11, __bss_start@l
	lis	12, __bss_end@ha
	addi	12, 12, __bss_end@l
	li	0, 0
1:	cmplw	11, 12
	bge	2f
	stw	0, 0(11)
	addi	11, 11, 4
	b	1b
2:	lis	1, (arcfw_stack + 0x10000)@ha
	addi	1, 1, (arcfw_stack + 0x10000)@l
	li	0, 0
	stwu	0, -64(1)
	bl	mmu_init			/* real mode in, translation on out */
	lis	3, arcfw_snapshot@ha
	lwz	3, arcfw_snapshot@l(3)
	bl	arcfw_main
	b	of_exit

/* VOID arcfw_enter(PVOID entry, ULONG argc, PCHAR *argv, PCHAR *envp): into the loaded program,
 * as the veneer's PxInvoke does -- a bare branch with the C arguments in 3..5.  NT's loaders
 * find their own TOC. */
	.text
	.globl arcfw_enter
arcfw_enter:
	mtctr	3
	mr	3, 4
	mr	4, 5
	mr	5, 6
	bctr

/* An exception (mmu.c's vectors): SPRG0 = the old r1, SPRG1 = the old r3, r3 = the vector.  The
 * machine state may be anyone's -- this firmware's, or a loader's with its own page table -- so
 * BATs are loaded to reach the firmware, its stack and the ESCC whatever the page table holds:
 * IBAT0/DBAT0 0-256 MB one-to-one, DBAT3 $F0000000 cache-inhibited.  Then arcfw_trap reports and
 * stops; nothing returns, so nothing needs restoring. */
	.globl trap_entry
trap_entry:
	li	4, 0
	mtspr	528, 4
	mtspr	536, 4
	mtspr	542, 4
	isync
	li	4, 0x12
	mtspr	529, 4				/* IBAT0L: BRPN 0, WIMG 0010, PP 10 */
	mtspr	537, 4				/* DBAT0L */
	li	4, 0x1FFE
	mtspr	528, 4				/* IBAT0U: 0, 256 MB, Vs */
	mtspr	536, 4				/* DBAT0U */
	lis	4, 0xF000
	ori	4, 4, 0x002A
	mtspr	543, 4				/* DBAT3L: $F0000000, WIMG 0101, PP 10 */
	lis	4, 0xF000
	ori	4, 4, 0x1FFE
	mtspr	542, 4				/* DBAT3U */
	isync
	lis	1, (arcfw_trap_stack + 0x2000)@ha
	addi	1, 1, (arcfw_trap_stack + 0x2000)@l
	li	0, 0
	stwu	0, -64(1)
	mfsrr0	4
	mfsrr1	5
	mfdar	6
	mfdsisr	7
	mfsprg	8, 0
	mfmsr	9
	ori	9, 9, 0x30			/* IR | DR: through the BATs above */
	mtmsr	9
	isync
	bl	arcfw_trap
1:	b	1b

	.data
	.globl arcfw_snapshot, arcfw_ram, arcfw_magic
	.balign 8
arcfw_snapshot:	.long 0
arcfw_ram:	.long 0
arcfw_magic:	.long 0

	.lcomm	arcfw_stack, 0x10000, 16
	.lcomm	arcfw_trap_stack, 0x2000, 16
