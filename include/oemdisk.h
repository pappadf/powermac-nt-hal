/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * Original work of this project.  Nothing here derives from leaked Windows NT source.
 *
 * oemdisk.h — the hand-off of the OEM disk image from the boot floppy to Windows NT.
 *
 * Shared by three parties, which is why it is one header:
 *
 *   * tools/mkbootfloppy.py writes \BOOT.OF, which has Open Firmware read the whole floppy into
 *     RAM at OEMDISK_PHYS + OEMDISK_HEADER_SIZE and lay an OEMDISK_HEADER down at OEMDISK_PHYS;
 *   * the HAL (src/oemdisk.c) validates that header in phase 0, marks the pages
 *     LoaderFirmwarePermanent so NT never hands them out, and answers HalAnsOemDiskQuery;
 *   * the ADB port driver (drivers/adbport) asks the HAL, maps the image, and serves it to Setup
 *     as \Device\Floppy0 -- because the copy of OEM files onto the hard disk happens under NT,
 *     through an NT floppy device, and NT has no driver for the SWIM3 behind the real drive.
 *
 * Why a fixed physical address.  The veneer is Microsoft's and publishes nothing we add to the
 * Open Firmware device tree, so the image has to be found by convention.  0x03B97000 is chosen
 * so that header + image (0x169000 bytes) end exactly at 0x3D00000, where the veneer's staging
 * area begins (boot.of reads the veneer there, then moves it to load-base 0x3E00000).  Claimed
 * adjacent to a range the firmware has already claimed, this adds no entry to /memory's
 * `available` list.  The first draft used 0x3A00000, in the middle of free memory: with that one
 * extra entry in the list SETUPLDR's fourth open of the CD came back through the veneer as
 * OFOpen('') -- an empty firmware path -- and Setup asked for the CD in "Drive A:".  The
 * firmware itself was fine (the CD still opened and read from the 0 > prompt, and claims still
 * succeeded); what inside the veneer turns the extra entry into an empty path is not known.
 * Placed here, the same run reaches Setup's menus (the boot-floppy note, E22).  The veneer marks
 * everything above 8 MB FirmwareTemporary, so SETUPLDR's own allocations never come near this.
 * It assumes at least 64 MB of RAM, which the load-base/real-base pair in setup.of already
 * assumes.  The HAL validates the header and the FAT boot sector behind it before trusting
 * either, so a machine that does not fit the assumption reports a missing disk rather than
 * serving garbage.
 *
 * Everything is little-endian: the machine runs little-endian from setup.of's reset onwards, so
 * Open Firmware's `!` stores exactly what NT reads. */
#pragma once

#define OEMDISK_PHYS          0x03B97000u   /* the header page; the image follows */
#define OEMDISK_HEADER_SIZE   0x1000u
#define OEMDISK_IMAGE_PHYS    (OEMDISK_PHYS + OEMDISK_HEADER_SIZE)
#define OEMDISK_MAGIC         0x4F534E41u   /* 'A','N','S','O' in memory order */
#define OEMDISK_VERSION       1u
#define OEMDISK_BLOCK         512u
#define OEMDISK_MAX_BYTES     0x00200000u   /* 2 MB: room for a 2.88 MB disk is not offered */

typedef struct _OEMDISK_HEADER {
    unsigned int Magic;          /* OEMDISK_MAGIC */
    unsigned int Version;        /* OEMDISK_VERSION */
    unsigned int ImagePhys;      /* physical address of the first byte of the image */
    unsigned int ImageBytes;     /* 1474560 for a 1.44 MB disk */
    unsigned int BlockBytes;     /* 512 */
    unsigned int BlocksRead;     /* what read-blocks actually returned, summed by boot.of */
    unsigned int Reserved[2];
} OEMDISK_HEADER;
