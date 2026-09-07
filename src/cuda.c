/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * UPSTREAM: Wack0/maciNTosh and MCJack123/maciNTosh-bandit (both GPL-2.0; the fork read
 *   at commit bafef57).  The Cuda handshake follows arcbandit/source/pxi.c — the same
 *   silicon — which is itself 95% Wack0/maciNTosh's arcgrackle/source/pxi.c: the fork's
 *   contribution is the Bandit port, Cuda at Grand Central +0x16000, which is the part
 *   that applies to this machine.  The three HalPxi* entry points take their names and
 *   prototypes from inc/halpxi.h, byte-identical between the two trees and so Wack0's
 *   file; it is the private HAL-to-driver ADB contract maciNTosh's usbadb.sys expects.
 *   Their exact semantics were read out of maciNTosh's halgoss.dll and usbadb.sys
 *   binaries (read locally, never redistributed: GPL-2.0 with no published source).
 *   Rewritten from reading; no lines copied.  See PROVENANCE.md and NOTICE.
 *
 * cuda.c — the Cuda microcontroller on Grand Central's VIA, and the three ADB entry points a
 * keyboard driver needs from the HAL.
 *
 * The Network Server's keyboard is ADB, reached through a 6522 VIA at Grand Central +0x16000
 * whose shift register talks to Cuda.  NT has no ADB anywhere, so this is the HAL's job, and the
 * shape of the job is set by maciNTosh: its `usbadb.sys` ("PowerMac General HID & Storage")
 * imports exactly three private names from HAL.dll — `HalPxiCommandAdb`, `HalPxiAdbSetCallback`
 * and `HalPxiAdbAutopoll` — connects no interrupt of its own, and knows nothing about the
 * chipset.  Implementing those three here is what lets that driver, or any driver written to the
 * same contract, run on this machine.  ("PXI" is maciNTosh's name for the Cuda/PMU interface.)
 *
 * The contract, as declared in maciNTosh-bandit `inc/halpxi.h` and as its own HAL implements it:
 *
 *   void Callback(UCHAR Status, UCHAR Command, PUCHAR Data, ULONG Length)
 *
 * is handed a Cuda ADB reply with the packet-type byte stripped: Status is Cuda's flag byte
 * (bit 1 = the addressed device did not answer, bit 6 = the packet came from an auto-poll),
 * Command is the ADB command byte echoed back, and Data/Length are what the device returned.
 * A driver reads the ADB address out of `Command >> 4`.
 *
 * The transport below is adapted from the same project's ARC firmware, `arcbandit/source/pxi.c`
 * (GPL-2.0) — the same silicon and the same handshake, so its sequence is the one this machine's
 * Cuda expects.  Rewritten here against Apple's Cuda protocol (TIP/ByteAck/TREQ over the VIA
 * shift register) with bounded waits, because a HAL may not hang the machine on a device that
 * stops answering. */
#include "hal.h"

/* The VIA's sixteen registers sit on 0x200 centres in an 8 KB window. */
#define VIA_REG(n)  (GC_BASE + 0x16000u + (n) * 0x200u)
#define VIA_BUFB    VIA_REG(0)
#define VIA_DIRB    VIA_REG(2)
#define VIA_SR      VIA_REG(10)
#define VIA_ACR     VIA_REG(11)
#define VIA_IFR     VIA_REG(13)
#define VIA_IER     VIA_REG(14)

#define VIA_IE_SR   0x04     /* shift-register interrupt, in both IFR and IER */
#define VIA_IE_SET  0x80     /* in IER: 1 = set the named bits, 0 = clear them */

#define VIA_ACR_SR_MASK 0x1C
#define VIA_ACR_SR_OUT  0x1C /* shift out under an external clock: Cuda clocks us */
#define VIA_ACR_SR_IN   0x0C /* shift in under an external clock */

/* Port B, as Cuda wires it: TREQ is Cuda's, the other two are ours, all active low. */
#define CUDA_TREQ    0x08    /* PB3 in:  Cuda has something to say */
#define CUDA_BYTEACK 0x10    /* PB4 out: this byte is taken */
#define CUDA_TIP     0x20    /* PB5 out: transfer in progress */

