/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * Original work of this project.  Nothing here derives from leaked Windows NT source.
 *
 * oemdisk.c — the OEM disk image the boot floppy leaves in RAM, found, checked and protected.
 *
 * Text-mode Setup reads the OEM disk twice.  The first time SETUPLDR reads it, through the
 * firmware, and everything on it works: the HAL, the display driver, the ADB driver.  The second
 * time is under NT, when Setup copies those same files onto the hard disk -- and that read goes
 * through \Device\Floppy0, which NT can only provide if a driver does.  There is no NT driver
 * for the SWIM3 behind this machine's drive.  So \BOOT.OF reads the whole floppy into RAM before
 * `go`, and the ADB port driver serves that copy as the floppy.  This file is the HAL's part:
 *
 *   1. In phase 0, before the memory manager exists, find the header at OEMDISK_PHYS, check it,
 *      and check that the image behind it begins with a FAT boot sector.  The check is what turns
 *      "the address convention did not hold on this machine" into one trace line instead of a
 *      floppy full of garbage.
 *   2. Retype the pages LoaderFirmwarePermanent in the loader's memory descriptor list, exactly
 *      as HalpReserveVgaAperture carves the VGA aperture out of free memory (init.c).  The veneer
 *      reports everything above 8 MB as *FirmwareTemporary* (its -vrdebug 0x10 dump shows it),
 *      and the kernel frees FirmwareTemporary memory once it is up -- so without this the OEM
 *      disk would be the first pool the system hands out.
 *   3. Answer HalAnsOemDiskQuery for the driver, which maps the image itself.
 *
 * Saying "this physical range is not yours to allocate" is a HAL's job; that is the whole reason
 * this is not done in the driver. */
#include "hal.h"
#include "oemdisk.h"

#define LoaderFree               2
#define LoaderFirmwareTemporary  5   /* what the veneer calls everything above 8 MB; freed after boot */
#define LoaderFirmwarePermanent  6
#define PAGE_SHIFT               12

static BOOLEAN HalpOemDiskPresent;
static ULONG HalpOemDiskPhys, HalpOemDiskBytes;
static MEMORY_ALLOCATION_DESCRIPTOR HalpOemDiskHole, HalpOemDiskTail;

static VOID HalpOemDiskInsertAfter(PLIST_ENTRY at, PLIST_ENTRY item)
{
    item->Flink = at->Flink;
    item->Blink = at;
    at->Flink->Blink = item;
    at->Flink = item;
}

/* Carve [lo, hi) pages out of whatever descriptor covers them and mark them firmware-permanent.
 * Returns what the covering descriptor's type was, or -1 if no single descriptor covers it. */
static LONG HalpOemDiskReserve(PLOADER_PARAMETER_BLOCK LoaderBlock, ULONG lo, ULONG hi)
{
    for (PLIST_ENTRY e = LoaderBlock->MemoryDescriptorListHead.Flink;
         e != &LoaderBlock->MemoryDescriptorListHead; e = e->Flink) {
        PMEMORY_ALLOCATION_DESCRIPTOR m = (PMEMORY_ALLOCATION_DESCRIPTOR)e;
        ULONG mlo = m->BasePage, mhi = m->BasePage + m->PageCount;
        if (mlo > lo || mhi < hi) continue;
        LONG was = (LONG)m->MemoryType;
        if (was != LoaderFree && was != LoaderFirmwareTemporary) return was;   /* leave anything else */
        HalpOemDiskHole.MemoryType = LoaderFirmwarePermanent;
        HalpOemDiskHole.BasePage = lo;
        HalpOemDiskHole.PageCount = hi - lo;
        if (mhi > hi) {
            HalpOemDiskTail.MemoryType = was;
            HalpOemDiskTail.BasePage = hi;
            HalpOemDiskTail.PageCount = mhi - hi;
        }
        if (mlo < lo) {
            m->PageCount = lo - mlo;
            HalpOemDiskInsertAfter(e, &HalpOemDiskHole.ListEntry);
        } else {
            m->MemoryType = LoaderFirmwarePermanent;
            m->PageCount = hi - lo;
        }
        if (mhi > hi)
            HalpOemDiskInsertAfter((mlo < lo) ? &HalpOemDiskHole.ListEntry : e, &HalpOemDiskTail.ListEntry);
        return was;
    }
    return -1;
}

/* Phase 0 has no MmMapIoSpace, and the kernel's KePhase0MapIo hands out virtual addresses inside
 * the window DBAT3 already maps for our I/O (MMIO_BASE_VIRT, init.c) -- two BATs on one address
 * is undefined on the 604, and the first test hung right there, silently, one line after the
 * VGA-aperture message.  So the HAL borrows DBAT2 for the few loads this takes: one 8 MB block
 * over header and image (oemdisk.h keeps them in one), cache-inhibited because the firmware's DMA
 * wrote the bytes, and cleared again before the function returns.  DBAT1/2 are the slots
 * KePhase0MapIo would use; nothing in this HAL calls it any more. */
