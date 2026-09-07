/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * UPSTREAM: Wack0/entii-for-workcubes, halartx/source/ (GPL-2.0, commit c9b041da).
 *   External-interrupt dispatch through `PCR->InterruptRoutine`, and the
 *   KINTERRUPT-from-dispatch-code trick, come from reading `ints.c`.
 *   Rewritten from reading: no lines were copied, and the code here was written fresh
 *   for Grand Central, Bandit and the ESCC.  See PROVENANCE.md.
 *
 * ints.c — Grand Central interrupt controller: 32 level/edge sources with Events, Mask, Clear and
 * Levels registers (GC +0x20..+0x2C).  Each source maps to one NT vector (DEVICE_VECTORS + bit)
 * and one IRQL; the HAL masks by IRQL and dispatches through the PCR's interrupt routine table. */
#include "hal.h"

ULONG HalpRegisteredInterrupts;
ULONG HalpIrqlToMask[32];
static ULONG HalpGcShadowMask;

/* IRQL per Grand Central bit: higher bits (the ANS's SCSI and slot lines) get higher IRQLs;
 * everything stays within DISPATCH_LEVEL+1 .. MAXIMUM_DEVICE_LEVEL. */
const UCHAR HalpInterruptToIrql[32] = {
    12, 12, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18, 19, 19,
    20, 20, 21, 21, 22, 23, 24, 25, 25, 25, 24, 25, 25, 25, 26, 27 };

VOID HalpSetGcMask(ULONG mask)
{
    if (!HalpIoBase) return;
    HalpGcShadowMask = mask;
    MmioWrite32(GC_INT_MASK, mask);
}

static VOID HalpInitPriorityMask(VOID)
{
    for (ULONG irql = 0; irql < 32; irql++) {
        ULONG m = 0;
        for (ULONG bit = 0; bit < 32; bit++)
            if (HalpInterruptToIrql[bit] > irql) m |= 1u << bit;
        HalpIrqlToMask[irql] = m;
    }
}

DEFINE_DESC(HalpExternalInterrupt);
DEFINE_DESC(HalpDecrementerInterrupt);

/* Machine check.  On this machine a PCI master abort (a probe of an address nobody decodes)
 * raises TEA, and the 604 model reports the faulting load or store itself in SRR0.  A driver
 * probing for hardware expects such a read to return all-ones, so: complete the access that
 * way, step past it, and let the kernel resume.  Only the volatile registers are in the trap
 * frame; a target register above r12 cannot be fixed up and ends in a bugcheck. */
static ULONG HalpMachineCheckCount;

BOOLEAN HalpMachineCheck(PKINTERRUPT Interrupt, PVOID ServiceContext, PVOID TrapFrame)
{
    UNREFERENCED_PARAMETER(Interrupt); UNREFERENCED_PARAMETER(ServiceContext);
    ULONG iar = TR_FIELD(TrapFrame, TR_IAR);
    ULONG insn = *(volatile ULONG *)iar;
    ULONG op = insn >> 26, rt = (insn >> 21) & 31, xo = (insn >> 1) & 0x3FF;
    LONG width = 0;      /* bytes loaded; 0 = store; -1 = unknown */
    BOOLEAN load = FALSE, sext = FALSE;
    switch (op) {
    case 32: load = TRUE; width = 4; break;               /* lwz */
    case 34: load = TRUE; width = 1; break;               /* lbz */
    case 40: load = TRUE; width = 2; break;               /* lhz */
    case 42: load = TRUE; width = 2; sext = TRUE; break;  /* lha */
    case 36: case 38: case 44: width = 0; break;          /* stw stb sth */
    case 31:
        switch (xo) {
        case 23: load = TRUE; width = 4; break;           /* lwzx */
        case 87: load = TRUE; width = 1; break;           /* lbzx */
        case 279: load = TRUE; width = 2; break;          /* lhzx */
        case 343: load = TRUE; width = 2; sext = TRUE; break; /* lhax */
        case 534: load = TRUE; width = 4; break;          /* lwbrx */
        case 790: load = TRUE; width = 2; break;          /* lhbrx */
        case 151: case 215: case 407: case 662: case 918: width = 0; break; /* stwx stbx sthx stwbrx sthbrx */
        default: width = -1; break;
        }
        break;
    default: width = -1; break;
    }
    if (width < 0 || (load && rt > 12)) {
        HalpPrint("\nHAL: machine check at %x (insn %x) cannot be completed\n", iar, insn);
        KeBugCheckEx(0x2E /* DATA_BUS_ERROR */, iar, insn, 0, TR_FIELD(TrapFrame, TR_LR));
    }
    if (load) {
        ULONG v = width == 4 ? 0xFFFFFFFFu : width == 2 ? (sext ? 0xFFFFFFFFu : 0xFFFFu) : 0xFFu;
        TR_GPR(TrapFrame, rt) = v;
    }
    TR_FIELD(TrapFrame, TR_IAR) = iar + 4;
    if (HalpMachineCheckCount++ < 8)
        HalpPrint("HAL: machine check (PCI master abort) at %x, insn %x — completed with all-ones\n", iar, insn);
    return TRUE;
}
DEFINE_DESC(HalpMachineCheck);