#define CUDA_PKT_ADB    0x00
#define CUDA_PKT_PSEUDO 0x01
#define CUDA_CMD_AUTOPOLL 0x01   /* pseudo-command: one byte, nonzero starts auto-polling */
#define CUDA_CMD_GET_TIME 0x03   /* reply carries 4 bytes, MSB first: seconds since 1904 */
#define CUDA_CMD_SET_TIME 0x09   /* takes those same 4 bytes */

#define CUDA_MAX_REPLY 24

/* A byte should move in tens of microseconds; ten milliseconds is a dead Cuda. */
#define CUDA_TIMEOUT_US 10000u

static BOOLEAN HalpCudaReady;        /* the VIA has been set up */
static PVOID HalpAdbCallback;        /* the driver's { entry, toc } descriptor, or NULL */
static USHORT HalpAdbAutopollMask;
static BOOLEAN HalpCudaBusy;         /* a transaction is in progress; keeps the poll out */

/* An auto-polled packet arrives on the VIA's interrupt, at that line's device IRQL — far above
 * DISPATCH_LEVEL.  The driver's callback takes a spin lock, and KeAcquireSpinLock *raises* to
 * DISPATCH_LEVEL, which from IRQL 21 is a lowering and bugchecks IRQL_NOT_GREATER_OR_EQUAL.
 * (maciNTosh's own HAL says the same thing by lowering to IRQL 2 before it calls the callback.)
 * So the interrupt only parks the packet in this ring and queues a DPC; the callback runs from
 * the DPC at DISPATCH_LEVEL.  One producer at device IRQL, one consumer at 2. */
#define ADB_RING 8
static struct { UCHAR Data[CUDA_MAX_REPLY]; UCHAR Length; } HalpAdbRing[ADB_RING];
/* Single producer (the VIA interrupt, at the device IRQL) and single consumer (the DPC, at
 * DISPATCH_LEVEL), which on this uniprocessor machine is safe without a lock: the consumer
 * cannot preempt the producer.  volatile because the compiler must not cache either index
 * across the delivery call.  An MP machine — the 700 can carry two 604e cards — would need a
 * spin lock here; nothing else in this file assumes one processor. */
static volatile ULONG HalpAdbRingHead, HalpAdbRingTail;
static KDPC HalpAdbDpc;
static BOOLEAN HalpAdbDpcReady;

/* Cuda drives the shift register, so "not busy" is "the VIA latched a byte". */
static BOOLEAN HalpCudaByteReady(VOID)
{
    return (MmioRead8(VIA_IFR) & VIA_IE_SR) != 0;
}

/* Spin for a byte.  Returns FALSE if Cuda never produced one. */
static BOOLEAN HalpCudaWait(VOID)
{
    ULONG start = HalpReadTimebaseLow();
    ULONG limit = CUDA_TIMEOUT_US * (HalpTimebaseFrequency / 1000000u);
    while (!HalpCudaByteReady())
        if (HalpReadTimebaseLow() - start > limit) return FALSE;
    return TRUE;
}

/* TREQ high means Cuda has finished the packet: it negates TREQ with the last byte. */
static BOOLEAN HalpCudaLastByte(VOID)
{
    return (MmioRead8(VIA_BUFB) & CUDA_TREQ) != 0;
}

static VOID HalpCudaAcr(UCHAR mode)
{
    MmioWrite8(VIA_ACR, (MmioRead8(VIA_ACR) & ~VIA_ACR_SR_MASK) | mode);
}

static VOID HalpCudaTip(BOOLEAN assert)
{
    if (assert) MmioWrite8(VIA_BUFB, MmioRead8(VIA_BUFB) | CUDA_BYTEACK);
    UCHAR b = MmioRead8(VIA_BUFB);
    MmioWrite8(VIA_BUFB, assert ? (UCHAR)(b & ~CUDA_TIP) : (UCHAR)(b | CUDA_TIP));
}

