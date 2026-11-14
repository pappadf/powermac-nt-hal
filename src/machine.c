/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * Original work of this project, written from hardware documentation and observed
 * behaviour.  Nothing here derives from leaked Windows NT source; see CONTRIBUTING.md.
 *
 * machine.c — which of the Bandit machines this is, and the two facts about it the HAL cannot
 * read from the hardware: how the board wires its PCI slots to Grand Central, and the
 * processor's timebase.  The ARC firmware (src/arcfw/config.c, add_root_data) leaves Open
 * Firmware's root `compatible` and the cpu node's timebase-frequency in the configuration
 * tree's root data, as "Model" and "TimebaseHz" string pairs after the veneer's own.
 *
 * Two machines are described.  The Apple Network Server 500/700 (Apple, Network Server Hardware
 * Developer Notes §4.2, §4.6.2): two Bandits, its own 53C825As at IDSEL 17 and 18 and the
 * 54M30 at 15.  And the Power Macintosh 7500 and 8500 (Apple, "Power Macintosh 7500 and 8500
 * Computers" Developer Note): one Bandit, three slots A1, B1 and C1 at IDSEL 13, 14 and 15 on
 * Grand Central's external interrupts 23, 24 and 25 -- the slot's AAPL,interrupts in Open
 * Firmware's device tree says the same -- and no on-board PCI device but Grand Central, whose
 * entry is its MESH cell's line: drivers/mesh is the one NT driver that claims Grand Central. */
#include "hal.h"

typedef struct {
    const char *Name;
    ULONG Bandits;                  /* 1: only Bandit 1 at 0xF2000000 exists */
    UCHAR Bus0Irq[32];              /* Grand Central bit per IDSEL on Bandit 1; 0 = none */
    UCHAR Bus1Irq[32];              /* ... and on Bandit 2 */
} HALP_MACHINE;

/* Grand Central bit 0 is never a slot line, so 0 can mean "no interrupt" in the tables */
static const HALP_MACHINE HalpNetworkServer = {
    "Apple Network Server", 2,
    { [13] = 23, [14] = 24, [17] = GC_IRQ_FWSCSI0, [18] = GC_IRQ_FWSCSI1 },
    { [13] = 25, [14] = 27, [15] = 28, [16] = 29 },
};
static const HALP_MACHINE HalpPowerMac8500 = {
    "Power Macintosh 7500/8500", 1,
    { [13] = 23, [14] = 24, [15] = 25, [16] = GC_IRQ_MESH },
    { 0 },
};

static const HALP_MACHINE *HalpMachine = &HalpNetworkServer;

static int HalpStrEq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static char HalpModel[48];

/* the string after `key` in the root's device-specific data, or NULL */
static const char *HalpRootString(PCONFIGURATION_COMPONENT_DATA root, const char *key)
{
    if (!root || !root->ConfigurationData || root->ComponentEntry.ConfigurationDataLength < sizeof(CM_PARTIAL_RESOURCE_LIST))
        return NULL;
    PCM_PARTIAL_RESOURCE_LIST l = (PCM_PARTIAL_RESOURCE_LIST)root->ConfigurationData;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &l->PartialDescriptors[0];
    if (l->Count < 1 || d->Type != CmResourceTypeDeviceSpecific) return NULL;
    const char *p = (const char *)(d + 1), *end = p + d->u.DeviceSpecificData.DataSize;
    if ((const char *)root->ConfigurationData + root->ComponentEntry.ConfigurationDataLength < end) return NULL;
    while (p < end && *p) {
        const char *s = p;
        while (p < end && *p) p++;
        p++;
        if (HalpStrEq(s, key)) return p < end ? p : NULL;
    }
    return NULL;
}

VOID HalpIdentifyMachine(PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    PCONFIGURATION_COMPONENT_DATA root = LoaderBlock->ConfigurationRoot;
    const char *model = HalpRootString(root, "Model");
    const char *tb = HalpRootString(root, "TimebaseHz");
    if (model) {
        ULONG i = 0;
        while (model[i] && i + 1 < sizeof HalpModel) { HalpModel[i] = model[i]; i++; }
        HalpModel[i] = 0;
        if (HalpStrEq(HalpModel, "AAPL,8500") || HalpStrEq(HalpModel, "AAPL,7500"))
            HalpMachine = &HalpPowerMac8500;
    }
    if (tb) {
        ULONG hz = 0;
        for (; *tb >= '0' && *tb <= '9'; tb++) hz = hz * 10 + (ULONG)(*tb - '0');
        if (hz >= 1000000u && hz <= 100000000u) HalpTimebaseFrequency = hz;
    }
    HalpPrint("HAL: machine %s (%s), %d Bandit%s, timebase %d Hz\n", HalpMachine->Name,
              model ? HalpModel : "no model from the firmware", HalpMachine->Bandits,
              HalpMachine->Bandits == 1 ? "" : "s", HalpTimebaseFrequency);
}

ULONG HalpBanditCount(VOID) { return HalpMachine->Bandits; }

/* The Grand Central bit a PCI device's INTA reaches, or 0xFF: the board's wiring, which the
 * device's own Interrupt Line register does not know. */
UCHAR HalpPciInterruptLine(ULONG bus, ULONG dev)
{
    if (dev >= 32 || bus >= HalpMachine->Bandits) return 0xFF;
    UCHAR irq = bus == 0 ? HalpMachine->Bus0Irq[dev] : HalpMachine->Bus1Irq[dev];
    return irq ? irq : 0xFF;
}
