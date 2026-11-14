/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * Original work of this project, written from hardware documentation and observed
 * behaviour.  Nothing here derives from leaked Windows NT source; see CONTRIBUTING.md.
 *
 * pci.c — PCI configuration space through the two Bandit bridges, and bus address translation.
 * Bandit type-0 config cycles: write (1 << device) | (function << 8) | (offset & 0xFC) to the
 * address port, then access the data port at (offset & 3) with the natural size.  Devices below
 * IDSEL 11 do not exist; an absent IDSEL reads all-ones. */
#include "hal.h"

/* A bus the machine does not have is answered as absent, never by touching its address: a Power
 * Macintosh 8500 has no second Bandit, and a cycle to 0xF4000000 there would be a machine check. */
static ULONG HalpBanditBase(ULONG BusNumber)
{
    if (BusNumber >= HalpBanditCount()) return 0;
    return BusNumber == 0 ? BANDIT1_BASE : BusNumber == 1 ? BANDIT2_BASE : 0;
}

static ULONG HalpPciCfgRead(ULONG base, ULONG dev, ULONG fn, ULONG off, ULONG size)
{
    MmioWrite32(base + BANDIT_CFG_ADDR, (1u << dev) | (fn << 8) | (off & 0xFC));
    ULONG p = base + BANDIT_CFG_DATA + (off & 3);
    switch (size) {
    case 1: return MmioRead8(p);
    case 2: return MmioRead16(p);
    default: return MmioRead32(p);
    }
}

static VOID HalpPciCfgWrite(ULONG base, ULONG dev, ULONG fn, ULONG off, ULONG size, ULONG v)
{
    MmioWrite32(base + BANDIT_CFG_ADDR, (1u << dev) | (fn << 8) | (off & 0xFC));
    ULONG p = base + BANDIT_CFG_DATA + (off & 3);
    switch (size) {
    case 1: MmioWrite8(p, (UCHAR)v); break;
    case 2: MmioWrite16(p, (USHORT)v); break;
    default: MmioWrite32(p, v); break;
    }
}

/* copy Length bytes of config space at Offset, honouring natural alignment.  Returns 0 only
 * when the bus does not exist; an empty slot returns 2 bytes of vendor id 0xFFFF, which is how
 * the kernel and scsiport tell "no device here" from "no more buses". */
static ULONG HalpPciAccess(ULONG BusNumber, ULONG SlotNumber, PUCHAR Buffer, ULONG Offset, ULONG Length, BOOLEAN Write)
{
    ULONG base = HalpBanditBase(BusNumber);
    ULONG dev = SlotNumber & 0x1F, fn = (SlotNumber >> 5) & 7;
    if (!base) return 0;
    if (Offset >= 256) return 0;
    if (Offset + Length > 256) Length = 256 - Offset;
    BOOLEAN absent = dev < PCI_MIN_IDSEL_DEVICE || dev > 31 || (fn != 0) ||
                     (HalpPciCfgRead(base, dev, fn, 0, 4) & 0xFFFF) == 0xFFFF;
    if (absent) {
        if (Write) return 0;
        if (Offset == 0 && Length >= 2) { Buffer[0] = 0xFF; Buffer[1] = 0xFF; return 2; }
        return 0;
    }
    ULONG done = 0;
    while (done < Length) {
        ULONG off = Offset + done, size;
        if ((off & 3) == 0 && Length - done >= 4) size = 4;
        else if ((off & 1) == 0 && Length - done >= 2) size = 2;
        else size = 1;
        if (Write) {
            ULONG v = 0;
            for (ULONG i = 0; i < size; i++) v |= (ULONG)Buffer[done + i] << (8 * i);
            HalpPciCfgWrite(base, dev, fn, off, size, v);
        } else {
            ULONG v = HalpPciCfgRead(base, dev, fn, off, size);
            for (ULONG i = 0; i < size; i++) Buffer[done + i] = (UCHAR)(v >> (8 * i));
        }
        done += size;
    }
    if (!Write) {
        if (Offset <= 0x3C && Offset + done > 0x3C)
            Buffer[0x3C - Offset] = HalpPciInterruptLine(BusNumber, dev);   /* Interrupt Line */
        /* The shipped symc810 miniport claims the 53C810 (device id 0x0001); the Network
         * Server's controllers are 53C825As (0x0003), and so is the Symbios card a Power
         * Macintosh takes in a slot.  Report 0x0001 for every 53C825A so scsiport hands the
         * adapter to the miniport — the 825A is register-compatible for what Setup needs
         * (a Phase 2 experiment; see docs/2026-09-06-first-boot.md). */
        if (Offset <= 2 && Offset + done > 3 && HalpPciCfgRead(base, dev, fn, 0, 4) == 0x00031000u) {
            Buffer[2 - Offset] = 0x01; Buffer[3 - Offset] = 0x00;
        }
    }
    return done;
}

