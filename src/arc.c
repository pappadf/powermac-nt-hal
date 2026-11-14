/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * Original work of this project, written from hardware documentation and observed
 * behaviour.  Nothing here derives from leaked Windows NT source; see CONTRIBUTING.md.
 *
 * arc.c — repairs to the ARC configuration tree the veneer hands the kernel.
 *
 * The veneer converts each Open Firmware SCSI node into an ARC ScsiAdapter component but its
 * resource list is unusable (one port descriptor starting at 0, no interrupt), so the miniport
 * never finds the controller.  Before the kernel builds the hardware registry from this tree
 * (Phase 1), give every ScsiAdapter under the PCI adapter a list built from the device's real
 * configuration space and the Network Server's interrupt routing.  The lists live in HAL data so
 * they stay valid for the kernel's registry pass. */
#include "hal.h"

typedef struct { CM_PARTIAL_RESOURCE_LIST List; CM_PARTIAL_RESOURCE_DESCRIPTOR More[3]; } HALP_SCSI_RESOURCES;
static HALP_SCSI_RESOURCES HalpScsiResources[2];

/* The n-th 53C8xx on the PCI buses, in IDSEL order, which is the order the firmware numbers the
 * NCRC810 adapters in: the Network Server's own pair at IDSEL 17 and 18, or a card in a Power
 * Macintosh's slot.  Returns the bus and device, FALSE when there are fewer. */
static BOOLEAN HalpFindScsiDevice(ULONG index, PULONG bus, PULONG dev)
{
    for (ULONG b = 0; b < HalpBanditCount(); b++)
        for (ULONG d = PCI_MIN_IDSEL_DEVICE; d < 32; d++) {
            ULONG id = 0;
            if (HalGetBusDataByOffset(PCIConfiguration, b, d, &id, 0, 4) < 4) continue;
            if (id != 0x00031000u && id != 0x00011000u) continue;    /* 53C825A (or 53C810) */
            if (index-- == 0) { *bus = b; *dev = d; return TRUE; }
        }
    return FALSE;
}

static BOOLEAN HalpFixScsiAdapter(PCONFIGURATION_COMPONENT_DATA node, ULONG index)
{
    ULONG bus, dev;
    if (index >= 2 || !HalpFindScsiDevice(index, &bus, &dev)) return FALSE;
    UCHAR irq = HalpPciInterruptLine(bus, dev);
    if (irq == 0xFF) return FALSE;
    UCHAR cfg[64];
    if (HalGetBusData(PCIConfiguration, bus, dev, cfg, sizeof cfg) < 64) return FALSE;
    ULONG vendev = *(PULONG)&cfg[0];
    if (vendev != 0x00031000u && vendev != 0x00011000u) return FALSE;   /* 53C825A or 53C810 */
    ULONG bar0 = *(PULONG)&cfg[0x10], bar1 = *(PULONG)&cfg[0x14];
    HALP_SCSI_RESOURCES *r = &HalpScsiResources[index];
    memset(r, 0, sizeof *r);
    r->List.Version = 1; r->List.Revision = 1; r->List.Count = 0;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR d = r->List.PartialDescriptors;
    if (bar1 & 1) { ULONG t = bar0; bar0 = bar1; bar1 = t; }        /* bar0 = I/O, bar1 = memory */
    /* memory-mapped registers first: that is what the driver maps */
    d->Type = CmResourceTypeMemory; d->ShareDisposition = 1 /* DeviceExclusive */; d->Flags = 0 /* read/write */;
    d->u.Memory.Start = (PHYSICAL_ADDRESS)(bar1 & ~0xFu); d->u.Memory.Length = 0x100; d++; r->List.Count++;
    d->Type = CmResourceTypePort; d->ShareDisposition = 1; d->Flags = 1 /* CM_RESOURCE_PORT_IO */;
    d->u.Port.Start = (PHYSICAL_ADDRESS)(bar0 & ~0x3u); d->u.Port.Length = 0x100; d++; r->List.Count++;
    d->Type = CmResourceTypeInterrupt; d->ShareDisposition = 1; d->Flags = 0 /* level sensitive */;
    d->u.Interrupt.Level = irq; d->u.Interrupt.Vector = irq; d->u.Interrupt.Affinity = 1; d++; r->List.Count++;
    node->ConfigurationData = r;
    node->ComponentEntry.ConfigurationDataLength = sizeof(CM_PARTIAL_RESOURCE_LIST) + (r->List.Count - 1) * sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR);
    HalpPrint("HAL: ScsiAdapter %d (%s) at bus %d device %d -> mem %x io %x irq %d\n", index, node->ComponentEntry.Identifier,
              bus, dev, LOW32(r->List.PartialDescriptors[0].u.Memory.Start), LOW32(r->List.PartialDescriptors[1].u.Port.Start), irq);
    return TRUE;
}

static VOID HalpWalk(PCONFIGURATION_COMPONENT_DATA node, PULONG scsiIndex)
{
    for (; node; node = node->Sibling) {
        PCONFIGURATION_COMPONENT c = &node->ComponentEntry;
        if (c->Class == AdapterClass && c->Type == ScsiAdapter && c->Identifier && c->Identifier[0] == 'N') {
            HalpFixScsiAdapter(node, (*scsiIndex)++);
        }
        if (node->Child) HalpWalk(node->Child, scsiIndex);
    }
}

VOID HalpFixConfigTree(PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    ULONG idx = 0;
    if (LoaderBlock->ConfigurationRoot) HalpWalk(LoaderBlock->ConfigurationRoot, &idx);
}
