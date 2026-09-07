/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * Original work of this project, written from hardware documentation and observed
 * behaviour.  Nothing here derives from leaked Windows NT source; see CONTRIBUTING.md.
 *
 *
 * disk.c — the three partition-table routines a NT 4.0 HAL exports.
 *
 * On NT 4.0 the partition table lives on the HAL side of the boundary: disk.sys, ftdisk and
 * text-mode Setup all reach the MBR through HAL exports, never by reading sector 0 themselves.
 * Setup's "No valid system partitions are defined on this computer" screen is what a stubbed
 * IoReadPartitionTable looks like from the outside — every disk reads back as having no
 * partitions at all, so there is nothing for Setup to install onto.
 *
 * Everything here is plain MBR parsing.  The only machine-specific part is that PowerPC NT runs
 * the 604 in little-endian mode, where a misaligned load traps instead of being fixed up in
 * hardware, so the on-disk 32-bit fields (which sit at odd offsets inside a 16-byte entry) are
 * assembled a byte at a time.
 */
#include "nt.h"
#include "hal.h"

#define NonPagedPoolCacheAligned 4
#define SECTOR_MAX      4096u
#define PARTITION_MAX   64u             /* what we are willing to report for one disk */
#define MBR_TABLE       0x1beu          /* the four 16-byte entries */
#define MBR_SIGNATURE   0x1b8u          /* NT's per-disk signature, written by Disk Administrator */
#define MBR_MAGIC       0x1feu

/* one 16-byte on-disk entry */
#define PTE_BOOT        0x0u
#define PTE_TYPE        0x4u
#define PTE_START       0x8u            /* relative sector, LBA */
#define PTE_LENGTH      0xcu            /* length in sectors */

/* The pointers below are volatile on purpose.  Assembling a little-endian ULONG out of four byte
 * loads is exactly the idiom clang recognises and folds back into a single `lwz` — which is the
 * misaligned load this code exists to avoid, and which traps on a 604 running little-endian.
 * volatile is what stops the fold; -O0 on the whole file would do too, less locally. */
static ULONG HalpGetUlong(const volatile UCHAR *p)
{
    return (ULONG)p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24);
}

/* Sector numbers come out of a byte offset; the division is 64-bit and a freestanding build
 * has no __udivdi3, so it goes through the HAL's own long division (misc.c). */
static ULONG HalpDivBySector(LARGE_INTEGER Offset, ULONG SectorSize)
{
    return (ULONG)HalpDivU64((ULONGLONG)Offset, SectorSize);
}

static VOID HalpPutUlong(volatile UCHAR *p, ULONG v)
{
    p[0] = (UCHAR)v; p[1] = (UCHAR)(v >> 8); p[2] = (UCHAR)(v >> 16); p[3] = (UCHAR)(v >> 24);
}

/* NT's IsRecognizedPartition, spelled out: a file-system type, with the fault-tolerance bits
 * (0x80 "part of an FT set", 0x40 "needs recovery") masked off before the comparison. */
static BOOLEAN HalpRecognized(UCHAR type)
{
    UCHAR base = (type & PARTITION_NTFT) ? (UCHAR)(type & ~VALID_NTFT) : type;
    return (BOOLEAN)(base == PARTITION_FAT_12 || base == PARTITION_FAT_16 ||
                     base == PARTITION_HUGE   || base == PARTITION_IFS   ||
                     base == PARTITION_FAT32  || base == PARTITION_FAT32_XINT13 ||
                     base == PARTITION_XINT13);
}

static BOOLEAN HalpExtended(UCHAR type)
{
    return (BOOLEAN)(type == PARTITION_EXTENDED || type == PARTITION_XINT13_EXTENDED);
}

/* Read or write one sector through the disk driver.  IoBuildSynchronousFsdRequest wants a
 * PASSIVE_LEVEL caller in a real thread; every caller of these three exports is one. */