/* Bus calls are traced until the budget runs out, so a boot leaves a readable record of who
 * asked for what without a driver's polling loop burying it.  64 covers everything up to and
 * including the video driver claiming the Cirrus, which is where the trace last earned its
 * keep; it was 12, which stopped just before that and made the video calls look absent. */
static ULONG HalpTraceBudget = 64;
#define HALP_TRACE(...) do { if (HalpTraceBudget) { HalpTraceBudget--; HalpPrint(__VA_ARGS__); } } while (0)

ULONG HalGetBusDataByOffset(BUS_DATA_TYPE BusDataType, ULONG BusNumber, ULONG SlotNumber, PVOID Buffer, ULONG Offset, ULONG Length)
{
    if (BusDataType != PCIConfiguration) { HALP_TRACE("HAL: GetBusData type %d bus %d slot %x -> 0\n", BusDataType, BusNumber, SlotNumber); return 0; }
    ULONG n = HalpPciAccess(BusNumber, SlotNumber, (PUCHAR)Buffer, Offset, Length, FALSE);
    if (n > 4) HALP_TRACE("HAL: GetBusData PCI bus %d slot %x off %x len %d -> %d (%x)\n", BusNumber, SlotNumber, Offset, Length, n, n >= 4 ? *(PULONG)Buffer : 0);
    return n;
}

ULONG HalGetBusData(BUS_DATA_TYPE BusDataType, ULONG BusNumber, ULONG SlotNumber, PVOID Buffer, ULONG Length)
{
    return HalGetBusDataByOffset(BusDataType, BusNumber, SlotNumber, Buffer, 0, Length);
}

ULONG HalSetBusDataByOffset(BUS_DATA_TYPE BusDataType, ULONG BusNumber, ULONG SlotNumber, PVOID Buffer, ULONG Offset, ULONG Length)
{
    if (BusDataType != PCIConfiguration) return 0;
    return HalpPciAccess(BusNumber, SlotNumber, (PUCHAR)Buffer, Offset, Length, TRUE);
}

ULONG HalSetBusData(BUS_DATA_TYPE BusDataType, ULONG BusNumber, ULONG SlotNumber, PVOID Buffer, ULONG Length)
{
    return HalSetBusDataByOffset(BusDataType, BusNumber, SlotNumber, Buffer, 0, Length);
}

/* AddressSpace: 0 = memory, 1 = I/O.  PCI memory is identity-mapped by both Bandits; PCI I/O
 * space appears in CPU memory space at the bridge base, so it translates to memory space. */
