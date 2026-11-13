/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * scsi.c -- a polled initiator for the Network Server's two Symbios 53C825A buses, which Open
 * Firmware's SCSI package used to drive for this firmware.  Adapted from powermac-os2's
 * shim/scsi.c (the same author's, GPL-2.0-only): the same SCRIPTS program, made per bus.
 *
 * The machine runs as stage0 left it: the CPU little-endian, Bandit reversing its byte lanes, so
 * every register and every DMA structure is written as its little-endian PCI owner expects, with
 * natural stores.  Each chip is reached through the memory BAR Open Firmware assigned it (the
 * snapshot's assigned-addresses, which the HAL and NT's symc810 use too), mapped by the page
 * table at a window of its own (mmu.c).  A command is select with ATN, an IDENTIFY without
 * disconnect privilege, the command, the data, status and message in: one at a time, nothing
 * disconnects.  Data moves through a bounce buffer in the firmware's own memory, whose virtual
 * and physical addresses are the same, so the SCRIPTS processor can be handed it directly and
 * callers may pass any buffer at any byte offset.  Original. */
#include "arcfw.h"
#include "of.h"

#define NCR_SCNTL0   0x00
#define NCR_SCID     0x04
#define NCR_DSTAT    0x0C
#define NCR_ISTAT    0x14
#define NCR_DSP      0x2C
#define NCR_DSPS     0x30
#define NCR_DMODE    0x38
#define NCR_DIEN     0x39
#define NCR_SIEN0    0x40
#define NCR_SIEN1    0x41
#define NCR_SIST0    0x42
#define NCR_SIST1    0x43
#define NCR_STIME0   0x48
#define NCR_RESPID0  0x4A
#define NCR_RESPID1  0x4B

#define BOUNCE 0x10000u

struct _SCSI_BUS {
    OF_PHANDLE node;
    ULONG regs;                    /* virtual address of the memory BAR */
    ULONG device;                  /* Bandit 1 device number */
    UCHAR *work;                   /* 1 KB: the script, its message, command and status bytes */
};
static struct _SCSI_BUS buses[2];
static ULONG nbuses;
static UCHAR work_area[2][1024] __attribute__((aligned(32)));
static UCHAR bounce[BOUNCE] __attribute__((aligned(32)));

static inline VOID eieio(VOID) { __asm__ volatile("eieio" ::: "memory"); }
static VOID flush(ULONG a, ULONG n)
{
    for (ULONG p = a & ~31u; p < a + n; p += 32) __asm__ volatile("dcbf 0,%0" ::"r"(p) : "memory");
    __asm__ volatile("sync" ::: "memory");
}

/* time from the time base, at the slowest bus these machines have (40 MHz / 4), so a time-out
 * is never short */
#define TB_PER_MS 10000u
static ULONG tb(VOID) { ULONG t; __asm__ volatile("mftb %0" : "=r"(t)); return t; }
static BOOLEAN expired(ULONG t0, ULONG ms) { return tb() - t0 > ms * TB_PER_MS; }
static VOID delay_us(ULONG us) { ULONG t0 = tb(); while (tb() - t0 < us * (TB_PER_MS / 1000u) + 1u) { } }

/* Bandit 1's configuration cycles: (1 << device) | register to the address port, the dword at
 * the data port (the HAL's pci.c, with the lanes reversed) */
#define BANDIT1_CFG_ADDR 0xF2800000u
#define BANDIT1_CFG_DATA 0xF2C00000u
static ULONG cfg_read32(ULONG dev, ULONG reg)
{
    *(volatile ULONG *)BANDIT1_CFG_ADDR = (1u << dev) | (reg & 0xFC); eieio();
    ULONG v = *(volatile ULONG *)BANDIT1_CFG_DATA; eieio();
    return v;
}
static VOID cfg_write32(ULONG dev, ULONG reg, ULONG v)
{
    *(volatile ULONG *)BANDIT1_CFG_ADDR = (1u << dev) | (reg & 0xFC); eieio();
    *(volatile ULONG *)BANDIT1_CFG_DATA = v; eieio();
}

static UCHAR rd8(SCSI_BUS *b, ULONG o) { UCHAR v = *(volatile UCHAR *)(b->regs + o); eieio(); return v; }
static VOID wr8(SCSI_BUS *b, ULONG o, UCHAR v) { *(volatile UCHAR *)(b->regs + o) = v; eieio(); }

static VOID chip_init(SCSI_BUS *b)
{
    wr8(b, NCR_ISTAT, 0x40);       /* software reset */
    delay_us(100);
    wr8(b, NCR_ISTAT, 0x00);
    wr8(b, NCR_SCNTL0, 0xC0);      /* full arbitration */
    wr8(b, NCR_SCID, 0x47);        /* ID 7, respond to reselection */
    wr8(b, NCR_RESPID0, 0x80);
    wr8(b, NCR_RESPID1, 0x00);
    wr8(b, NCR_STIME0, 0x0C);      /* selection time-out about 100 ms */
    wr8(b, NCR_DMODE, 0x00);
    wr8(b, NCR_DIEN, 0x7D);        /* every DMA condition halts and interrupts */
    wr8(b, NCR_SIEN0, 0x8F);       /* phase mismatch, gross error, unexpected disconnect, reset, parity */
    wr8(b, NCR_SIEN1, 0x04);       /* selection time-out */
}

SCSI_BUS *scsi_bus(OF_PHANDLE ctl)
{
    for (ULONG i = 0; i < nbuses; i++) if (buses[i].node == ctl) return &buses[i];
    if (nbuses == 2) return NULL;
    /* the register BAR Open Firmware assigned: the assigned-addresses entry (phys.hi phys.mid
     * phys.lo size.hi size.lo) for configuration register 0x14 -- BAR1, the chip's registers in
     * memory space (BAR0 is the same registers in I/O space, BAR2 its SCRIPTS RAM, 0x30 its
     * expansion ROM, which the ROM lists first) */
    ULONG aa[30];
    LONG n = of_get_ints(ctl, "assigned-addresses", aa, sizeof aa);
    ULONG bar = 0, dev = 0;
    for (LONG k = 0; k + 5 <= n / 4; k += 5) {
        if ((aa[k] & 0xFF) == 0x14 && ((aa[k] >> 24) & 3) == 2) { bar = aa[k + 2]; dev = (aa[k] >> 11) & 0x1F; break; }
    }
    if (!bar || dev < 11) { DBG(DBG_IO, "arcfw: scsi: no memory BAR on %x\n", ctl); return NULL; }
    SCSI_BUS *b = &buses[nbuses];
    b->node = ctl; b->device = dev; b->work = work_area[nbuses];
    /* memory decoding and bus mastering on, as the ROM's package had them */
    cfg_write32(dev, 0x04, (cfg_read32(dev, 0x04) & 0xFFFF0000u) | 0x0006u);
    b->regs = mmu_map_io(bar, 0x100);
    chip_init(b);
    nbuses++;
    DBG(DBG_IO, "arcfw: scsi: 53C825A at device %u, registers %08x (virtual %08x)\n", dev, bar, b->regs);
    return b;
}

/* one command: the SCSI status byte (0 good, 2 check condition), -1 when nothing answers at
 * that ID, -2 on any other failure.  `buf` must be physically addressable as it is (the bounce
 * buffer or the work area). */
static LONG ncr_cmd(SCSI_BUS *b, ULONG id, const UCHAR *cdb, ULONG cdblen, ULONG buf, ULONG len, BOOLEAN out)
{
    ULONG *s = (ULONG *)b->work;
    const ULONG at = (ULONG)b->work;
    UCHAR *msgout = b->work + 0x100, *cdbbuf = b->work + 0x110, *status = b->work + 0x120, *msgin = b->work + 0x124;
    s[0] = 0x41000000u | (id << 16);        /* SELECT ATN id; alternate (reselected): INT 2 */
    s[1] = at + 4 * 20;
    s[2] = 0x0E000001u;                     /* MOVE 1, msgout, WHEN MSG_OUT */
    s[3] = (ULONG)msgout;
    s[4] = 0x0A000000u | cdblen;            /* MOVE cdblen, cdb, WHEN CMD */
    s[5] = (ULONG)cdbbuf;
    s[6] = len ? 0x830B0000u : 0x80080000u; /* JUMP status, WHEN STATUS (always if no data) */
    s[7] = at + 4 * 10;
    s[8] = (out ? 0x08000000u : 0x09000000u) | len;   /* MOVE len, buf, WHEN DATA_OUT / DATA_IN */
    s[9] = buf;
    s[10] = 0x0B000001u;                    /* MOVE 1, status, WHEN STATUS */
    s[11] = (ULONG)status;
    s[12] = 0x0F000001u;                    /* MOVE 1, msgin, WHEN MSG_IN */
    s[13] = (ULONG)msgin;
    s[14] = 0x60000040u;                    /* CLEAR ACK */
    s[15] = 0;
    s[16] = 0x48000000u;                    /* WAIT DISCONNECT */
    s[17] = 0;
    s[18] = 0x98080000u;                    /* INT 1: done */
    s[19] = 1;
    s[20] = 0x98080000u;                    /* INT 2: reselected (never: no disconnect privilege) */
    s[21] = 2;
    *(volatile UCHAR *)msgout = 0x80;       /* IDENTIFY, LUN 0, no disconnect */
    for (ULONG i = 0; i < cdblen; i++) ((volatile UCHAR *)cdbbuf)[i] = cdb[i];
    *(volatile UCHAR *)status = 0xFF;
    flush(at, 0x200);
    if (len) flush(buf, len);

    *(volatile ULONG *)(b->regs + NCR_DSP) = at;
    eieio();
    ULONG t0 = tb();
    while (!expired(t0, 20000)) {
        UCHAR istat = rd8(b, NCR_ISTAT);
        if (!(istat & 0x03)) continue;
        UCHAR dstat = 0, sist0 = 0, sist1 = 0;
        if (istat & 0x01) dstat = rd8(b, NCR_DSTAT);
        if (istat & 0x02) { sist0 = rd8(b, NCR_SIST0); sist1 = rd8(b, NCR_SIST1); }
        if (len) flush(buf, len);
        flush(at, 0x200);
        if (sist1 & 0x04) return -1;        /* selection time-out: no device */
        if (sist0) return -2;               /* phase mismatch, unexpected disconnect, gross error, reset, parity */
        if ((dstat & 0x04) && *(volatile ULONG *)(b->regs + NCR_DSPS) == 1) return *(volatile UCHAR *)status;
        return -2;
    }
    chip_init(b);                           /* a script that never finished: stop it */
    return -2;
}

/* with the retries a fresh target needs (unit attention after reset, a CD spinning up) */
static LONG cmd_retry(SCSI_BUS *b, ULONG id, const UCHAR *cdb, ULONG cdblen, ULONG buf, ULONG len, BOOLEAN out)
{
    LONG st = -2;
    for (ULONG tries = 0; tries < 8; tries++) {
        st = ncr_cmd(b, id, cdb, cdblen, buf, len, out);
        if (st == 0 || st == -1) return st;
        if (st == 2) {
            static const UCHAR sense[6] = { 0x03, 0, 0, 0, 18, 0 };
            UCHAR *sb = b->work + 0x200;
            ncr_cmd(b, id, sense, 6, (ULONG)sb, 18, FALSE);
            if ((sb[2] & 0x0F) == 2) delay_us(500000);   /* not ready, becoming ready */
        }
    }
    return st;
}

LONG scsi_command(SCSI_BUS *b, ULONG target, const UCHAR *cdb, ULONG cdblen, PVOID buf, ULONG len, BOOLEAN out)
{
    if (len > BOUNCE || cdblen > 12) return -2;
    if (out) memcpy(bounce, buf, len);
    LONG st = ncr_cmd(b, target, cdb, cdblen, (ULONG)bounce, len, out);
    if (!out && st == 0) memcpy(buf, bounce, len);
    return st;
}

static ULONG be32(const UCHAR *p) { return (ULONG)p[0] << 24 | (ULONG)p[1] << 16 | (ULONG)p[2] << 8 | p[3]; }

LONG scsi_capacity(SCSI_BUS *b, ULONG target, PULONG blocks, PULONG block)
{
    static const UCHAR tur[6] = { 0, 0, 0, 0, 0, 0 };
    static const UCHAR cap[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    if (cmd_retry(b, target, tur, 6, 0, 0, FALSE) == -1) return -1;
    UCHAR *c = b->work + 0x200;
    if (cmd_retry(b, target, cap, 10, (ULONG)c, 8, FALSE) != 0) return -1;
    *blocks = be32(c) + 1;
    *block = be32(c + 4);
    if (*block != 512 && *block != 2048) return -1;
    return 0;
}

/* bytes [pos, pos + len) of the unit into buf: whole blocks through the bounce buffer */
LONG scsi_read_bytes(SCSI_BUS *b, ULONG target, ULONG bs, ULONG pos_hi, ULONG pos_lo, PVOID buf, ULONG len)
{
    ULONGLONG pos = ((ULONGLONG)pos_hi << 32) | pos_lo;
    ULONG shift = bs == 2048 ? 11 : 9;          /* scsi_capacity takes no other size */
    UCHAR *d = buf;
    while (len) {
        ULONG lba = (ULONG)(pos >> shift), skip = (ULONG)pos & (bs - 1);
        ULONG want = skip + len;
        if (want > BOUNCE) want = BOUNCE;
        ULONG nblk = (want + bs - 1) / bs;
        UCHAR c[10] = { 0x28, 0, (UCHAR)(lba >> 24), (UCHAR)(lba >> 16), (UCHAR)(lba >> 8), (UCHAR)lba, 0, (UCHAR)(nblk >> 8), (UCHAR)nblk, 0 };
        if (cmd_retry(b, target, c, 10, (ULONG)bounce, nblk * bs, FALSE) != 0) return -1;
        ULONG n = nblk * bs - skip;
        if (n > len) n = len;
        memcpy(d, bounce + skip, n);
        d += n; pos += n; len -= n;
    }
    return 0;
}