static NTSTATUS HalpDiskIo(PDEVICE_OBJECT DeviceObject, ULONG SectorSize, ULONG Lba,
                           PVOID Buffer, BOOLEAN Write)
{
    KEVENT event;
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER offset = (LARGE_INTEGER)(ULONGLONG)Lba * SectorSize;
    PIRP irp;
    NTSTATUS status;

    iosb.Status = STATUS_UNSUCCESSFUL; iosb.Information = 0;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    irp = IoBuildSynchronousFsdRequest(Write ? IRP_MJ_WRITE : IRP_MJ_READ, DeviceObject,
                                       Buffer, SectorSize, &offset, &event, &iosb);
    if (irp == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    status = IoCallDriver(DeviceObject, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }
    return status;
}

/* ---- IoReadPartitionTable -------------------------------------------------------------------
 * Walks the MBR and, behind it, the extended chain.  Entries come back in the order NT numbers
 * them: the four primary slots in table order, then each logical drive in chain order.
 */
NTSTATUS IoReadPartitionTable(PDEVICE_OBJECT DeviceObject, ULONG SectorSize,
                              BOOLEAN ReturnRecognizedPartitions, PDRIVE_LAYOUT_INFORMATION *Layout)
{
    PUCHAR sector;
    PDRIVE_LAYOUT_INFORMATION layout;
    NTSTATUS status;
    ULONG count = 0, number = 0;
    ULONG extendedStart = 0;            /* LBA of the outermost extended partition, 0 = none */
    ULONG next = 0;                     /* LBA of the EBR to read on the next pass */
    ULONG signature = 0;
    BOOLEAN first = TRUE;
    ULONG passes = 0;

    *Layout = NULL;
    if (SectorSize < 512 || SectorSize > SECTOR_MAX) return STATUS_INVALID_PARAMETER;

    sector = ExAllocatePool(NonPagedPoolCacheAligned, SectorSize);
    layout = ExAllocatePool(NonPagedPool,
                            sizeof(DRIVE_LAYOUT_INFORMATION) +
                            (PARTITION_MAX - 1) * sizeof(PARTITION_INFORMATION));
    if (sector == NULL || layout == NULL) {
        if (sector) ExFreePool(sector);
        if (layout) ExFreePool(layout);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    memset(layout, 0, sizeof(DRIVE_LAYOUT_INFORMATION) +
                      (PARTITION_MAX - 1) * sizeof(PARTITION_INFORMATION));

    for (;;) {
        ULONG base = next;              /* LBA of the sector holding this table */
        ULONG i;
        BOOLEAN sawNext = FALSE;

        status = HalpDiskIo(DeviceObject, SectorSize, base, sector, FALSE);
        if (!NT_SUCCESS(status)) {
            HalpPrint("HAL: IoReadPartitionTable: read of sector %d failed %x\n", base, status);
            if (first) { ExFreePool(sector); ExFreePool(layout); return status; }
            break;                      /* a broken chain still yields the partitions we have */
        }
        if (sector[MBR_MAGIC] != 0x55 || sector[MBR_MAGIC + 1] != 0xaa) {
            if (first) HalpPrint("HAL: IoReadPartitionTable: no MBR signature (%x %x)\n",
                                 sector[MBR_MAGIC], sector[MBR_MAGIC + 1]);
            break;
        }
        if (first) signature = HalpGetUlong(sector + MBR_SIGNATURE);

        for (i = 0; i < 4 && count < PARTITION_MAX; i++) {
            PUCHAR e = sector + MBR_TABLE + i * 16;
            UCHAR type = e[PTE_TYPE];
            ULONG rel = HalpGetUlong(e + PTE_START);
            ULONG len = HalpGetUlong(e + PTE_LENGTH);
            PPARTITION_INFORMATION p;

            /* A primary entry counts from sector 0 and a logical one from its own EBR, but the
             * links that stitch the chain together all count from the outermost extended
             * partition.  That asymmetry is the whole trick of an extended chain. */
            ULONG absolute = base + rel;

            if (HalpExtended(type)) {
                if (first) extendedStart = rel;
                else absolute = extendedStart + rel;
                next = absolute;
                sawNext = TRUE;
                if (ReturnRecognizedPartitions) continue;
            } else if (type == PARTITION_ENTRY_UNUSED || len == 0) {
                if (ReturnRecognizedPartitions) continue;
            }

            p = &layout->PartitionEntry[count++];
            p->StartingOffset  = (LARGE_INTEGER)(ULONGLONG)absolute * SectorSize;
            p->PartitionLength = (LARGE_INTEGER)(ULONGLONG)len * SectorSize;
            p->HiddenSectors   = rel;
            p->PartitionType   = type;
            p->BootIndicator   = (BOOLEAN)((e[PTE_BOOT] & 0x80) != 0);
            p->RecognizedPartition = HalpRecognized(type);
            p->RewritePartition = FALSE;
            p->PartitionNumber = p->RecognizedPartition ? ++number : 0;
        }
        first = FALSE;
        if (!sawNext || next == base || count >= PARTITION_MAX) break;
        if (++passes > PARTITION_MAX) break;    /* a chain that loops back on itself */
    }

    layout->PartitionCount = count;
    layout->Signature = signature;
    ExFreePool(sector);
    *Layout = layout;
    HalpPrint("HAL: IoReadPartitionTable: %d partitions, signature %x\n", count, signature);
    for (ULONG i = 0; i < count; i++)
        HalpPrint("HAL:   #%d type %x start %x len %x%s\n", layout->PartitionEntry[i].PartitionNumber,
                  layout->PartitionEntry[i].PartitionType,
                  HalpDivBySector(layout->PartitionEntry[i].StartingOffset, SectorSize),
                  HalpDivBySector(layout->PartitionEntry[i].PartitionLength, SectorSize),
                  layout->PartitionEntry[i].RecognizedPartition ? " recognized" : "");
    return STATUS_SUCCESS;
}

/* Walk the on-disk tables the way IoReadPartitionTable numbers them, and hand back where the
 * Nth recognised partition's entry actually lives: which sector holds its table, and which of
 * the four slots it is.  IoSetPartitionInformation is the only caller. */
static NTSTATUS HalpFindPartition(PDEVICE_OBJECT DeviceObject, ULONG SectorSize, ULONG Number,
                                  PUCHAR Sector, PULONG TableLba, PULONG Slot)
{
    ULONG base = 0, extendedStart = 0, number = 0, passes = 0;
    BOOLEAN first = TRUE;

    for (;;) {
        BOOLEAN sawNext = FALSE;
        ULONG next = base;
        NTSTATUS status = HalpDiskIo(DeviceObject, SectorSize, base, Sector, FALSE);

        if (!NT_SUCCESS(status)) return status;
        if (Sector[MBR_MAGIC] != 0x55 || Sector[MBR_MAGIC + 1] != 0xaa) return STATUS_UNSUCCESSFUL;
        for (ULONG i = 0; i < 4; i++) {
            PUCHAR e = Sector + MBR_TABLE + i * 16;
            UCHAR type = e[PTE_TYPE];
            ULONG rel = HalpGetUlong(e + PTE_START);

            if (HalpExtended(type)) {
                if (first) extendedStart = rel;
                next = first ? rel : extendedStart + rel;
                sawNext = TRUE;
                continue;
            }
            if (type == PARTITION_ENTRY_UNUSED || !HalpRecognized(type)) continue;
            if (++number == Number) { *TableLba = base; *Slot = i; return STATUS_SUCCESS; }
        }
        first = FALSE;
        if (!sawNext || next == base || ++passes > PARTITION_MAX) break;
        base = next;
    }
    return STATUS_NOT_FOUND;
}

/* ---- IoSetPartitionInformation --------------------------------------------------------------
 * Change one partition's system-id byte in place.  Setup uses this after it formats a partition,
 * to stamp the file system it just laid down.
 */
NTSTATUS IoSetPartitionInformation(PDEVICE_OBJECT DeviceObject, ULONG SectorSize,
                                   ULONG PartitionNumber, ULONG PartitionType)
{
    PUCHAR sector;
    NTSTATUS status;
    ULONG lba = 0, slot = 0;

    if (SectorSize < 512 || SectorSize > SECTOR_MAX) return STATUS_INVALID_PARAMETER;
    sector = ExAllocatePool(NonPagedPoolCacheAligned, SectorSize);
    if (sector == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    status = HalpFindPartition(DeviceObject, SectorSize, PartitionNumber, sector, &lba, &slot);
    if (NT_SUCCESS(status)) {
        sector[MBR_TABLE + slot * 16 + PTE_TYPE] = (UCHAR)PartitionType;
        status = HalpDiskIo(DeviceObject, SectorSize, lba, sector, TRUE);
    }
    HalpPrint("HAL: IoSetPartitionInformation: #%d type -> %x (table lba %d slot %d, %x)\n",
              PartitionNumber, PartitionType, lba, slot, status);
    ExFreePool(sector);
    return status;
}

/* ---- IoWritePartitionTable -------------------------------------------------------------------
 * Lay the caller's layout back down.  disk.sys hands over a layout whose PartitionCount is a
 * multiple of four: each group of four entries is one on-disk table — group 0 is the MBR, and
 * every group after it is the extended boot record of one logical drive.  Every StartingOffset
 * in the layout is absolute; HiddenSectors is *not* filled in on this path (Setup's layouts
 * arrive with it zero), so the relative-sector fields are computed here, and they follow the two
 * different rules an extended chain uses: a partition entry counts from the table it sits in,
 * while the links that stitch the chain together count from the outermost extended partition.
 * Getting that wrong puts each extended boot record at the start of its own data instead of one
 * sector-group earlier, and the chain reads back as unterminated garbage.
 */
NTSTATUS IoWritePartitionTable(PDEVICE_OBJECT DeviceObject, ULONG SectorSize, ULONG SectorsPerTrack,
                               ULONG NumberOfHeads, PDRIVE_LAYOUT_INFORMATION Layout)
{
    PUCHAR sector;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG groups;

    if (SectorSize < 512 || SectorSize > SECTOR_MAX) return STATUS_INVALID_PARAMETER;
    if (SectorsPerTrack == 0) SectorsPerTrack = 63;
    if (NumberOfHeads == 0) NumberOfHeads = 255;
    groups = (Layout->PartitionCount + 3) / 4;

    HalpPrint("HAL: IoWritePartitionTable: %d entries (%d tables), signature %x, %d/%d geometry\n",
              Layout->PartitionCount, groups, Layout->Signature, SectorsPerTrack, NumberOfHeads);
    for (ULONG i = 0; i < Layout->PartitionCount; i++) {
        PPARTITION_INFORMATION p = &Layout->PartitionEntry[i];
        HalpPrint("HAL:   [%d] type %x start %d len %d hidden %d num %d%s%s\n", i, p->PartitionType,
                  HalpDivBySector(p->StartingOffset, SectorSize),
                  HalpDivBySector(p->PartitionLength, SectorSize), p->HiddenSectors,
                  p->PartitionNumber, p->BootIndicator ? " boot" : "",
                  p->RewritePartition ? " rewrite" : "");
    }

    sector = ExAllocatePool(NonPagedPoolCacheAligned, SectorSize);
    if (sector == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    ULONG lba = 0;                  /* LBA of the table being written; group 0 is the MBR */
    ULONG extendedStart = 0;        /* the outermost extended partition, the chain's origin */

    for (ULONG g = 0; g < groups; g++) {
        ULONG nextLba = 0;
        NTSTATUS one;

        /* Keep whatever is already in the sector outside the table: sector 0's boot code, and an
         * extended boot record's (unused) code area. */
        one = HalpDiskIo(DeviceObject, SectorSize, lba, sector, FALSE);
        if (!NT_SUCCESS(one)) memset(sector, 0, SectorSize);
        if (g == 0) HalpPutUlong(sector + MBR_SIGNATURE, Layout->Signature);
        memset(sector + MBR_TABLE, 0, 64);

        for (ULONG i = 0; i < 4 && g * 4 + i < Layout->PartitionCount; i++) {
            PPARTITION_INFORMATION p = &Layout->PartitionEntry[g * 4 + i];
            PUCHAR e = sector + MBR_TABLE + i * 16;
            ULONG len = HalpDivBySector(p->PartitionLength, SectorSize);
            ULONG rel;
            ULONG absolute = HalpDivBySector(p->StartingOffset, SectorSize);
            ULONG endLba, c, h, sc;

            if (p->PartitionType == PARTITION_ENTRY_UNUSED || len == 0) continue;
            if (HalpExtended(p->PartitionType)) {
                /* group 0's extended entry defines the origin and locates the first record;
                 * a link inside the chain is stored relative to that origin */
                if (g == 0) extendedStart = absolute;
                rel = (g == 0) ? absolute : absolute - extendedStart;
                nextLba = absolute;
            } else {
                rel = absolute - lba;
            }
            e[PTE_BOOT] = p->BootIndicator ? 0x80 : 0x00;
            e[PTE_TYPE] = p->PartitionType;
            HalpPutUlong(e + PTE_START, rel);
            HalpPutUlong(e + PTE_LENGTH, len);

            /* CHS is dead weight on an ARC machine, but a table full of zeroes confuses every
             * other operating system that ever looks at this disk.  Clamp at the 1024-cylinder
             * wall the way every BIOS-era tool does. */
            c = absolute / (SectorsPerTrack * NumberOfHeads);
            h = (absolute / SectorsPerTrack) % NumberOfHeads;
            sc = (absolute % SectorsPerTrack) + 1;
            if (c > 1023) { c = 1023; h = NumberOfHeads - 1; sc = SectorsPerTrack; }
            e[1] = (UCHAR)h; e[2] = (UCHAR)(sc | ((c >> 2) & 0xc0)); e[3] = (UCHAR)c;
            endLba = absolute + len - 1;
            c = endLba / (SectorsPerTrack * NumberOfHeads);
            h = (endLba / SectorsPerTrack) % NumberOfHeads;
            sc = (endLba % SectorsPerTrack) + 1;
            if (c > 1023) { c = 1023; h = NumberOfHeads - 1; sc = SectorsPerTrack; }
            e[5] = (UCHAR)h; e[6] = (UCHAR)(sc | ((c >> 2) & 0xc0)); e[7] = (UCHAR)c;
        }
        sector[MBR_MAGIC] = 0x55; sector[MBR_MAGIC + 1] = 0xaa;

        one = HalpDiskIo(DeviceObject, SectorSize, lba, sector, TRUE);
        HalpPrint("HAL:   table %d -> lba %d (%x)\n", g, lba, one);
        if (!NT_SUCCESS(one)) status = one;
        if (nextLba == 0 && g + 1 < groups) {
            HalpPrint("HAL: IoWritePartitionTable: table %d has no link to table %d\n", g, g + 1);
            break;
        }
        lba = nextLba;
    }
    ExFreePool(sector);
    return status;
}