BOOLEAN HalTranslateBusAddress(INTERFACE_TYPE InterfaceType, ULONG BusNumber, PHYSICAL_ADDRESS BusAddress,
                               PULONG AddressSpace, PPHYSICAL_ADDRESS TranslatedAddress)
{
    ULONG bus_lo = LOW32(BusAddress);
    HALP_TRACE("HAL: TranslateBusAddress type %d bus %d addr %x space %d\n", InterfaceType, BusNumber, bus_lo, *AddressSpace);
    switch (InterfaceType) {
    case PCIBus: {
        ULONG base = HalpBanditBase(BusNumber);
        if (!base) return FALSE;
        if (*AddressSpace == 1) {
            /* PCI I/O space: VGA's legacy range and the BARs Open Firmware assigned (0x400 up).
             * Nothing decodes the PC legacy ports below 0x3B0 here; refusing them keeps a
             * driver's probe from turning into a master abort. */
            if (bus_lo >= BANDIT_IO_SIZE || bus_lo < 0x3B0) return FALSE;
            *TranslatedAddress = base + bus_lo;
            *AddressSpace = 0;
            return TRUE;
        }
        /* The legacy VGA window.  A PCI VGA part decodes 0xA0000..0xBFFFF on the bus; the CPU
         * cannot reach PCI addresses below 0x80000000 through either Bandit, and 0xA0000 as a
         * CPU address is ordinary RAM here.  So the window is answered inside the 54M30's BAR0,
         * at its LEGACY-WINDOW MIRROR: 128 KB where the part decodes banked display memory and,
         * at $B8000, the memory-mapped BitBLT registers cirrus.dll draws with
         * (docs/2026-09-17-the-bitblt-engine.md).  Raw VRAM was an earlier answer, and it put
         * the driver's register writes into display memory as pixels.
         *
         * Two bus ranges mean this window.  0xA0000 is what Microsoft's cirrus.sys asks for, in
         * its access-range table (VideoPortVerifyAccessRanges has IoReportResourceUsage translate
         * both ends of every range; a refusal is STATUS_INVALID_PARAMETER and Setup's "fatal
         * error while initializing your computer's video", wall 25) and again by name for the
         * window it maps.  VGA_APERTURE_MOVED is where ledger row 6 relocated that table entry
         * while the HAL still refused 0xA0000 as "RAM"; floppies built with that patched driver
         * exist, so it stays accepted.  The refusal was wrong about this range: the translated
         * address is VRAM, never RAM, and a stock cirrus.sys boots to the desktop through it
         * (docs/2026-11-05-the-vga-window.md). */
        if ((bus_lo >= VGA_APERTURE_MOVED && bus_lo < VGA_APERTURE_MOVED + 0x20000u) ||
            (bus_lo >= 0xA0000u && bus_lo < 0xC0000u)) {
            ULONG vram = HalpVgaVramPhys();
            if (!vram) return FALSE;
            *TranslatedAddress = vram + VGA_APERTURE_MIRROR + (bus_lo & 0x1FFFFu);
            HALP_TRACE("HAL: legacy VGA window %x -> 54M30 mirror %x\n", bus_lo, LOW32(*TranslatedAddress));
            return TRUE;
        }
        /* Other PCI memory is identity-mapped, but only above the Bandit windows; anything else
         * below 0x80000000 is ordinary RAM here, and handing it back would let a driver write
         * over the kernel. */
        if (bus_lo < 0x80000000u) return FALSE;
        *TranslatedAddress = bus_lo;
        return TRUE;
    }
    case Internal:
    case Isa:
        /* There is no ISA bus.  Drivers that probe legacy ports (PCMCIA at 0x3E0, i8042 at 0x60,
         * floppy at 0x3F0, COM ports) must be told so, not sent into Bandit's PCI I/O window
         * where a master abort becomes a machine check.  The one legacy range a PCI VGA chip
         * (the 54M30) does decode is 0x3B0..0x3DF; that goes to Bandit 1's I/O space. */
        if (*AddressSpace == 1) {
            if (bus_lo >= 0x3B0 && bus_lo <= 0x3DF) {
                *TranslatedAddress = BANDIT1_BASE + bus_lo;
                *AddressSpace = 0;
                return TRUE;
            }
            return FALSE;
        }
        if (bus_lo < 0x80000000u) return FALSE;
        *TranslatedAddress = bus_lo;
        return TRUE;
    default:
        return FALSE;
    }
}

NTSTATUS HalAdjustResourceList(PIO_RESOURCE_REQUIREMENTS_LIST *List)
{
    UNREFERENCED_PARAMETER(List);
    return STATUS_SUCCESS;
}