static VOID HalpCudaIdleLines(VOID)
{
    MmioWrite8(VIA_BUFB, MmioRead8(VIA_BUFB) | CUDA_TIP | CUDA_BYTEACK);
}

static VOID HalpCudaToggleAck(VOID)
{
    MmioWrite8(VIA_BUFB, MmioRead8(VIA_BUFB) ^ CUDA_BYTEACK);
}

/* ---- the two halves of a transfer ------------------------------------------------------- */

static BOOLEAN HalpCudaWriteFirst(UCHAR data)
{
    HalpCudaAcr(VIA_ACR_SR_OUT);
    MmioWrite8(VIA_SR, data);
    HalpCudaTip(TRUE);
    return HalpCudaWait();
}

static BOOLEAN HalpCudaWriteNext(UCHAR data)
{
    MmioWrite8(VIA_SR, data);
    HalpCudaToggleAck();
    return HalpCudaWait();
}

static VOID HalpCudaWriteEnd(VOID)
{
    HalpCudaAcr(VIA_ACR_SR_IN);
    (void)MmioRead8(VIA_SR);      /* clears the interrupt */
    HalpCudaIdleLines();
}

/* The first byte Cuda clocks out is an attention byte that carries no information; take it,
 * assert TIP, and return the byte after it — the packet type. */
static BOOLEAN HalpCudaReadFirst(PUCHAR out)
{
    if (!HalpCudaWait()) return FALSE;
    (void)MmioRead8(VIA_SR);
    HalpCudaTip(TRUE);
    if (!HalpCudaWait()) return FALSE;
    *out = MmioRead8(VIA_SR);
    return TRUE;
}

static BOOLEAN HalpCudaReadNext(PUCHAR out)
{
    HalpCudaToggleAck();
    if (!HalpCudaWait()) return FALSE;
    *out = MmioRead8(VIA_SR);
    return TRUE;
}

static VOID HalpCudaReadEnd(VOID)
{
    HalpCudaIdleLines();
    if (HalpCudaWait()) (void)MmioRead8(VIA_SR);   /* the idle acknowledge */
}

/* Read a packet Cuda is already presenting.  Returns its length. */
static ULONG HalpCudaReadPacket(PUCHAR reply, ULONG max)
{
    ULONG n = 0;
    if (!HalpCudaReadFirst(&reply[0])) { HalpCudaIdleLines(); return 0; }
    n = 1;
    while (n < max && !HalpCudaLastByte()) {
        if (!HalpCudaReadNext(&reply[n])) break;
        n++;
    }
    HalpCudaReadEnd();
    return n;
}

/* One synchronous request: send [type][payload…], read the reply.  Returns the reply length,
 * with reply[0] the packet type, [1] the flags and [2] the command echoed back. */
static ULONG HalpCudaRequest(UCHAR type, const UCHAR *in, ULONG inlen, PUCHAR reply, ULONG max)
{
    if (!HalpCudaReady) return 0;
    ULONG msr = HalpReadMsr();
    HalpDisableInterrupts();          /* a few hundred microseconds, and it must not nest */
    HalpCudaBusy = TRUE;
    ULONG n = 0;
    if (HalpCudaWriteFirst(type)) {
        BOOLEAN ok = TRUE;
        for (ULONG i = 0; i < inlen && ok; i++) ok = HalpCudaWriteNext(in[i]);
        HalpCudaWriteEnd();
        if (ok) n = HalpCudaReadPacket(reply, max);
    } else {
        HalpCudaWriteEnd();
    }
    HalpCudaBusy = FALSE;
    HalpWriteMsr(msr);
    return n;
}

/* ---- delivery to the driver -------------------------------------------------------------- */

/* maciNTosh's HAL hands the callback the reply from its flags byte on; do the same. */
static ULONG HalpAdbTrace;

static VOID HalpAdbDeliver(PUCHAR packet, ULONG length)
{
    /* Trace the bus scan, then only packets that carry data — which is what a key looks like. */
    if (HalpAdbTrace < 20 && (HalpAdbTrace < 12 || length > 2)) {
        HalpAdbTrace++;
        HalpPrint("HAL: adb status %x cmd %x len %d [%x %x]\n", packet[0], packet[1], length - 2,
                  length > 2 ? packet[2] : 0, length > 3 ? packet[3] : 0);
    }
    if (length <= 1 || !HalpAdbCallback) return;
    HalpCallDesc4(HalpAdbCallback, packet[0], packet[1], (ULONG)(packet + 2), length - 2);
}

