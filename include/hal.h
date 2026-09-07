/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * Original work of this project, written from hardware documentation and observed
 * behaviour.  Nothing here derives from leaked Windows NT source; see CONTRIBUTING.md.
 *
 * hal.h — internal HAL interfaces, shared across the source files. */
#pragma once
#include "nt.h"
#include "ans.h"

extern ULONG HalpInitPhase;
extern ULONG HalpTimebaseFrequency;
extern ULONG HalpClockCount, HalpCurrentTimeIncrement, HalpNewTimeIncrement;
extern ULONG HalpRegisteredInterrupts;
extern ULONG HalpIrqlToMask[32];
extern const UCHAR HalpInterruptToIrql[32];

VOID HalpPrint(const char *fmt, ...);
VOID HalpSeedEnvironment(PVOID ArcDiskInformation);
VOID HalpPutChar(UCHAR c);
BOOLEAN HalpInitializeDisplay(PLOADER_PARAMETER_BLOCK LoaderBlock);
VOID HalpInitializeInterrupts(VOID);
VOID HalpSetGcMask(ULONG mask);
ULONG HalpReadTimebaseLow(VOID);
VOID HalpReadTimebase(PULARGE_INTEGER out);
ULONG HalpCallDesc3(PVOID Desc, ULONG a1, ULONG a2, ULONG a3);
ULONG HalpCallDesc4(PVOID Desc, ULONG a1, ULONG a2, ULONG a3, ULONG a4);
VOID HalpDisableInterrupts(VOID);
VOID HalpEnableInterrupts(VOID);
ULONG HalpReadMsr(VOID);
VOID HalpWriteMsr(ULONG msr);
ULONG HalpReadDec(VOID);
VOID HalpWriteDec(ULONG v);
ULONG HalpReadPvr(VOID);

/* exported HAL entry points implemented across the source files */
VOID HalDisplayString(PCHAR String);
VOID KeRaiseIrql(KIRQL NewIrql, PKIRQL OldIrql);
VOID KeLowerIrql(KIRQL NewIrql);
VOID KeStallExecutionProcessor(ULONG Microseconds);
BOOLEAN HalpDecrementerInterrupt(PKINTERRUPT Interrupt, PVOID ServiceContext, PVOID TrapFrame);
BOOLEAN HalpExternalInterrupt(PKINTERRUPT Interrupt, PVOID ServiceContext, PVOID TrapFrame);
VOID HalSweepDcacheRange(PVOID Address, ULONG Length);
VOID HalSweepIcacheRange(PVOID Address, ULONG Length);
ULONG HalGetBusData(BUS_DATA_TYPE BusDataType, ULONG BusNumber, ULONG SlotNumber, PVOID Buffer, ULONG Length);

/* Cuda and ADB (source/cuda.c): the transport, and the three entry points a keyboard port
 * driver written to maciNTosh's contract imports from the HAL. */
VOID HalpCudaInitialize(VOID);
VOID HalpCudaService(VOID);
VOID HalPxiAdbSetCallback(PVOID Callback);
VOID HalPxiAdbAutopoll(USHORT Mask);
BOOLEAN HalPxiCommandAdb(UCHAR Command, PUCHAR Data, UCHAR Length, BOOLEAN Poll);

/* Cirrus 54M30 framebuffer console (source/vga.c) */
extern BOOLEAN HalpFbActive;
extern ULONG HalpFbCols, HalpFbRows, HalpFbCol, HalpFbRow;
BOOLEAN HalpFbInit(PLOADER_PARAMETER_BLOCK LoaderBlock);
VOID HalpFbPutChar(UCHAR ch);
ULONG HalGetBusData(BUS_DATA_TYPE, ULONG, ULONG, PVOID, ULONG);
ULONG HalSetBusData(BUS_DATA_TYPE, ULONG, ULONG, PVOID, ULONG);
ULONG HalSetBusDataByOffset(BUS_DATA_TYPE, ULONG, ULONG, PVOID, ULONG, ULONG);