/* Read one BAR and size it: returns the assigned base (with flag bits) and, in *len, the span. */
static ULONG HalpSizeBar(ULONG bus, ULONG dev, ULONG bar, PULONG len)
{
    UCHAR b[4]; ULONG off = 0x10 + bar * 4;
    HalGetBusDataByOffset(PCIConfiguration, bus, dev, b, off, 4);
    ULONG orig = b[0] | (b[1] << 8) | (b[2] << 16) | ((ULONG)b[3] << 24);
    if (orig == 0) { *len = 0; return 0; }
    UCHAR ff[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    HalSetBusDataByOffset(PCIConfiguration, bus, dev, ff, off, 4);
    HalGetBusDataByOffset(PCIConfiguration, bus, dev, b, off, 4);
    ULONG size = b[0] | (b[1] << 8) | (b[2] << 16) | ((ULONG)b[3] << 24);
    UCHAR ob[4] = { (UCHAR)orig, (UCHAR)(orig >> 8), (UCHAR)(orig >> 16), (UCHAR)(orig >> 24) };
    HalSetBusDataByOffset(PCIConfiguration, bus, dev, ob, off, 4);   /* restore */
    ULONG mask = (orig & 1) ? ~0x3u : ~0xFu;
    *len = (~(size & mask)) + 1;
    return orig;
}

/* Build the CM_RESOURCE_LIST a PCI driver (scsiport for the 53C825A) needs: the device's BARs as
 * port/memory ranges plus its interrupt, from real config space and the ANS interrupt routing. */
NTSTATUS HalAssignSlotResources(PUNICODE_STRING RegistryPath, PUNICODE_STRING DriverClassName, PDRIVER_OBJECT DriverObject,
                                PDEVICE_OBJECT DeviceObject, INTERFACE_TYPE BusType, ULONG BusNumber, ULONG SlotNumber,
                                PCM_RESOURCE_LIST *AllocatedResources)
{
    UNREFERENCED_PARAMETER(RegistryPath); UNREFERENCED_PARAMETER(DriverClassName); UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(DeviceObject);
    if (BusType != PCIBus) return STATUS_NOT_SUPPORTED;
    ULONG dev = SlotNumber & 0x1F, fn = (SlotNumber >> 5) & 7;
    UCHAR cfg[4];
    if (HalGetBusData(PCIConfiguration, BusNumber, SlotNumber, cfg, 4) < 4 ||
        (cfg[0] | (cfg[1] << 8)) == 0xFFFF)
        return STATUS_NO_SUCH_DEVICE;

    CM_PARTIAL_RESOURCE_DESCRIPTOR desc[7]; ULONG n = 0;
    for (ULONG bar = 0; bar < 6 && n < 6; bar++) {
        ULONG len, v = HalpSizeBar(BusNumber, dev, bar, &len);
        if (len == 0) continue;
        if (v & 1) {   /* I/O */
            desc[n].Type = CmResourceTypePort; desc[n].ShareDisposition = CmResourceShareDeviceExclusive;
            desc[n].Flags = CM_RESOURCE_PORT_IO; desc[n].u.Port.Start = (PHYSICAL_ADDRESS)(v & ~0x3u); desc[n].u.Port.Length = len;
        } else {       /* memory */
            desc[n].Type = CmResourceTypeMemory; desc[n].ShareDisposition = CmResourceShareDeviceExclusive;
            desc[n].Flags = CM_RESOURCE_MEMORY_READ_WRITE; desc[n].u.Memory.Start = (PHYSICAL_ADDRESS)(v & ~0xFu); desc[n].u.Memory.Length = len;
            if (v & 0x4) bar++;   /* 64-bit BAR consumes the next slot */
        }
        n++;
    }
    UCHAR irq = HalpPciInterruptLine(BusNumber, dev);
    if (irq != 0xFF) {
        desc[n].Type = CmResourceTypeInterrupt; desc[n].ShareDisposition = CmResourceShareDeviceExclusive;
        desc[n].Flags = CM_RESOURCE_INTERRUPT_LEVEL_SENSITIVE;
        desc[n].u.Interrupt.Level = irq; desc[n].u.Interrupt.Vector = irq; desc[n].u.Interrupt.Affinity = 1;
        n++;
    }

    ULONG size = sizeof(CM_RESOURCE_LIST_REAL) + (n ? n - 1 : 0) * sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR);
    PCM_RESOURCE_LIST_REAL list = (PCM_RESOURCE_LIST_REAL)ExAllocatePool(NonPagedPool, size);
    if (!list) return STATUS_INSUFFICIENT_RESOURCES;
    memset(list, 0, size);
    list->Count = 1;
    list->List[0].InterfaceType = PCIBus;
    list->List[0].BusNumber = BusNumber;
    list->List[0].PartialResourceList.Version = 1;
    list->List[0].PartialResourceList.Revision = 1;
    list->List[0].PartialResourceList.Count = n;
    for (ULONG i = 0; i < n; i++) list->List[0].PartialResourceList.PartialDescriptors[i] = desc[i];
    *AllocatedResources = (PCM_RESOURCE_LIST)list;
    HalpPrint("HAL: HalAssignSlotResources bus %d dev %d fn %d -> %d resources\n", BusNumber, dev, fn, n);
    return STATUS_SUCCESS;
}