VOID HalpInitializeInterrupts(VOID)
{
    HalpDisableInterrupts();
    HalpInitPriorityMask();
    HalpRegisteredInterrupts = 0;
    MmioWrite32(GC_INT_MASK, 0);
    MmioWrite32(GC_INT_CLEAR, 0xFFFFFFFFu);
    PCR->ReservedVectors |= 1u << EXTERNAL_INTERRUPT_VECTOR;
    PCR->InterruptRoutine[EXTERNAL_INTERRUPT_VECTOR] = DESC(HalpExternalInterrupt);
    PCR->InterruptRoutine[DECREMENT_VECTOR] = DESC(HalpDecrementerInterrupt);
    PCR->InterruptRoutine[MACHINE_CHECK_VECTOR] = DESC(HalpMachineCheck);
    PCR->ReservedVectors |= 1u << MACHINE_CHECK_VECTOR;
    extern VOID HalpInitializeClock(VOID);
    HalpInitializeClock();
}

BOOLEAN HalEnableSystemInterrupt(ULONG Vector, KIRQL Irql, KINTERRUPT_MODE Mode)
{
    UNREFERENCED_PARAMETER(Irql); UNREFERENCED_PARAMETER(Mode);
    HalpPrint("HAL: EnableSystemInterrupt vector %d irql %d\n", Vector, Irql);
    if (Vector < DEVICE_VECTORS || Vector >= DEVICE_VECTORS + 32) return FALSE;
    ULONG bit = 1u << (Vector - DEVICE_VECTORS);
    KIRQL old; KeRaiseIrql(HIGH_LEVEL, &old);
    HalpRegisteredInterrupts |= bit;
    MmioWrite32(GC_INT_CLEAR, bit);
    HalpSetGcMask(HalpIrqlToMask[old] & HalpRegisteredInterrupts);
    KeLowerIrql(old);
    return TRUE;
}

VOID HalDisableSystemInterrupt(ULONG Vector, KIRQL Irql)
{
    UNREFERENCED_PARAMETER(Irql);
    if (Vector < DEVICE_VECTORS || Vector >= DEVICE_VECTORS + 32) return;
    ULONG bit = 1u << (Vector - DEVICE_VECTORS);
    KIRQL old; KeRaiseIrql(HIGH_LEVEL, &old);
    HalpRegisteredInterrupts &= ~bit;
    HalpSetGcMask(HalpIrqlToMask[old] & HalpRegisteredInterrupts);
    KeLowerIrql(old);
}

