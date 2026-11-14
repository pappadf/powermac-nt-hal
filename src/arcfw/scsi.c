/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * scsi.c -- a polled initiator for the SCSI buses Open Firmware's SCSI package used to drive for
 * this firmware: the Network Server's two Symbios 53C825As, a 53C825A card in a Power Macintosh
 * slot, and a Power Macintosh 7500/8500's internal MESH bus.  Adapted from powermac-os2's
 * shim/scsi.c (the same author's, GPL-2.0-only): the same SCRIPTS program, made per bus, and the
 * same MESH sequence.
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
    ULONG regs;                    /* virtual address of the memory BAR, or of MESH's cell */
    ULONG device;                  /* Bandit 1 device number (53C825A) */
    BOOLEAN mesh;                  /* MESH: regs is the cell, dma its DBDMA channel */
    ULONG dma;
    UCHAR *work;                   /* 1 KB: the script, its message, command and status bytes,
                                      or MESH's two DBDMA descriptors */
};
#define MAX_BUSES 3
static struct _SCSI_BUS buses[MAX_BUSES];
static ULONG nbuses;
static UCHAR work_area[MAX_BUSES][1024] __attribute__((aligned(32)));
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

/* ---- MESH ------------------------------------------------------------------------------
 *
 * A Power Macintosh 7500/8500's internal bus: Apple's MESH cell in Grand Central (model
 * AAPL,343S1146), byte registers on 16-byte centres, the non-data phases through its 16-byte
 * FIFO and the data phases through DBDMA channel 10 -- as Linux's and MkLinux's mesh drivers do
 * it, and powermac-os2's shim.  Grand Central sits behind Bandit, which reverses the byte lanes,
 * so the DBDMA registers and descriptors are written with natural stores, as for the 53C825A.
 * Grand Central is mapped one-to-one (mmu.c). */
#define MR_COUNT_LO  0x0
#define MR_COUNT_HI  0x1
#define MR_FIFO      0x2
#define MR_SEQUENCE  0x3
#define MR_BUS0      0x4
#define MR_BUS1      0x5
#define MR_FIFO_CNT  0x6
#define MR_EXCEPTION 0x7
#define MR_ERROR     0x8
#define MR_INTR_MASK 0x9
#define MR_INTERRUPT 0xA
#define MR_SOURCE_ID 0xB
#define MR_DEST_ID   0xC
#define MR_SYNC      0xD
#define MR_SEL_TO    0xF

#define SEQ_DMA       0x80
#define SEQ_ATN       0x20
#define SEQ_ARBITRATE 0x1
#define SEQ_SELECT    0x2
#define SEQ_COMMAND   0x3
#define SEQ_STATUS    0x4
#define SEQ_DATAOUT   0x5
#define SEQ_DATAIN    0x6
#define SEQ_MSGOUT    0x7
#define SEQ_MSGIN     0x8
#define SEQ_BUSFREE   0x9
#define SEQ_DISRESEL  0xD
#define SEQ_RESETMESH 0xE
#define SEQ_FLUSHFIFO 0xF

#define BS0_REQ      0x20
#define BS1_RST      0x80
#define INT_CMDDONE  0x01
#define EXC_PHASEMM  0x02
#define EXC_SELTO    0x01

#define DMA_CONTROL    0x00
#define DMA_STATUS     0x04
#define DMA_CMDPTR     0x0C
#define DMA_RUN        0x8000u
#define DMA_ACTIVE     0x0400u
#define DB_OUTPUT_LAST 0x1000u
#define DB_INPUT_LAST  0x3000u
#define DB_STOP        0x7000u
#define MESH_CHUNK     0xF000u     /* bytes per data sequence: the count register is 16 bits */

static UCHAR mr_rd(SCSI_BUS *b, ULONG r) { UCHAR v = *(volatile UCHAR *)(b->regs + 16u * r); eieio(); return v; }
static VOID mr_wr(SCSI_BUS *b, ULONG r, UCHAR v) { *(volatile UCHAR *)(b->regs + 16u * r) = v; eieio(); }
static VOID mr_count(SCSI_BUS *b, ULONG n) { mr_wr(b, MR_COUNT_LO, (UCHAR)n); mr_wr(b, MR_COUNT_HI, (UCHAR)(n >> 8)); }
static VOID dma_wr(SCSI_BUS *b, ULONG o, ULONG v) { *(volatile ULONG *)(b->dma + o) = v; eieio(); }
static ULONG dma_rd(SCSI_BUS *b, ULONG o) { ULONG v = *(volatile ULONG *)(b->dma + o); eieio(); return v; }

static VOID dma_stop(SCSI_BUS *b)
{
    dma_wr(b, DMA_CONTROL, 0xF000u << 16);          /* RUN, PAUSE, FLUSH and WAKE all clear */
    ULONG t0 = tb();
    while ((dma_rd(b, DMA_STATUS) & DMA_ACTIVE) && !expired(t0, 10)) { }
}

/* the next completion: interrupt | exception << 8 | error << 16, all three then cleared; 0 when
 * nothing completed in `ms` */
static ULONG mesh_wait(SCSI_BUS *b, ULONG ms)
{
    ULONG t0 = tb();
    for (;;) {
        UCHAR i = mr_rd(b, MR_INTERRUPT);
        if (i & 7) {
            ULONG r = i | (ULONG)mr_rd(b, MR_EXCEPTION) << 8 | (ULONG)mr_rd(b, MR_ERROR) << 16;
            mr_wr(b, MR_EXCEPTION, 0xFF);
            mr_wr(b, MR_ERROR, 0xFF);
            mr_wr(b, MR_INTERRUPT, 0x07);
            return r;
        }
        if (expired(t0, ms)) return 0;
    }
}
static ULONG mesh_seq(SCSI_BUS *b, UCHAR seq, ULONG ms) { mr_wr(b, MR_SEQUENCE, seq); return mesh_wait(b, ms); }
static BOOLEAN mesh_clean(ULONG r) { return r == INT_CMDDONE; }   /* done, and nothing else */

static VOID mesh_init(SCSI_BUS *b)
{
    dma_stop(b);
    mr_wr(b, MR_EXCEPTION, 0xFF);
    mr_wr(b, MR_ERROR, 0xFF);
    mr_wr(b, MR_INTR_MASK, 0x00);                   /* polled: nothing reaches Grand Central */
    mesh_seq(b, SEQ_RESETMESH, 10);
    mr_wr(b, MR_SOURCE_ID, 7);
    mr_wr(b, MR_SEL_TO, 25);                        /* 250 ms */
    mr_wr(b, MR_SYNC, 2);                           /* asynchronous */
    mr_wr(b, MR_INTERRUPT, 0xFF);
    mr_wr(b, MR_SEQUENCE, SEQ_FLUSHFIFO);
}

/* something went wrong mid-command: reset the bus (a target left in any phase lets go of it)
 * and the cell, and give the targets their reset recovery */
static LONG mesh_fail(SCSI_BUS *b)
{
    dma_stop(b);
    mr_wr(b, MR_BUS1, BS1_RST);
    delay_us(30);
    mr_wr(b, MR_BUS1, 0);
    mesh_init(b);
    delay_us(250000);
    return -2;
}

static BOOLEAN mesh_wait_req(SCSI_BUS *b, ULONG ms)
{
    ULONG t0 = tb();
    while (!(mr_rd(b, MR_BUS0) & BS0_REQ)) if (expired(t0, ms)) return FALSE;
    return TRUE;
}

static LONG mesh_cmd(SCSI_BUS *b, ULONG id, const UCHAR *cdb, ULONG cdblen, ULONG buf, ULONG len, BOOLEAN out)
{
    /* arbitrate with our own ID in dest_id (MESH puts both IDs on the bus while it arbitrates)
     * and reselection off (the cell can hang in ARBITRATE on a reselection race): both as
     * Linux's mesh.c does */
    mr_wr(b, MR_INTERRUPT, 0x07);
    mr_wr(b, MR_SEQUENCE, SEQ_FLUSHFIFO);
    mr_wr(b, MR_DEST_ID, 7);
    mr_wr(b, MR_SEQUENCE, SEQ_DISRESEL);
    if (!mesh_clean(mesh_seq(b, SEQ_ARBITRATE, 10))) return mesh_fail(b);
    mr_wr(b, MR_DEST_ID, (UCHAR)id);
    ULONG r = mesh_seq(b, SEQ_SELECT | SEQ_ATN, 1000);
    if (r & (EXC_SELTO << 8)) { mr_wr(b, MR_SEQUENCE, SEQ_FLUSHFIFO); return -1; }   /* nothing at this ID */
    if (!mesh_clean(r)) return mesh_fail(b);

    /* IDENTIFY, LUN 0, no disconnect: the message's only byte, so ATN drops for it */
    if (!mesh_wait_req(b, 1000)) return mesh_fail(b);
    mr_count(b, 1);
    mr_wr(b, MR_SEQUENCE, SEQ_MSGOUT);
    mr_wr(b, MR_FIFO, 0x80);
    if (!mesh_clean(mesh_wait(b, 1000))) return mesh_fail(b);

    mr_count(b, cdblen);
    mr_wr(b, MR_SEQUENCE, SEQ_COMMAND);
    for (ULONG i = 0; i < cdblen; i++) mr_wr(b, MR_FIFO, cdb[i]);
    if (!mesh_clean(mesh_wait(b, 1000))) return mesh_fail(b);

    /* what the target does next: the data, or (an error) straight to status */
    if (!mesh_wait_req(b, 20000)) return mesh_fail(b);
    UCHAR phase = mr_rd(b, MR_BUS0) & 7;
    if (len && phase == (out ? 0 : 1)) {
        volatile ULONG *d = (volatile ULONG *)b->work;
        flush(buf, len);
        for (ULONG done = 0; done < len;) {
            ULONG n = len - done > MESH_CHUNK ? MESH_CHUNK : len - done;
            d[0] = (out ? DB_OUTPUT_LAST : DB_INPUT_LAST) << 16 | n;
            d[1] = buf + done;
            d[2] = 0; d[3] = 0;
            d[4] = DB_STOP << 16;
            d[5] = d[6] = d[7] = 0;
            flush((ULONG)b->work, 32);
            dma_stop(b);
            dma_wr(b, DMA_CMDPTR, (ULONG)b->work);
            dma_wr(b, DMA_CONTROL, DMA_RUN << 16 | DMA_RUN);
            mr_count(b, n);
            r = mesh_seq(b, (out ? SEQ_DATAOUT : SEQ_DATAIN) | SEQ_DMA, 20000);
            dma_stop(b);
            if (r & (EXC_PHASEMM << 8)) break;      /* the target moved on early: a short transfer */
            if (!mesh_clean(r)) return mesh_fail(b);
            done += n;
        }
        flush(buf, len);
    }

    mr_count(b, 1);
    if (!mesh_clean(mesh_seq(b, SEQ_STATUS, 20000)) || !mr_rd(b, MR_FIFO_CNT)) return mesh_fail(b);
    UCHAR status = mr_rd(b, MR_FIFO);
    mr_count(b, 1);
    if (!mesh_clean(mesh_seq(b, SEQ_MSGIN, 1000))) return mesh_fail(b);
    while (mr_rd(b, MR_FIFO_CNT)) (void)mr_rd(b, MR_FIFO);   /* COMMAND COMPLETE */
    if (!mesh_clean(mesh_seq(b, SEQ_BUSFREE, 1000))) return mesh_fail(b);
    return status;
}

