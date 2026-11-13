/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * stage0.S -- the big-endian front end's entry, its BATs, and the switch to little-endian mode.
 *
 * Open Firmware runs big-endian, as every one of these machines does out of the box: the boot
 * floppy sets no NVRAM variable.  xcoff-loader lays this image out at 0x600000 and `go` enters
 * _start with r5 = the client-interface handler (IEEE 1275 PowerPC binding).  stage0.c asks the
 * firmware what the ARC firmware will need, then stage0_take and stage0_enter leave it for good.
 * A power cycle or a reset brings the machine back big-endian: nothing persistent changes.
 * Original. */
#define LI32(r, v) lis r, (v)@h; ori r, r, (v)@l

	.section .text.start,"ax",@progbits
	.globl _start
_start:
	lis	6, stage0_cif@ha
	stw	5, stage0_cif@l(6)
	lis	1, (stage0_stack + 16384)@ha
	addi	1, 1, (stage0_stack + 16384)@l
	li	0, 0
	stwu	0, -16(1)
	bl	stage0_main		/* never returns */
1:	b	1b

	.text
/* LONG stage0_cif_call(ULONG *args): one argument array, as the binding defines it */
	.globl stage0_cif_call
stage0_cif_call:
	mflr	0
	stwu	1, -32(1)
	stw	0, 36(1)
	lis	4, stage0_cif@ha
	lwz	4, stage0_cif@l(4)
	mtctr	4
	bctrl
	lwz	0, 36(1)
	addi	1, 1, 32
	mtlr	0
	blr

/* VOID stage0_take(VOID): out of Open Firmware's MMU.  Interrupts off, then temporary BATs:
 * 0-256 MB identity, cacheable (this image and its stack, the ARC firmware's places, the OEM
 * disk), and $F0000000-$FFFFFFFF identity, cache-inhibited and guarded (Bandit's ports).  They
 * take precedence over the firmware's page table, and every address used from here on is in
 * one of them.  stage0_enter clears them again: the ARC firmware builds its own page table,
 * as Open Firmware did, and hands NT's loader a machine with every BAT invalid. */
	.globl stage0_take
stage0_take:
	mfmsr	3
	rlwinm	3, 3, 0, 17, 15		/* EE off */
	mtmsr	3
	isync
	li	0, 0
	mtspr	528, 0			/* every upper half invalid first */
	mtspr	530, 0
	mtspr	532, 0
	mtspr	534, 0
	mtspr	536, 0
	mtspr	538, 0
	mtspr	540, 0
	mtspr	542, 0
	isync
	li	3, 0x12
	mtspr	529, 3			/* IBAT0L: BRPN 0, WIMG 0010 (M), PP 10 */
	li	3, 0x1FFE
	mtspr	528, 3			/* IBAT0U: BEPI 0, 256 MB, Vs */
	li	3, 0x12
	mtspr	537, 3			/* DBAT0 = IBAT0 */
	li	3, 0x1FFE
	mtspr	536, 3
	LI32(3, 0xF000002A)
	mtspr	543, 3			/* DBAT3L: $F0000000, WIMG 0101 (I, G), PP 10 */
	LI32(3, 0xF0001FFE)
	mtspr	542, 3
	isync
	sync
	blr

/* VOID stage0_flush(ULONG addr, ULONG len): data cache written back over the range */
	.globl stage0_flush
stage0_flush:
	add	4, 3, 4
	rlwinm	3, 3, 0, 0, 26
1:	cmplw	3, 4
	bge	2f
	dcbst	0, 3
	addi	3, 3, 32
	b	1b
2:	sync
	blr

/* VOID stage0_enter(ULONG entry, ULONG blob, ULONG ram, const ULONG *bandits, ULONG n)
 *
 * 1. The instruction cache flash-invalidated (the caller has written the data cache back).
 * 2. Each Bandit's byte lanes reversed, by the same bus cycles the ROM makes when it starts in
 *    little-endian mode (traced in the emulator, powermac-os2 shim/stage0.S has the same
 *    sequence): config address $50080000 -- the bridge's own device 11, register $50,
 *    mode-select -- stored as a word at address port + 4 and read back, then a byte
 *    read-modify-write of data port + 7 clearing its bit 0.  The ROM runs it with the 604
 *    already little-endian, so its munged accesses reach port + 4 and data + 7; issued
 *    big-endian, these are the same cycles.
 * 3. Every BAT invalid, ILE on (a 60x rfi loads MSR[16-31] only), and rfi into the ARC firmware
 *    in real mode: MSR = FP | ME | LE, IP 0 (exceptions at 0x0, where it puts its own vectors),
 *    r3 = the device-tree snapshot, r4 = 'ARC0', r5 = RAM size.  Nothing touches a bridge
 *    between step 2 and the firmware. */
	.globl stage0_enter
stage0_enter:
	mr	20, 3
	mr	21, 4
	mr	22, 5
	mr	23, 6
	mr	24, 7
	mfspr	3, 1008			/* HID0: flash-invalidate the I-cache */
	ori	4, 3, 0x800
	mtspr	1008, 4
	isync
	mtspr	1008, 3
	isync
	sync

	cmpwi	24, 0
	beq	3f
	mtctr	24
	LI32(9, 0x50080000)
2:	lwz	8, 0(23)		/* bridge base: $F2000000, $F4000000 */
	addis	10, 8, 0x80		/* + $800000: config address */
	addis	11, 8, 0xC0		/* + $C00000: config data */
	stw	9, 4(10)
	sync
	lwz	12, 4(10)
	sync
	lbz	12, 7(11)
	sync
	andi.	12, 12, 0xFE
	stb	12, 7(11)
	sync
	eieio
	addi	23, 23, 4
	bdnz	2b
3:
	li	0, 0
	mtspr	528, 0			/* every BAT invalid: the firmware maps with a page table */
	mtspr	530, 0
	mtspr	532, 0
	mtspr	534, 0
	mtspr	536, 0
	mtspr	538, 0
	mtspr	540, 0
	mtspr	542, 0
	mfmsr	3
	oris	3, 3, 1			/* ILE */
	mtmsr	3
	isync
	mtsrr0	20
	li	3, 0x3001		/* FP | ME | LE; IR, DR and IP off */
	mtsrr1	3
	mr	3, 21
	LI32(4, 0x41524330)		/* 'ARC0' */
	mr	5, 22
	rfi

/* The ARC firmware, a little-endian flat image linked at 0x50000 (src/arcfw/arcfw.ld). */
	.section .rodata.payload,"a"
	.balign 8
	.globl stage0_payload, stage0_payload_end
stage0_payload:
	.incbin PAYLOAD
	.balign 8
stage0_payload_end:
