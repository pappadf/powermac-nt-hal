/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * UPSTREAM: Wack0/entii-for-workcubes, halartx/source/ (GPL-2.0, commit c9b041da).
 *   Which adapter routines may be stubs comes from reading `hwsup.c`.
 *   Rewritten from reading: no lines were copied, and the code here was written fresh
 *   for Grand Central, Bandit and the ESCC.  See PROVENANCE.md.
 *
 * dma.c — adapter objects and common buffers.  PCI bus masters on the Network Server address
 * memory directly (Bandit is coherent with the 604), so map registers are identity and a
 * common buffer is contiguous non-paged memory. */
#include "hal.h"

typedef struct _ADAPTER_OBJECT { CSHORT Type, Size; ULONG MapRegistersPerChannel; BOOLEAN Master; } ADAPTER_OBJECT;
static ADAPTER_OBJECT HalpMasterAdapter = { 0, sizeof(ADAPTER_OBJECT), 64, TRUE };

PADAPTER_OBJECT HalGetAdapter(PDEVICE_DESCRIPTION Desc, PULONG NumberOfMapRegisters)
{
    HalpPrint("HAL: GetAdapter type %d bus %d master %d sg %d maxlen %x\n", Desc->InterfaceType, Desc->BusNumber, Desc->Master, Desc->ScatterGather, Desc->MaximumLength);
    if (Desc->Version > DEVICE_DESCRIPTION_VERSION1) return NULL;
    if (Desc->InterfaceType != PCIBus && Desc->InterfaceType != Internal) return NULL;
    if (!Desc->Master) return NULL;             /* no slave DMA controller on this machine */
    ULONG regs = (Desc->MaximumLength + 4095) / 4096 + 1;
    if (regs > 64) regs = 64;
    *NumberOfMapRegisters = regs;
    return &HalpMasterAdapter;
}

/* A bus master on this machine needs no translation, so there is nothing to hand out and nothing
 * that can be contended for: the "map register base" is a token the driver only passes back to
 * IoMapTransfer and IoFreeMapRegisters.  Every request is therefore granted immediately, in the
 * caller's context, which is what the kernel's IoAllocateAdapterChannel wrapper expects when it
 * returns STATUS_SUCCESS without having queued anything. */
static ULONG HalpMapRegisters[1];

NTSTATUS HalAllocateAdapterChannel(PADAPTER_OBJECT Adapter, PWAIT_CONTEXT_BLOCK Wcb, ULONG NumberOfMapRegisters, PDRIVER_CONTROL ExecutionRoutine)
{
    if (NumberOfMapRegisters > Adapter->MapRegistersPerChannel) return STATUS_INSUFFICIENT_RESOURCES;
    Wcb->NumberOfMapRegisters = NumberOfMapRegisters;
    HalpCallDesc4(ExecutionRoutine, (ULONG)Wcb->DeviceObject, (ULONG)Wcb->CurrentIrp,
                  (ULONG)HalpMapRegisters, (ULONG)Wcb->DeviceContext);
    /* KeepObject / DeallocateObject / DeallocateObjectKeepRegisters all mean the same here */
    return STATUS_SUCCESS;
}

PVOID HalAllocateCommonBuffer(PADAPTER_OBJECT Adapter, ULONG Length, PPHYSICAL_ADDRESS LogicalAddress, BOOLEAN CacheEnabled)
{
    UNREFERENCED_PARAMETER(Adapter); UNREFERENCED_PARAMETER(CacheEnabled);
    PVOID va = MmAllocateContiguousMemory(Length, 0xFFFFFFFFull);
    if (!va) return NULL;
    MmGetPhysicalAddress(LogicalAddress, va);
    return va;
}

BOOLEAN HalFlushCommonBuffer(PADAPTER_OBJECT Adapter, ULONG Length, PHYSICAL_ADDRESS LogicalAddress, PVOID VirtualAddress)
{
    UNREFERENCED_PARAMETER(Adapter); UNREFERENCED_PARAMETER(Length); UNREFERENCED_PARAMETER(LogicalAddress); UNREFERENCED_PARAMETER(VirtualAddress);
    return TRUE;
}