/* the cell and its DMA channel: the node's two reg entries, offsets into Grand Central, whose
 * base is its own assigned-addresses (0xF3000000 on these machines) */
static SCSI_BUS *mesh_bus(OF_PHANDLE ctl)
{
    ULONG reg[4], aa[5], gc = 0xF3000000u;
    if (of_get_ints(ctl, "reg", reg, sizeof reg) < 16) { reg[0] = 0x18000; reg[2] = 0x8A00; }
    OF_PHANDLE p = of_parent(ctl);
    if (p && of_get_ints(p, "assigned-addresses", aa, sizeof aa) >= 20 && aa[2]) gc = aa[2];
    SCSI_BUS *b = &buses[nbuses];
    b->node = ctl; b->mesh = TRUE; b->work = work_area[nbuses];
    b->regs = gc + reg[0];
    b->dma = gc + reg[2];
    mesh_init(b);
    nbuses++;
    DBG(DBG_IO, "arcfw: scsi: MESH at %08x, DBDMA %08x\n", b->regs, b->dma);
    return b;
}

SCSI_BUS *scsi_bus(OF_PHANDLE ctl)
{
    for (ULONG i = 0; i < nbuses; i++) if (buses[i].node == ctl) return &buses[i];
    if (nbuses == MAX_BUSES) return NULL;
    CHAR nm[16];
    if (of_get_str(ctl, "name", nm, sizeof nm) >= 0 && strcmp(nm, "mesh") == 0) return mesh_bus(ctl);
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

static LONG bus_cmd(SCSI_BUS *b, ULONG id, const UCHAR *cdb, ULONG cdblen, ULONG buf, ULONG len, BOOLEAN out)
{
    return b->mesh ? mesh_cmd(b, id, cdb, cdblen, buf, len, out) : ncr_cmd(b, id, cdb, cdblen, buf, len, out);
}

/* with the retries a fresh target needs (unit attention after reset, a CD spinning up) */
static LONG cmd_retry(SCSI_BUS *b, ULONG id, const UCHAR *cdb, ULONG cdblen, ULONG buf, ULONG len, BOOLEAN out)
{
    LONG st = -2;
    for (ULONG tries = 0; tries < 8; tries++) {
        st = bus_cmd(b, id, cdb, cdblen, buf, len, out);
        if (st == 0 || st == -1) return st;
        if (st == 2) {
            static const UCHAR sense[6] = { 0x03, 0, 0, 0, 18, 0 };
            UCHAR *sb = b->work + 0x200;
            bus_cmd(b, id, sense, 6, (ULONG)sb, 18, FALSE);
            if ((sb[2] & 0x0F) == 2) delay_us(500000);   /* not ready, becoming ready */
        }
    }
    return st;
}

LONG scsi_command(SCSI_BUS *b, ULONG target, const UCHAR *cdb, ULONG cdblen, PVOID buf, ULONG len, BOOLEAN out)
{
    if (len > BOUNCE || cdblen > 12) return -2;
    if (out) memcpy(bounce, buf, len);
    LONG st = bus_cmd(b, target, cdb, cdblen, (ULONG)bounce, len, out);
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
