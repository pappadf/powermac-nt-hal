/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * mmu.c -- the address space this firmware and NT's loaders run in, which Open Firmware's MMU
 * package used to provide, and the exception vectors that stand in for Open Firmware's.
 *
 * What Open Firmware handed the loader, measured on this machine at the moment the firmware
 * starts OSLOADER (granny-smith, the 1.1.22 ROM in little-endian mode): translation on, every
 * BAT invalid, everything through a hashed page table at SDR1.  NT's kernel later loads BAT0
 * (KSEG0), DBAT3 (its I/O) and its own SDR1, and leaves the other BATs as it found them.  So
 * this firmware does as Open Firmware did -- a page table and no BATs -- and NT finds the BATs
 * exactly as it always has:
 *
 *   - low memory one-to-one, cacheable: the firmware, its heap and buffers, the vectors, the
 *     system parameter block, the snapshot and the OEM disk (up to 64 MB);
 *   - KSEG0, 0x80000000-0x807FFFFF onto the first 8 MB, where NT's loaders are linked and take
 *     their memory (the veneer mapped exactly this much);
 *   - Grand Central and Bandit's ports one-to-one, cache-inhibited and guarded; the 53C825A
 *     register windows at addresses of their own (mmu_map_io), since their BARs can sit under
 *     KSEG0's virtual range.
 *
 * The page table is prefilled, so no page fault is ever taken in it: there is no fault handler
 * to run.  The CPU is little-endian while this runs, and a little-endian word store at X lands
 * at X ^ 4, while the MMU reads each entry as two big-endian words at its address: so an
 * entry's two words are stored the other way round (word 0 at +4, word 1 at +0).
 *
 * The vectors.  stage0 enters with MSR[IP] clear, so exceptions go to 0x100...0x1F00 in RAM,
 * where Open Firmware's big-endian handlers would be garbage to a little-endian CPU.  Each
 * vector here saves r1 and r3, loads its own number and branches to trap_entry (crt0.S), which
 * reports the exception on the serial console and stops -- in place of Open Firmware's DEFAULT
 * CATCH!.  The decrementer's just reloads it and returns.  NT's kernel installs its own.
 * Original. */
#include "arcfw.h"
#include "of.h"
#include "dtsnap.h"

#define HTAB_PHYS 0x00E00000u                      /* above KSEG0's 8 MB: FirmwareTemporary */
#define HTAB_SIZE 0x00040000u                      /* 256 KB: 4096 groups, 32768 entries */
#define HTAB_MASK ((HTAB_SIZE >> 16) - 1u)
#define IDENT_MAX 0x04000000u
#define WIMG_M    0x2u                             /* memory: coherent */
#define WIMG_IO   0x5u                             /* device: caching inhibited, guarded */
#define PP_RW     0x2u

static ULONG io_next = 0xE0000000u;                 /* mmu_map_io's windows */

static VOID pte_insert(ULONG ea, ULONG pa, ULONG wimg)
{
    ULONG vsid = ea >> 28;                          /* SR[n] holds VSID n (mmu_init) */
    ULONG pi = (ea >> 12) & 0xFFFFu, api = (ea >> 22) & 0x3Fu;
    ULONG hash = (vsid ^ pi) & 0x7FFFFu;
    for (ULONG h = 0; h < 2; h++) {
        ULONG hv = h ? ~hash : hash;
        ULONG pteg = HTAB_PHYS | ((hv & ((HTAB_MASK << 10) | 0x3FFu)) << 6);
        for (ULONG s = 0; s < 8; s++) {
            volatile ULONG *p = (volatile ULONG *)(pteg + 8 * s);
            if (p[1] & 0x80000000u) continue;      /* word 0 (at +4, see above): valid */
            p[0] = (pa & 0xFFFFF000u) | 0x180u | (wimg << 3) | PP_RW;   /* R, C preset */
            __asm__ volatile("eieio" ::: "memory");
            p[1] = 0x80000000u | (vsid << 7) | (h << 6) | api;
            __asm__ volatile("sync" ::: "memory");
            return;
        }
    }
    for (;;) { }                                    /* the table is sized never to fill */
}

ULONG mmu_map_io(ULONG phys, ULONG size)
{
    ULONG v = io_next, base = phys & ~0xFFFu, end = (phys + size + 0xFFFu) & ~0xFFFu;
    for (ULONG pa = base; pa < end; pa += 0x1000u) {
        ULONG ea = v + (pa - base);
        pte_insert(ea, pa, WIMG_IO);
        __asm__ volatile("tlbie %0; sync; isync" ::"r"(ea) : "memory");
    }
    io_next += (end - base + 0xFFFFu) & ~0xFFFFu;
    return v + (phys & 0xFFFu);
}