/* Called by the kernel's external-interrupt vector with EE off. */
BOOLEAN HalpExternalInterrupt(PKINTERRUPT Interrupt, PVOID ServiceContext, PVOID TrapFrame)
{
    UNREFERENCED_PARAMETER(Interrupt); UNREFERENCED_PARAMETER(ServiceContext);
    PKPCR pcr = PCR;
    ULONG events = MmioRead32(GC_INT_EVENTS);
    ULONG mask = HalpGcShadowMask;
    ULONG pending = events & mask;
    static ULONG extCount;
    if (extCount < 12) { extCount++; HalpPrint("HAL: ext int: events %x mask %x pending %x\n", events, mask, pending); }
    if (pending == 0) {
        /* spurious: clear whatever fired while masked so the line drops */
        if (events) MmioWrite32(GC_INT_CLEAR, events & ~mask);
        return FALSE;
    }
    ULONG bit = 31 - __builtin_clz(pending);
    /* prefer the highest IRQL among the pending sources */
    for (ULONG b = 0; b < 32; b++)
        if ((pending & (1u << b)) && HalpInterruptToIrql[b] > HalpInterruptToIrql[bit]) bit = b;
    KIRQL oldIrql = pcr->CurrentIrql;
    KIRQL newIrql = HalpInterruptToIrql[bit];
    if (newIrql <= oldIrql) newIrql = oldIrql;   /* should not happen: it was masked */
    pcr->CurrentIrql = newIrql;
    ULONG cur = 1u << bit;
    HalpSetGcMask(HalpIrqlToMask[newIrql] & HalpRegisteredInterrupts & ~cur);
    MmioWrite32(GC_INT_CLEAR, cur);
    HalpEnableInterrupts();

    BOOLEAN handled = FALSE;
    if (bit == GC_IRQ_VIA1) {
        /* The VIA is the HAL's own: Cuda speaks ADB through it and no NT driver may have it. */
        HalpCudaService();
        HalpDisableInterrupts();
        pcr->CurrentIrql = oldIrql;
        HalpSetGcMask(HalpIrqlToMask[oldIrql] & HalpRegisteredInterrupts);
        return TRUE;
    }
    PVOID routine = pcr->InterruptRoutine[DEVICE_VECTORS + bit];
    if (routine != pcr->InterruptRoutine[255]) {
        /* the routine is the interrupt object's dispatch descriptor; the object starts 0x3C before it */
        PKINTERRUPT obj = (PKINTERRUPT)((PUCHAR)routine - 0x3C);
        PVOID context = *(PVOID *)((PUCHAR)obj + 0x10);
        handled = (BOOLEAN)HalpCallDesc3(routine, (ULONG)obj, (ULONG)context, (ULONG)TrapFrame);
    } else {
        HalpPrint("HAL: unexpected interrupt on GC bit %d (events %x)\n", bit, events);
        HalpRegisteredInterrupts &= ~cur;   /* nobody wants it: stop it */
    }
    HalpDisableInterrupts();
    pcr->CurrentIrql = oldIrql;
    HalpSetGcMask(HalpIrqlToMask[oldIrql] & HalpRegisteredInterrupts);
    return handled;
}

ULONG HalGetInterruptVector(INTERFACE_TYPE InterfaceType, ULONG BusNumber, ULONG BusInterruptLevel,
                            ULONG BusInterruptVector, PKIRQL Irql, PKAFFINITY Affinity)
{
    UNREFERENCED_PARAMETER(InterfaceType); UNREFERENCED_PARAMETER(BusNumber); UNREFERENCED_PARAMETER(BusInterruptVector);
    ULONG bit = BusInterruptLevel;
    HalpPrint("HAL: GetInterruptVector type %d bus %d level %d vector %d\n", InterfaceType, BusNumber, BusInterruptLevel, BusInterruptVector);
    if (bit >= 32) { *Irql = 0; *Affinity = 0; return 0; }
    *Irql = HalpInterruptToIrql[bit];
    *Affinity = 1;
    return DEVICE_VECTORS + bit;
}
