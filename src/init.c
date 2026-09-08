/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * UPSTREAM: Wack0/entii-for-workcubes, halartx/source/ (GPL-2.0, commit c9b041da).
 *   The phase 0/1 order and the PRCB version check come from reading `init.c`.
 *   Rewritten from reading: no lines were copied, and the code here was written fresh
 *   for Grand Central, Bandit and the ESCC.  See PROVENANCE.md.
 *
 * init.c — HalInitializeProcessor and HalInitSystem for the Apple Network Server. */
#include "hal.h"

ULONG HalpInitPhase;
volatile UCHAR *HalpIoBase;
/* A version, not a build counter: the counter was maintained by hand, went stale immediately,
 * and appears in every trace and screenshot.  The marker string is here so a loaded image can
 * be found by searching memory for it. */
#define HALSHINR_VERSION "0.1"
static const char HalpVersion[] = HALSHINR_VERSION;
static const char HalpBuildTag[] = "HALSHINR-" HALSHINR_VERSION "-MARKER";

VOID HalInitializeProcessor(ULONG Number)
{
    UNREFERENCED_PARAMETER(Number);
    /* Nothing per-processor yet: the veneer/SETUPLDR left the machine in LE mode with caches on. */
}

/* The kernel's KePhase0MapIo hands out 8 MB BAT slots, three at most; the Network Server's
 * devices span 0xF2000000..0xF5FFFFFF, so the HAL programs DBAT3 itself: 256 MB of
 * cache-inhibited, guarded space at MMIO_BASE_VIRT onto MMIO_BASE_PHYS.  DBAT0 is the kernel's
 * KSEG0; DBAT1/2 stay for KePhase0MapIo.  A BAT never faults, so device access works at any IRQL. */
/* Print the ARC configuration tree the veneer built, with the resource lists of adapters and
 * controllers: this is the contract every driver's HwFindAdapter works from. */
static VOID HalpDumpConfigTree(PCONFIGURATION_COMPONENT_DATA node, ULONG depth)
{
    static const char *classes[] = { "System", "Processor", "Cache", "Adapter", "Controller", "Peripheral", "Memory" };
    for (; node; node = node->Sibling) {
        PCONFIGURATION_COMPONENT c = &node->ComponentEntry;
        HalpPrint("HAL: %s%s/%d key %x '%s' cfg %d", depth == 0 ? "" : depth == 1 ? " " : depth == 2 ? "  " : "   ",
                  c->Class < 7 ? classes[c->Class] : "?", c->Type, c->Key, c->Identifier ? c->Identifier : "", c->ConfigurationDataLength);
        if (node->ConfigurationData && c->ConfigurationDataLength >= sizeof(CM_PARTIAL_RESOURCE_LIST) &&
            (c->Class == AdapterClass || c->Class == ControllerClass)) {
            PCM_PARTIAL_RESOURCE_LIST l = (PCM_PARTIAL_RESOURCE_LIST)node->ConfigurationData;
            HalpPrint(" [v%d.%d n=%d:", l->Version, l->Revision, l->Count);
            for (ULONG i = 0; i < l->Count && i < 8; i++) {
                PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &l->PartialDescriptors[i];
                switch (d->Type) {
                case CmResourceTypePort: HalpPrint(" port %x+%x", LOW32(d->u.Port.Start), d->u.Port.Length); break;
                case CmResourceTypeMemory: HalpPrint(" mem %x+%x", LOW32(d->u.Memory.Start), d->u.Memory.Length); break;
                case CmResourceTypeInterrupt: HalpPrint(" irq lvl %d vec %d", d->u.Interrupt.Level, d->u.Interrupt.Vector); break;
                case CmResourceTypeDma: HalpPrint(" dma %d", d->u.Dma.Channel); break;
                case CmResourceTypeDeviceSpecific: HalpPrint(" devspec %d", d->u.DeviceSpecificData.DataSize); break;
                default: HalpPrint(" type%d", d->Type); break;
                }
            }
            HalpPrint("]");
        }
        HalpPrint("\n");
        if (node->Child && depth < 4) HalpDumpConfigTree(node->Child, depth + 1);
    }
}

static VOID HalpMapIo(VOID)
{
    if (HalpIoBase) return;
    ULONG upper = MMIO_BASE_VIRT | 0x1FFC | 0x2;          /* BL = 256 MB, Vs */
    ULONG lower = MMIO_BASE_PHYS | 0x20 | 0x08 | 0x2;     /* I, G, PP = read/write */
    __asm__ volatile("mtdbatu 3, %0" : : "r"(0)); __asm__ volatile("isync");
    __asm__ volatile("mtdbatl 3, %0" : : "r"(lower));
    __asm__ volatile("mtdbatu 3, %0" : : "r"(upper)); __asm__ volatile("isync");
    HalpIoBase = (volatile UCHAR *)MMIO_BASE_VIRT;
}

