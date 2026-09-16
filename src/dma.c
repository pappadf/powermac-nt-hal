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
    ULONG limit = Mdl->ByteOffset + Mdl->ByteCount;

    /* CurrentVa is "a virtual address in the buffer", and which address space it is in depends
     * on the caller: an MDL built for a user buffer has StartVa in user space and
     * MappedSystemVa in kernel space, and a driver that took MmGetSystemAddressForMdl hands us
     * the kernel one.  Subtracting the wrong base would give a wild page index and send the
     * transfer to a physical address from elsewhere in the page array, so take whichever base
     * puts CurrentVa inside the buffer, and refuse loudly if neither does.
     *
     * This is a latent bug found while chasing wall 50, NOT its cause: the complaint below has
     * never fired on this machine.  The corruption was the contiguity check further down. */
    ULONG offset = (ULONG)CurrentVa - (ULONG)Mdl->StartVa;      /* StartVa is page aligned */
    if (offset > limit && Mdl->MappedSystemVa) {
        ULONG alt = (ULONG)CurrentVa - (ULONG)Mdl->MappedSystemVa;
        if (alt <= limit) offset = alt;
    }
    if (offset > limit) {                                       /* neither: refuse, loudly */
        static ULONG complained;
        if (complained++ < 8)
            HalpPrint("HAL: IoMapTransfer: CurrentVa %x outside MDL (StartVa %x sysva %x "
                      "off %x count %x)\n", CurrentVa, Mdl->StartVa, Mdl->MappedSystemVa,
                      Mdl->ByteOffset, Mdl->ByteCount);
        *Length = 0;
        *Result = (PHYSICAL_ADDRESS)0;
        return Result;
    }
    ULONG idx = offset >> 12, inpage = offset & 0xFFF;
    ULONG run = 4096 - inpage, total = limit - offset, npages = (limit + 4095) >> 12;
    /* Extend the run only over pages we have actually checked are contiguous.  The page a run of
     * `run` bytes would grow into is at index (run + inpage) >> 12, NOT run >> 12: for a buffer
     * that does not start on a page boundary the latter is one page behind, so the first
     * extension compares pages[idx] with itself, passes, and swallows the next page unchecked.
     * The chip is then told "one contiguous run" across two pages that are not, and every byte
     * past the first page boundary is read from — or written to — whatever else lives at the
     * adjacent physical page.  That is how blocks of one file ended up inside the cached FAT
     * page and from there on disk, truncating ~70 files of a fresh install (STORY.md wall 50).
     * It only bit non-page-aligned buffers, which is why most I/O was fine. */
    for (;;) {
        ULONG next = (run + inpage) >> 12;              /* the page this run would grow into */
        if (run >= *Length || run >= total) break;
        if (idx + next >= npages) break;
        if (pages[idx + next] != pages[idx] + next) break;
        run += 4096;
    }
    if (run > *Length) run = *Length;
    if (run > total) run = total;
    *Length = run;
    *Result = (PHYSICAL_ADDRESS)((pages[idx] << 12) + inpage);
    return Result;
}

/* We hand drivers the buffer's own physical addresses rather than map registers, so there is
 * nothing to copy back when a transfer finishes. */
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

/* Flush pages an I/O read brought in, so the instruction cache never sees stale code.
 *
 * Sweeping the data cache around every DMA was tried too, on the theory that a dirty line could
 * reach the disk stale (STORY.md wall 50).  It changed nothing: Bandit is coherent with the
 * 604, as the file header says, and the corruption had another cause entirely. */
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