VOID HalFreeCommonBuffer(PADAPTER_OBJECT Adapter, ULONG Length, PHYSICAL_ADDRESS LogicalAddress, PVOID VirtualAddress, BOOLEAN CacheEnabled)
{
    UNREFERENCED_PARAMETER(Adapter); UNREFERENCED_PARAMETER(Length); UNREFERENCED_PARAMETER(LogicalAddress); UNREFERENCED_PARAMETER(CacheEnabled);
    MmFreeContiguousMemory(VirtualAddress);
}

PVOID HalAllocateCrashDumpRegisters(PADAPTER_OBJECT Adapter, PULONG NumberOfMapRegisters)
{
    UNREFERENCED_PARAMETER(Adapter); UNREFERENCED_PARAMETER(NumberOfMapRegisters);
    return NULL;
}

/* PHYSICAL_ADDRESS IoMapTransfer(...) — eight-byte result through the hidden pointer in r3. */
PPHYSICAL_ADDRESS IoMapTransfer(PPHYSICAL_ADDRESS Result, PADAPTER_OBJECT Adapter, PMDL Mdl, PVOID MapRegisterBase,
                                PVOID CurrentVa, PULONG Length, BOOLEAN WriteToDevice)
{
    UNREFERENCED_PARAMETER(Adapter); UNREFERENCED_PARAMETER(MapRegisterBase); UNREFERENCED_PARAMETER(WriteToDevice);
    /* one physically contiguous run at a time, taken from the MDL's page array */
    PULONG pages = (PULONG)(Mdl + 1);
    ULONG offset = (ULONG)CurrentVa - (ULONG)Mdl->StartVa;      /* StartVa is page aligned */
    ULONG idx = offset >> 12, inpage = offset & 0xFFF;
    ULONG run = 4096 - inpage, total = Mdl->ByteOffset + Mdl->ByteCount - offset;
    while (run < *Length && run < total && idx + (run >> 12) < ((Mdl->ByteOffset + Mdl->ByteCount + 4095) >> 12) &&
           pages[idx + (run >> 12)] == pages[idx] + (run >> 12)) run += 4096;
    if (run > *Length) run = *Length;
    if (run > total) run = total;
    *Length = run;
    *Result = (PHYSICAL_ADDRESS)((pages[idx] << 12) + inpage);
    return Result;
}

BOOLEAN IoFlushAdapterBuffers(PADAPTER_OBJECT Adapter, PMDL Mdl, PVOID MapRegisterBase, PVOID CurrentVa, ULONG Length, BOOLEAN WriteToDevice)
{
    UNREFERENCED_PARAMETER(Adapter); UNREFERENCED_PARAMETER(Mdl); UNREFERENCED_PARAMETER(MapRegisterBase);
    UNREFERENCED_PARAMETER(CurrentVa); UNREFERENCED_PARAMETER(Length); UNREFERENCED_PARAMETER(WriteToDevice);
    return TRUE;
}

VOID IoFreeMapRegisters(PADAPTER_OBJECT Adapter, PVOID MapRegisterBase, ULONG NumberOfMapRegisters)
{
    UNREFERENCED_PARAMETER(Adapter); UNREFERENCED_PARAMETER(MapRegisterBase); UNREFERENCED_PARAMETER(NumberOfMapRegisters);
}

VOID IoFreeAdapterChannel(PADAPTER_OBJECT Adapter)
{
    UNREFERENCED_PARAMETER(Adapter);
}

ULONG HalReadDmaCounter(PADAPTER_OBJECT Adapter)
{
    UNREFERENCED_PARAMETER(Adapter);
    return 0;
}

ULONG HalGetDmaAlignmentRequirement(VOID)
{
    return 1;
}

/* Flush pages an I/O read brought in, so the instruction cache never sees stale code. */
VOID HalFlushIoBuffers(PMDL Mdl, BOOLEAN ReadOperation, BOOLEAN DmaOperation)
{
    UNREFERENCED_PARAMETER(DmaOperation);
    if (!ReadOperation || !(Mdl->MdlFlags & MDL_IO_PAGE_READ)) return;
    PULONG pages = (PULONG)(Mdl + 1);
    ULONG npages = (Mdl->ByteOffset + Mdl->ByteCount + 4095) >> 12;
    for (ULONG i = 0; i < npages; i++) {
        PVOID va = (PVOID)(0x80000000u + (pages[i] << 12));   /* KSEG0 */
        HalSweepDcacheRange(va, 4096);
        HalSweepIcacheRange(va, 4096);
    }
}
