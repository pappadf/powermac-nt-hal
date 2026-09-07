/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * UPSTREAM: Wack0/entii-for-workcubes, halartx/source/ (GPL-2.0, commit c9b041da).
 *   The IRQL-to-mask model and the PCR-owned IRQL come from reading `irql.c`.
 *   Rewritten from reading: no lines were copied, and the code here was written fresh
 *   for Grand Central, Bandit and the ESCC.  See PROVENANCE.md.
 *
 * irql.c — KeRaiseIrql / KeLowerIrql.  On PowerPC NT the HAL owns the IRQL: it lives in the PCR,
 * device interrupts are masked in Grand Central by IRQL, the decrementer is gated by MSR[EE]. */
#include "hal.h"

VOID KeRaiseIrql(KIRQL NewIrql, PKIRQL OldIrql)
{
    PKPCR pcr = PCR;
    KIRQL cur = pcr->CurrentIrql;
    if (NewIrql == cur) { *OldIrql = cur; return; }
    if (NewIrql < cur)
        KeBugCheckEx(IRQL_NOT_GREATER_OR_EQUAL, NewIrql, cur, 0, (ULONG)__builtin_return_address(0));
    HalpDisableInterrupts();
    *OldIrql = cur;
    pcr->CurrentIrql = NewIrql;
    HalpSetGcMask(HalpIrqlToMask[NewIrql] & HalpRegisteredInterrupts);
    if (NewIrql < CLOCK2_LEVEL) HalpEnableInterrupts();
}

VOID KeLowerIrql(KIRQL NewIrql)
{
    PKPCR pcr = PCR;
    KIRQL cur = pcr->CurrentIrql;
    if (NewIrql != cur) {
        if (NewIrql > cur)
            KeBugCheckEx(IRQL_NOT_LESS_OR_EQUAL, NewIrql, cur, 0, (ULONG)__builtin_return_address(0));
        HalpDisableInterrupts();
        pcr->CurrentIrql = NewIrql;
        HalpSetGcMask(HalpIrqlToMask[NewIrql] & HalpRegisteredInterrupts);
        if (NewIrql < CLOCK2_LEVEL) HalpEnableInterrupts();
    }
    if (NewIrql < DISPATCH_LEVEL && pcr->SoftwareInterrupt)
        KiDispatchSoftwareInterrupt();
}
