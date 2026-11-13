/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * cuda.c -- reset and power-off, for ArcRestart/ArcReboot/ArcPowerDown, which used to be Open
 * Firmware's reset-all and shut-down.  One Cuda pseudo-command over Grand Central's VIA (at
 * +0x16000, registers 0x200 apart), sent with the handshake the HAL's src/cuda.c uses (TIP and
 * ByteAck out, the shift register clocked by Cuda), polled and bounded.  The reply is not read:
 * the machine resets or powers off under it.  Original. */
#include "arcfw.h"
#include "of.h"

#define VIA_REG(n)  (0xF3016000u + (n) * 0x200u)
#define VIA_BUFB    VIA_REG(0)
#define VIA_DIRB    VIA_REG(2)
#define VIA_SR      VIA_REG(10)
#define VIA_ACR     VIA_REG(11)
#define VIA_IFR     VIA_REG(13)
#define VIA_IER     VIA_REG(14)
#define VIA_IE_SR   0x04
#define VIA_ACR_SR_MASK 0x1C
#define VIA_ACR_SR_OUT  0x1C
#define VIA_ACR_SR_IN   0x0C
#define CUDA_BYTEACK 0x10
#define CUDA_TIP     0x20
#define CUDA_PKT_PSEUDO 0x01
#define CUDA_CMD_POWER_DOWN 0x0A
#define CUDA_CMD_RESET      0x11

static inline VOID eieio(VOID) { __asm__ volatile("eieio" ::: "memory"); }
static UCHAR rd(ULONG a) { UCHAR v = *(volatile UCHAR *)a; eieio(); return v; }
static VOID wr(ULONG a, UCHAR v) { *(volatile UCHAR *)a = v; eieio(); }
static BOOLEAN wait_byte(VOID)
{
    ULONG t0 = of_milliseconds();
    while (!(rd(VIA_IFR) & VIA_IE_SR)) if (of_milliseconds() - t0 > 20) return FALSE;
    return TRUE;
}

static VOID pseudo(UCHAR cmd)
{
    __asm__ volatile("mtmsr %0; isync" ::"r"(0x3031u | 0x10000u));     /* external interrupts stay off */
    wr(VIA_DIRB, rd(VIA_DIRB) | CUDA_TIP | CUDA_BYTEACK);
    wr(VIA_IER, VIA_IE_SR);                                            /* clear: polled, not interrupting */
    wr(VIA_BUFB, rd(VIA_BUFB) | CUDA_TIP | CUDA_BYTEACK);              /* idle */
    (void)rd(VIA_SR);
    wr(VIA_ACR, (rd(VIA_ACR) & ~VIA_ACR_SR_MASK) | VIA_ACR_SR_OUT);
    wr(VIA_SR, CUDA_PKT_PSEUDO);
    wr(VIA_BUFB, rd(VIA_BUFB) | CUDA_BYTEACK);
    wr(VIA_BUFB, rd(VIA_BUFB) & ~CUDA_TIP);                            /* TIP asserted */
    if (wait_byte()) {
        wr(VIA_SR, cmd);
        wr(VIA_BUFB, rd(VIA_BUFB) ^ CUDA_BYTEACK);
        wait_byte();
    }
    wr(VIA_ACR, (rd(VIA_ACR) & ~VIA_ACR_SR_MASK) | VIA_ACR_SR_IN);
    (void)rd(VIA_SR);
    wr(VIA_BUFB, rd(VIA_BUFB) | CUDA_TIP | CUDA_BYTEACK);              /* end of the request */
    ULONG t0 = of_milliseconds();
    while (of_milliseconds() - t0 < 2000) { }
}

VOID cuda_reset_system(VOID)
{
    pseudo(CUDA_CMD_RESET);
    of_exit();
}
VOID cuda_power_down(VOID)
{
    pseudo(CUDA_CMD_POWER_DOWN);
    of_exit();
}