BOOLEAN HalInitSystem(ULONG Phase, PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    PKPRCB Prcb = PCR->Prcb;
    HalpInitPhase = Phase;

    if (Phase == 0) {
        if (Prcb->MajorVersion != PRCB_MAJOR_VERSION) {
            HalpMapIo();
            HalpPrint("HAL: PRCB version %d, expected %d\n", Prcb->MajorVersion, PRCB_MAJOR_VERSION);
            KeBugCheck(MISMATCHED_HAL);
        }
        HalpMapIo();
        HalpInitializeDisplay(LoaderBlock);
        HalpPrint("\nHAL: halshinr %s (%s) for the Apple Network Server (phase 0)\n", HalpVersion, HalpBuildTag);
        HalpPrint("HAL: I/O base %x, PVR %x, MSR %x, PCR %x, loader block %x\n",
                  (ULONG)HalpIoBase, HalpReadPvr(), HalpReadMsr(), (ULONG)PCR, (ULONG)LoaderBlock);
        HalpPrint("HAL: GC events %x mask %x levels %x\n", MmioRead32(GC_INT_EVENTS), MmioRead32(GC_INT_MASK), MmioRead32(GC_INT_LEVELS));
        HalpPrint("HAL: boot device %s\n", LoaderBlock->ArcBootDeviceName);
        HalpPrint("HAL: hal path %s, options %s\n", LoaderBlock->NtHalPathName, LoaderBlock->LoadOptions);
        HalpPrint("HAL: nt boot path %s, setup block %x\n", LoaderBlock->NtBootPathName, (ULONG)LoaderBlock->SetupLoaderBlock);
        /* The loader's ARC disk signatures: IopCreateArcNames matches these against the disks it
         * finds, and only a match turns ArcBootDeviceName into a \ArcName\ symbolic link. */
        if (LoaderBlock->ArcDiskInformation) {
            PLIST_ENTRY head = (PLIST_ENTRY)LoaderBlock->ArcDiskInformation;
            for (PLIST_ENTRY e = head->Flink; e && e != head; e = e->Flink) {
                PARC_DISK_SIGNATURE s = (PARC_DISK_SIGNATURE)e;
                HalpPrint("HAL: arc disk sig %x sum %x valid %d '%s'\n",
                          s->Signature, s->CheckSum, s->ValidPartitionTable, s->ArcName);
            }
            HalpSeedEnvironment(LoaderBlock->ArcDiskInformation);
        }
        {
            ULONG n = 0, pages = 0;
            for (PLIST_ENTRY e = LoaderBlock->MemoryDescriptorListHead.Flink; e != &LoaderBlock->MemoryDescriptorListHead; e = e->Flink) {
                PMEMORY_ALLOCATION_DESCRIPTOR m = (PMEMORY_ALLOCATION_DESCRIPTOR)e; n++; pages += m->PageCount;
            }
            HalpPrint("HAL: %d memory descriptors, %d pages; PCR irql %d, kseg0 top %x\n", n, pages, PCR->CurrentIrql, PCR->Kseg0Top);
        }

        HalpCurrentTimeIncrement = MAXIMUM_INCREMENT;
        HalpNewTimeIncrement = MAXIMUM_INCREMENT;
        KeSetTimeIncrement(MAXIMUM_INCREMENT, MINIMUM_INCREMENT);
        PCR->StallScaleFactor = 1;

        HalpDumpConfigTree(LoaderBlock->ConfigurationRoot, 0);
        extern VOID HalpFixConfigTree(PLOADER_PARAMETER_BLOCK);
        HalpFixConfigTree(LoaderBlock);
        HalpInitializeInterrupts();
        HalpCudaInitialize();
        HalpPrint("HAL: phase 0 done\n");
        return TRUE;
    }
    if (Phase == 1) {
        HalpPrint("HAL: phase 1\n");
        HalpFbInit(LoaderBlock);   /* the Cirrus console: MmMapIoSpace is available now */
        for (PLIST_ENTRY e = LoaderBlock->LoadOrderListHead.Flink; e != &LoaderBlock->LoadOrderListHead; e = e->Flink) {
            PLDR_DATA_TABLE_ENTRY m = (PLDR_DATA_TABLE_ENTRY)e;
            char name[32]; ULONG n = m->BaseDllName.Length / 2; if (n > 31) n = 31;
            for (ULONG i = 0; i < n; i++) name[i] = (char)m->BaseDllName.Buffer[i];
            name[n] = 0;
            HalpPrint("HAL: module %s at %x size %x\n", name, (ULONG)m->DllBase, m->SizeOfImage);
        }
        /* The BAT mapping from phase 0 stays: HAL code must reach its devices at any IRQL. */
        PCONFIGURATION_INFORMATION Config = IoGetConfigurationInformation();
        Config->AtDiskPrimaryAddressClaimed = TRUE;
        Config->AtDiskSecondaryAddressClaimed = TRUE;
        return TRUE;
    }
    return FALSE;
}

VOID HalReportResourceUsage(VOID)
{
}

BOOLEAN HalAllProcessorsStarted(VOID)
{
    return TRUE;
}

BOOLEAN HalStartNextProcessor(PLOADER_PARAMETER_BLOCK LoaderBlock, PKPROCESSOR_STATE ProcessorState)
{
    UNREFERENCED_PARAMETER(LoaderBlock); UNREFERENCED_PARAMETER(ProcessorState);
    return FALSE;
}

VOID HalRequestIpi(KAFFINITY Mask)
{
    UNREFERENCED_PARAMETER(Mask);
}

VOID HalReturnToFirmware(FIRMWARE_REENTRY Routine)
{
    HalpDisableInterrupts();
    HalpPrint("\nHAL: HalReturnToFirmware(%d) — halted\n", Routine);
    for (;;) { }
}