static VOID map_ident(ULONG lo, ULONG hi, ULONG wimg)
{
    for (ULONG pa = lo & ~0xFFFu; pa < hi; pa += 0x1000u) pte_insert(pa, pa, wimg);
}

extern VOID trap_entry(VOID);
static VOID put_vector(ULONG at, const ULONG *code, ULONG n)
{
    volatile ULONG *w = (volatile ULONG *)at;
    for (ULONG i = 0; i < n; i++) w[i] = code[i];
}

static VOID install_vectors(VOID)
{
    for (ULONG v = 0x100; v < 0x2000; v += 0x100) {
        if (v == 0x900) {
            static const ULONG dec[] = {
                0x7C7043A6,                         /* mtsprg 0, r3 */
                0x3C607FFF,                         /* lis   r3, 0x7FFF */
                0x6063FFFF,                         /* ori   r3, r3, 0xFFFF */
                0x7C7603A6,                         /* mtdec r3 */
                0x7C7042A6,                         /* mfsprg r3, 0 */
                0x4C000064,                         /* rfi */
            };
            put_vector(v, dec, 6);
            continue;
        }
        ULONG code[4] = {
            0x7C3043A6,                             /* mtsprg 0, r1 */
            0x7C7143A6,                             /* mtsprg 1, r3 */
            0x38600000u | v,                        /* li    r3, vector */
            0x48000002u | ((ULONG)trap_entry & 0x03FFFFFCu),   /* ba    trap_entry */
        };
        put_vector(v, code, 4);
    }
    for (ULONG a = 0x100; a < 0x2000; a += 32) __asm__ volatile("dcbst 0,%0" ::"r"(a) : "memory");
    __asm__ volatile("sync" ::: "memory");
    for (ULONG a = 0x100; a < 0x2000; a += 32) __asm__ volatile("icbi 0,%0" ::"r"(a) : "memory");
    __asm__ volatile("sync; isync" ::: "memory");
}

/* Real mode, from crt0, before anything else: the vectors, the page table, then translation. */
VOID mmu_init(VOID)
{
    install_vectors();
    memset((PVOID)HTAB_PHYS, 0, HTAB_SIZE);
    ULONG ident = arcfw_ram < IDENT_MAX ? arcfw_ram : IDENT_MAX;
    map_ident(0, ident, WIMG_M);
    for (ULONG pa = 0; pa < ARC_KSEG0_SIZE; pa += 0x1000u) pte_insert(ARC_KSEG0 + pa, pa, WIMG_M);
    map_ident(0xF2000000u, 0xF2010000u, WIMG_IO);   /* Bandit 1's PCI I/O: the ISA/VGA ports */
    map_ident(0xF2800000u, 0xF2801000u, WIMG_IO);   /* its configuration address port */
    map_ident(0xF2C00000u, 0xF2C01000u, WIMG_IO);   /* and data port */
    map_ident(0xF3000000u, 0xF3020000u, WIMG_IO);   /* Grand Central: ESCC, VIA (Cuda), ... */
    map_ident(0xF4800000u, 0xF4801000u, WIMG_IO);   /* Bandit 2's configuration ports */
    map_ident(0xF4C00000u, 0xF4C01000u, WIMG_IO);

    ULONG sdr1 = HTAB_PHYS | HTAB_MASK;
    __asm__ volatile("sync; mtsdr1 %0; isync" ::"r"(sdr1));
    for (ULONG sr = 0; sr < 16; sr++)
        __asm__ volatile("mtsrin %0,%1" ::"r"(sr), "r"(sr << 28));      /* VSID sr, Ks = Kp = 0 */
    __asm__ volatile("isync" ::: "memory");
    for (ULONG ea = 0; ea < 0x40000u; ea += 0x1000u)                     /* every TLB congruence class */
        __asm__ volatile("tlbie %0" ::"r"(ea) : "memory");
    __asm__ volatile("sync; isync" ::: "memory");
    ULONG msr;
    __asm__ volatile("mfmsr %0" : "=r"(msr));
    msr |= 0x30u;                                                       /* IR | DR */
    __asm__ volatile("mtmsr %0; isync" ::"r"(msr) : "memory");
}

/* trap_entry's C half: what happened, on the serial console, and stop */
VOID arcfw_trap(ULONG vector, ULONG srr0, ULONG srr1, ULONG dar, ULONG dsisr, ULONG r1)
{
    con_printf("\narcfw: exception %x at %08x (MSR %08x, DAR %08x, DSISR %08x, r1 %08x)\n",
               vector, srr0, srr1, dar, dsisr, r1);
    of_exit();
}
