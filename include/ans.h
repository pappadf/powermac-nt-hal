/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * Original work of this project, written from hardware documentation and observed
 * behaviour.  Nothing here derives from leaked Windows NT source; see CONTRIBUTING.md.
 *
 * ans.h — the Apple Network Server 500/700 as the HAL sees it.  Sources: Apple, Network Server
 * Hardware Developer Notes §4 (address map, interrupt map §4.2, IDSEL §4.6.2); the Open Firmware
 * device tree of ROM 2.26NT (bandit `ranges`, `timebase-frequency`, gc `reg`, escc/ch-a `reg`);
 * Zilog Z8530 SCC User Manual (register protocol). */
#pragma once
#include "nt.h"

/* The 604 runs little-endian with address munging and the Bandits reverse their byte lanes, so a
 * plain load or store of the natural size reaches every device register (see docs). */
#define MMIO_BASE_PHYS  0xF0000000u   /* one 256 MB block covers both Bandits, GC, Hammerhead, ROM */
#define MMIO_BASE_SIZE  0x10000000u
#define MMIO_BASE_VIRT  0xB0000000u   /* HAL I/O window, the range KePhase0MapIo also uses */
extern volatile UCHAR *HalpIoBase;    /* virtual address of MMIO_BASE_PHYS */
#define IOV(phys) ((volatile void *)(HalpIoBase + ((phys) - MMIO_BASE_PHYS)))

static inline UCHAR  MmioRead8(ULONG phys)              { UCHAR v = *(volatile UCHAR *)IOV(phys); __asm__ volatile("eieio" ::: "memory"); return v; }
static inline void   MmioWrite8(ULONG phys, UCHAR v)    { *(volatile UCHAR *)IOV(phys) = v; __asm__ volatile("eieio" ::: "memory"); }
static inline ULONG  MmioRead32(ULONG phys)             { ULONG v = *(volatile ULONG *)IOV(phys); __asm__ volatile("eieio" ::: "memory"); return v; }
static inline void   MmioWrite32(ULONG phys, ULONG v)   { *(volatile ULONG *)IOV(phys) = v; __asm__ volatile("eieio" ::: "memory"); }
static inline USHORT MmioRead16(ULONG phys)             { USHORT v = *(volatile USHORT *)IOV(phys); __asm__ volatile("eieio" ::: "memory"); return v; }
static inline void   MmioWrite16(ULONG phys, USHORT v)  { *(volatile USHORT *)IOV(phys) = v; __asm__ volatile("eieio" ::: "memory"); }

/* Bandit PCI host bridges: registers at the base, config address port at +0x800000, config data
 * port at +0xC00000; PCI I/O space appears at base+io_addr (8 MB); PCI memory is identity-mapped. */
#define BANDIT1_BASE    0xF2000000u
#define BANDIT2_BASE    0xF4000000u
#define BANDIT_CFG_ADDR 0x00800000u
#define BANDIT_CFG_DATA 0x00C00000u
#define BANDIT_IO_SIZE  0x00800000u
#define PCI_MIN_IDSEL_DEVICE 11       /* devices 0..10 do not exist on a Bandit bus */

/* Grand Central at Bandit 1 IDSEL 16, 128 KB window */
#define GC_BASE         0xF3000000u
#define GC_INT_EVENTS   (GC_BASE + 0x20)
#define GC_INT_MASK     (GC_BASE + 0x24)
#define GC_INT_CLEAR    (GC_BASE + 0x28)
#define GC_INT_LEVELS   (GC_BASE + 0x2C)

/* ESCC channel A (ttya, the firmware console): control at +0x13020, data at +0x13030 */
#define ESCC_A_CTRL     (GC_BASE + 0x13020)
#define ESCC_A_DATA     (GC_BASE + 0x13030)
#define ESCC_B_CTRL     (GC_BASE + 0x13000)
#define ESCC_B_DATA     (GC_BASE + 0x13010)
#define SCC_RR0_TX_EMPTY 0x04
#define SCC_RR0_RX_AVAIL 0x01

/* Grand Central interrupt bits that matter to the HAL (ANS re-purposes the external lines) */
#define GC_IRQ_SCC_A     15
#define GC_IRQ_SCC_B     16
#define GC_IRQ_FWSCSI0   22   /* EXT2 — 53C825A #0 */
#define GC_IRQ_FWSCSI1   26   /* EXT6 — 53C825A #1 */
#define GC_IRQ_BANDIT_ERR 21  /* EXT1 — both Bandits' error interrupt */
#define GC_IRQ_VIA1      18   /* VIA1/Cuda cascade: ADB, the 60 Hz tick and the VIA timers */

/* PowerPC 604: timebase per Open Firmware `timebase-frequency` (bus clock / 4) */
#define ANS_TIMEBASE_HZ 11000000u

/* Cirrus 54M30 on-board video (Bandit 1 IDSEL 15). PCI I/O forwards at the bridge base
 * (bandit.c: 8 MB at 0xF2000000 driving PCI I/O 0), so the legacy VGA ports live at
 * BANDIT1_BASE + port. The framebuffer is BAR0 in Bandit's 256 MB PCI-memory window at
 * 0x80000000 (OF assigns it 0x81000000). */
#define C54M30_BUS      0
#define C54M30_DEV      15
#define C54M30_VENDOR   0x1013u
#define C54M30_DEVID    0x00A0u
#define VGA_PORT(p)     (BANDIT1_BASE + (p))   /* CPU-physical address of legacy VGA port p */