#define OEMDISK_BAT_VIRT 0xA0000000u
#define OEMDISK_BAT_BASE (OEMDISK_PHYS & ~0x7FFFFFu)
_Static_assert(OEMDISK_IMAGE_PHYS + OEMDISK_MAX_BYTES <= OEMDISK_BAT_BASE + 0x800000, "the OEM disk must fit one 8 MB BAT block");

static ULONG HalpReadDbatu(ULONG n)
{
    ULONG v;
    if (n == 1) __asm__ volatile("mfdbatu %0, 1" : "=r"(v)); else __asm__ volatile("mfdbatu %0, 2" : "=r"(v));
    return v;
}

static volatile UCHAR *HalpOemDiskBatMap(VOID)
{
    ULONG upper = OEMDISK_BAT_VIRT | 0xFC | 0x2;          /* BL = 8 MB, Vs */
    ULONG lower = OEMDISK_BAT_BASE | 0x20 | 0x08 | 0x2;   /* I, G, PP = read/write */
    __asm__ volatile("mtdbatu 2, %0" : : "r"(0)); __asm__ volatile("isync");
    __asm__ volatile("mtdbatl 2, %0" : : "r"(lower));
    __asm__ volatile("mtdbatu 2, %0" : : "r"(upper)); __asm__ volatile("isync");
    return (volatile UCHAR *)OEMDISK_BAT_VIRT;
}

static VOID HalpOemDiskBatUnmap(VOID)
{
    __asm__ volatile("mtdbatu 2, %0" : : "r"(0)); __asm__ volatile("isync");
}

VOID HalpOemDiskInitialize(PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    HalpPrint("HAL: OEM disk: looking at %x (dbat1 %x dbat2 %x)\n", OEMDISK_PHYS, HalpReadDbatu(1), HalpReadDbatu(2));
    volatile UCHAR *w = HalpOemDiskBatMap();
    volatile OEMDISK_HEADER *h = (volatile OEMDISK_HEADER *)(w + (OEMDISK_PHYS - OEMDISK_BAT_BASE));
    ULONG magic = h->Magic, version = h->Version, phys = h->ImagePhys, bytes = h->ImageBytes;
    ULONG block = h->BlockBytes, blocks = h->BlocksRead;
    UCHAR b0 = 0, b510 = 0, b511 = 0;
    if (phys == OEMDISK_IMAGE_PHYS) {
        volatile UCHAR *bs = w + (phys - OEMDISK_BAT_BASE);
        b0 = bs[0]; b510 = bs[510]; b511 = bs[511];
    }
    HalpOemDiskBatUnmap();

    if (magic != OEMDISK_MAGIC) {
        HalpPrint("HAL: no OEM disk header at %x (saw %x) - Setup will have no drive A:\n", OEMDISK_PHYS, magic);
        return;
    }
    if (version != OEMDISK_VERSION || block != OEMDISK_BLOCK || phys != OEMDISK_IMAGE_PHYS ||
        bytes == 0 || bytes > OEMDISK_MAX_BYTES || (bytes & (OEMDISK_BLOCK - 1)) || blocks * block != bytes) {
        HalpPrint("HAL: OEM disk header rejected: version %d phys %x bytes %x block %d read %d\n",
                  version, phys, bytes, block, blocks);
        return;
    }

    /* The image must start with a FAT boot sector: a short jump and the 0x55AA signature.  This
     * is the same test SETUPLDR's IsFatFileStructure applies, and it is what catches a header
     * that survived from a previous boot in front of memory that did not. */
    if (!((b0 == 0xEB || b0 == 0xE9) && b510 == 0x55 && b511 == 0xAA)) {
        HalpPrint("HAL: OEM disk image at %x is not a FAT floppy (%x .. %x %x) - ignored\n", phys, b0, b510, b511);
        return;
    }

    ULONG lo = OEMDISK_PHYS >> PAGE_SHIFT;
    ULONG hi = (phys + bytes + (1u << PAGE_SHIFT) - 1) >> PAGE_SHIFT;
    LONG was = HalpOemDiskReserve(LoaderBlock, lo, hi);
    if (was == LoaderFree || was == LoaderFirmwareTemporary)
        HalpPrint("HAL: OEM disk: pages %x..%x were type %d - now firmware-permanent\n", lo, hi, was);
    else if (was == -1)
        HalpPrint("HAL: OEM disk: pages %x..%x not covered by one descriptor - unprotected, expect trouble\n", lo, hi);
    else
        HalpPrint("HAL: OEM disk: pages %x..%x already type %d - left alone\n", lo, hi, was);

    HalpOemDiskPresent = TRUE;
    HalpOemDiskPhys = phys;
    HalpOemDiskBytes = bytes;
    HalpPrint("HAL: OEM disk: %d bytes at %x, %d blocks of %d\n", bytes, phys, blocks, block);
}

/* Exported for drivers/adbport.  Returns FALSE, and touches nothing, when there is no disk. */
BOOLEAN HalAnsOemDiskQuery(PULONG PhysicalBase, PULONG Bytes)
{
    if (!HalpOemDiskPresent) return FALSE;
    if (PhysicalBase) *PhysicalBase = HalpOemDiskPhys;
    if (Bytes) *Bytes = HalpOemDiskBytes;
    return TRUE;
}