/* Cuda has raised the shift-register interrupt with no request outstanding: an auto-poll
 * packet, a one-second tick, or a device that just spoke.  Called from the VIA interrupt. */
VOID HalpCudaService(VOID)
{
    UCHAR reply[CUDA_MAX_REPLY];
    if (!HalpCudaReady || HalpCudaBusy) return;
    if (!HalpCudaByteReady()) return;
    HalpCudaBusy = TRUE;
    ULONG n = HalpCudaReadPacket(reply, sizeof(reply));
    HalpCudaBusy = FALSE;
    if (n < 3 || reply[0] != CUDA_PKT_ADB) return;    /* a tick or a command reply: not ours */
    ULONG next = (HalpAdbRingTail + 1) % ADB_RING;
    if (next == HalpAdbRingHead) return;              /* the driver is not keeping up; drop it */
    for (ULONG i = 0; i + 1 < n && i < CUDA_MAX_REPLY; i++) HalpAdbRing[HalpAdbRingTail].Data[i] = reply[1 + i];
    HalpAdbRing[HalpAdbRingTail].Length = (UCHAR)(n - 1);
    HalpAdbRingTail = next;
    if (HalpAdbDpcReady) KeInsertQueueDpc(&HalpAdbDpc, NULL, NULL);
}

/* DISPATCH_LEVEL: hand the parked packets to the driver. */
static VOID HalpAdbDpcRoutine(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    UNREFERENCED_PARAMETER(Dpc); UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Arg1); UNREFERENCED_PARAMETER(Arg2);
    while (HalpAdbRingHead != HalpAdbRingTail) {
        ULONG i = HalpAdbRingHead;
        HalpAdbDeliver(HalpAdbRing[i].Data, HalpAdbRing[i].Length);
        HalpAdbRingHead = (i + 1) % ADB_RING;
    }
}
DEFINE_DESC(HalpAdbDpcRoutine);

/* ---- the real-time clock ---------------------------------------------------------------------
 * Cuda keeps the clock, as a 32-bit count of seconds since 1904-01-01 00:00 — the classic Mac
 * epoch, delivered most significant byte first.  It runs out in 2040.
 */
BOOLEAN HalpCudaGetTime(PULONG Seconds)
{
    UCHAR cmd = CUDA_CMD_GET_TIME, reply[CUDA_MAX_REPLY];
    ULONG n = HalpCudaRequest(CUDA_PKT_PSEUDO, &cmd, 1, reply, sizeof(reply));

    /* [0] packet type, [1] flags, [2] the command echoed back, then the four bytes */
    if (n < 7 || reply[2] != CUDA_CMD_GET_TIME) {
        HalpPrint("HAL: cuda get-time failed (n %d)\n", n);
        return FALSE;
    }
    /* volatile, and for the same reason as HalpGetUlong in disk.c — except that this is the
     * big-endian twin of that trap.  Assembling a big-endian ULONG out of four byte loads is an
     * idiom clang folds into one `lwbrx`, which faults here because reply[3] sits at 2 mod 4 on
     * the stack.  The Makefile's -combiner-store-merging=false does not cover it: that is a
     * store combine, and this is a load. */
    volatile const UCHAR *p = reply;
    *Seconds = ((ULONG)p[3] << 24) | ((ULONG)p[4] << 16) |
               ((ULONG)p[5] << 8) | (ULONG)p[6];
    return TRUE;
}

