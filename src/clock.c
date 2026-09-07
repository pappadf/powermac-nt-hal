/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * UPSTREAM: Wack0/entii-for-workcubes, halartx/source/ (GPL-2.0, commit c9b041da).
 *   The decrementer reload, the `KeUpdateSystemTime` call and the
 *   `KeSetTimeIncrement` use come from reading `clock.c`.
 *   Rewritten from reading: no lines were copied, and the code here was written fresh
 *   for Grand Central, Bandit and the ESCC.  See PROVENANCE.md.
 *
 * clock.c — the system clock on the 604 decrementer, stalls and the performance counter on the
 * timebase (11 MHz on the Network Server). */
#include "hal.h"

ULONG HalpTimebaseFrequency = ANS_TIMEBASE_HZ;
ULONG HalpClockCount, HalpCurrentTimeIncrement, HalpNewTimeIncrement;
static ULONG HalpTicks;

static ULONG HalpUpdateDecrementer(ULONG value)
{
    ULONG dec = HalpReadDec();
    ULONG next = dec + value;                 /* dec is negative (past zero) after the interrupt */
    if (next < value || dec > value) next = value;
    HalpWriteDec(next);
    return ~dec;
}

VOID HalpInitializeClock(VOID)
{
    HalpClockCount = (HalpTimebaseFrequency / 1000) * (MAXIMUM_INCREMENT / 10000);   /* 10 ms */
    HalpWriteDec(HalpClockCount);
}

BOOLEAN HalpDecrementerInterrupt(PKINTERRUPT Interrupt, PVOID ServiceContext, PVOID TrapFrame)
{
    UNREFERENCED_PARAMETER(Interrupt); UNREFERENCED_PARAMETER(ServiceContext);
    PKPCR pcr = PCR;
    KIRQL old = pcr->CurrentIrql;
    pcr->CurrentIrql = CLOCK2_LEVEL;
    HalpSetGcMask(HalpIrqlToMask[CLOCK2_LEVEL] & HalpRegisteredInterrupts);
    HalpUpdateDecrementer(HalpClockCount);
    HalpTicks++;
    if (HalpTicks <= 3) HalpPrint("HAL: decrementer tick %d\n", HalpTicks);
    KeUpdateSystemTime(TrapFrame, HalpCurrentTimeIncrement);
    HalpCurrentTimeIncrement = HalpNewTimeIncrement;
    pcr->CurrentIrql = old;
    HalpSetGcMask(HalpIrqlToMask[old] & HalpRegisteredInterrupts);
    return TRUE;
}

ULONG HalSetTimeIncrement(ULONG DesiredIncrement)
{
    KIRQL old; KeRaiseIrql(HIGH_LEVEL, &old);
    ULONG ms = DesiredIncrement / MINIMUM_INCREMENT;
    if (ms == 0) ms = 1;
    HalpClockCount = (HalpTimebaseFrequency / 1000) * ms;
    HalpNewTimeIncrement = ms * MINIMUM_INCREMENT;
    KeLowerIrql(old);
    return HalpNewTimeIncrement;
}

VOID KeStallExecutionProcessor(ULONG Microseconds)
{
    if (!Microseconds) return;
    ULARGE_INTEGER start, now; HalpReadTimebase(&start);
    ULONGLONG ticks = (ULONGLONG)Microseconds * (HalpTimebaseFrequency / 1000000u);   /* 11 ticks per us */
    do { HalpReadTimebase(&now); } while (now.QuadPart - start.QuadPart < ticks);
}

/* LARGE_INTEGER KeQueryPerformanceCounter(PLARGE_INTEGER Frequency) — the eight-byte result comes
 * back through the hidden pointer the caller passes in r3 (see the ABI note in nt.h). */
PLARGE_INTEGER KeQueryPerformanceCounter(PLARGE_INTEGER Result, PLARGE_INTEGER Frequency)
{
    ULARGE_INTEGER tb; HalpReadTimebase(&tb);
    if (Frequency) *Frequency = HalpTimebaseFrequency;
    *Result = (LARGE_INTEGER)tb.QuadPart;
    return Result;
}

VOID HalCalibratePerformanceCounter(volatile PLONG Number)
{
    UNREFERENCED_PARAMETER(Number);
}

VOID HalStartProfileInterrupt(KPROFILE_SOURCE Source) { UNREFERENCED_PARAMETER(Source); }
VOID HalStopProfileInterrupt(KPROFILE_SOURCE Source) { UNREFERENCED_PARAMETER(Source); }
ULONG HalSetProfileInterval(ULONG Interval) { return Interval; }
