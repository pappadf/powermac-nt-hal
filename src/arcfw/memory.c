/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * memory.c -- the ARC memory descriptors, from the snapshot's /memory node.
 *
 * `reg` is the installed memory.  Open Firmware is gone (stage0 left it), so nothing of its own
 * needs keeping: the list the loader walks (GetMemoryDescriptor) covers all of `reg`, with the
 * loaded program's pages MemoryLoadedProgram, the first ten pages (this firmware's exception
 * vectors, the system parameter block at 0x4000) MemoryFirmwarePermanent, this image with its
 * heap, stack and buffers MemoryFirmwareTemporary, the rest of the first 8 MB MemoryFree, and
 * all memory above those 8 MB MemoryFirmwareTemporary -- where the page table, the device-tree
 * snapshot and the OEM disk are -- which the kernel reclaims once it has taken over.  That is
 * the veneer's policy, measured in the trace of 5 November (docs/2026-11-05-the-arc-firmware.md):
 * descriptors 0-0xA permanent, the veneer temporary, the loader, 0x800 and up temporary.  The
 * HAL and the kernel cope with that layout, so it is kept.  Original. */
#include "arcfw.h"
#include "of.h"

#define MAX_DESC 64
static MEMORY_DESCRIPTOR descs[MAX_DESC];
static ULONG ndesc;
static ULONG loaded_base, loaded_pages;      /* the program load_image placed, in pages */

static VOID add(MEMORY_TYPE type, ULONG base, ULONG count)
{
    if (!count) return;
    if (ndesc && descs[ndesc - 1].MemoryType == (ULONG)type && descs[ndesc - 1].BasePage + descs[ndesc - 1].PageCount == base) {
        descs[ndesc - 1].PageCount += count;        /* merge with the previous one */
        return;
    }
    if (ndesc == MAX_DESC) fatal("too many memory descriptors");
    descs[ndesc].MemoryType = type; descs[ndesc].BasePage = base; descs[ndesc].PageCount = count;
    ndesc++;
}

/* type a page range by what it is, splitting on the special ranges */
static VOID type_range(ULONG base, ULONG count, MEMORY_TYPE deflt)
{
    while (count) {
        ULONG end = base + count, cut = end;
        MEMORY_TYPE t = deflt;
        if (base < 0xA) { t = MemoryFirmwarePermanent; cut = 0xA < end ? 0xA : end; }
        else if (base >= ARC_KSEG0_PAGES && deflt == MemoryFree) {
            /* The loader takes its heap and stack from a free descriptor through KSEG0 addresses,
             * and KSEG0 is mapped for 8 MB: a free page above it is a DSI in RtlZeroMemory
             * (OSLOADER at 0x80611B58, 5 November).  The veneer called the rest firmware-
             * temporary too; the kernel reclaims that type once it has its own page tables. */
            t = MemoryFirmwareTemporary;
        }
        else if (deflt == MemoryFree && end > ARC_KSEG0_PAGES && base < ARC_KSEG0_PAGES) {
            cut = ARC_KSEG0_PAGES;
            if (loaded_base > base && loaded_base < cut) cut = loaded_base;
            if (base >= loaded_base && base < loaded_base + loaded_pages) { t = MemoryLoadedProgram; cut = loaded_base + loaded_pages < cut ? loaded_base + loaded_pages : cut; }
        }
        else if (base >= loaded_base && base < loaded_base + loaded_pages) {
            t = MemoryLoadedProgram; cut = loaded_base + loaded_pages < end ? loaded_base + loaded_pages : end;
        } else {
            if (loaded_base > base && loaded_base < cut) cut = loaded_base;
        }
        add(t, base, cut - base);
        count -= cut - base; base = cut;
    }
}

VOID memory_note_claim(ULONG phys, ULONG size, MEMORY_TYPE type)
{
    if (type == MemoryLoadedProgram) { loaded_base = phys >> 12; loaded_pages = (size + 0xFFF) >> 12; }
}

extern UCHAR _start[], __bss_end[];

VOID memory_init(VOID)
{
    OF_PHANDLE mem = of_finddevice("/memory");
    if (mem == OF_NONE) fatal("no /memory node");
    ULONG reg[32];
    LONG nreg = of_get_ints(mem, "reg", reg, sizeof reg);
    if (nreg < 8) fatal("/memory has no reg property");
    ULONG fw_lo = (ULONG)_start >> 12, fw_hi = ((ULONG)__bss_end + 0xFFF) >> 12;
    ndesc = 0;
    for (LONG i = 0; i + 1 < nreg / 4; i += 2) {
        ULONG rbase = reg[i] >> 12, rend = (reg[i] + reg[i + 1]) >> 12;
        ULONG lo = fw_lo > rbase ? fw_lo : rbase, hi = fw_hi < rend ? fw_hi : rend;
        if (lo >= hi) { type_range(rbase, rend - rbase, MemoryFree); continue; }
        type_range(rbase, lo - rbase, MemoryFree);
        type_range(lo, hi - lo, MemoryFirmwareTemporary);
        type_range(hi, rend - hi, MemoryFree);
    }
    for (ULONG k = 0; k < ndesc; k++)
        DBG(DBG_MEM, "arcfw:   descriptor %u: type %u pages %05x..%05x\n", k, descs[k].MemoryType, descs[k].BasePage, descs[k].BasePage + descs[k].PageCount);
}

PMEMORY_DESCRIPTOR memory_next(PMEMORY_DESCRIPTOR prev)
{
    if (!ndesc) return NULL;
    if (prev == NULL) return &descs[0];
    ULONG i = (ULONG)(prev - descs);
    if (i + 1 >= ndesc) return NULL;
    return &descs[i + 1];
}