BOOLEAN HalpCudaSetTime(ULONG Seconds)
{
    UCHAR out[5], reply[CUDA_MAX_REPLY];
    ULONG n;

    volatile UCHAR *p = out;                /* the store-side twin: `stwbrx` at out[1] */
    p[0] = CUDA_CMD_SET_TIME;
    p[1] = (UCHAR)(Seconds >> 24); p[2] = (UCHAR)(Seconds >> 16);
    p[3] = (UCHAR)(Seconds >> 8);  p[4] = (UCHAR)Seconds;
    n = HalpCudaRequest(CUDA_PKT_PSEUDO, out, sizeof(out), reply, sizeof(reply));
    return (BOOLEAN)(n >= 3 && reply[2] == CUDA_CMD_SET_TIME);
}

/* ---- setup --------------------------------------------------------------------------------- */

VOID HalpCudaInitialize(VOID)
{
    if (!HalpIoBase || HalpCudaReady) return;
    (void)MmioRead8(VIA_SR);
    /* TIP and ByteAck are ours to drive, TREQ is Cuda's. */
    MmioWrite8(VIA_DIRB, (UCHAR)((MmioRead8(VIA_DIRB) & ~CUDA_TREQ) | CUDA_BYTEACK | CUDA_TIP));
    HalpCudaIdleLines();
    MmioWrite8(VIA_IER, (UCHAR)~VIA_IE_SET);     /* clear every VIA interrupt enable */
    (void)MmioRead8(VIA_IER);
    (void)MmioRead8(VIA_SR);
    HalpCudaAcr(VIA_ACR_SR_IN);
    HalpCudaReady = TRUE;
}

/* ---- the exported contract ------------------------------------------------------------------ */

VOID HalPxiAdbSetCallback(PVOID Callback)
{
    HalpAdbCallback = Callback;
}

/* Start or stop auto-polling.  Cuda takes no device mask — it polls whatever answered its own
 * bus scan — so a nonzero mask means "on".  Enabling it also unmasks the VIA at Grand Central,
 * which is how an unsolicited packet reaches HalpCudaService. */
VOID HalPxiAdbAutopoll(USHORT Mask)
{
    if (!HalpCudaReady) return;
    if (HalpAdbAutopollMask == Mask) return;
    HalpAdbAutopollMask = Mask;
    if (!HalpAdbDpcReady) {
        KeInitializeDpc(&HalpAdbDpc, DESC(HalpAdbDpcRoutine), NULL);
        HalpAdbDpcReady = TRUE;
    }
    UCHAR arg[2] = { CUDA_CMD_AUTOPOLL, (UCHAR)(Mask != 0) };
    UCHAR reply[CUDA_MAX_REPLY];
    HalpCudaRequest(CUDA_PKT_PSEUDO, arg, 2, reply, sizeof(reply));
    HalpPrint("HAL: ADB autopoll %s\n", Mask ? "on" : "off");
    KIRQL old; KeRaiseIrql(HIGH_LEVEL, &old);
    if (Mask) {
        MmioWrite8(VIA_IER, VIA_IE_SET | VIA_IE_SR);
        HalpRegisteredInterrupts |= 1u << GC_IRQ_VIA1;
    } else {
        MmioWrite8(VIA_IER, VIA_IE_SR);
        HalpRegisteredInterrupts &= ~(1u << GC_IRQ_VIA1);
    }
    HalpSetGcMask(HalpIrqlToMask[old] & HalpRegisteredInterrupts);
    KeLowerIrql(old);
}

/* Send one ADB command and hand the reply to the callback.  `Poll` asks for the answer to be
 * waited for rather than left to the interrupt; this transport is synchronous either way. */
BOOLEAN HalPxiCommandAdb(UCHAR Command, PUCHAR Data, UCHAR Length, BOOLEAN Poll)
{
    UNREFERENCED_PARAMETER(Poll);
    UCHAR request[16], reply[CUDA_MAX_REPLY];
    if (!HalpCudaReady || Length > sizeof(request) - 1) return FALSE;
    request[0] = Command;
    for (ULONG i = 0; i < Length; i++) request[1 + i] = Data[i];
    ULONG n = HalpCudaRequest(CUDA_PKT_ADB, request, Length + 1u, reply, sizeof(reply));
    if (n <= 2 || reply[0] != CUDA_PKT_ADB || reply[2] != Command) return FALSE;
    HalpAdbDeliver(&reply[1], n - 1);
    return TRUE;
}
