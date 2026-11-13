/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * dtsnap.h -- the device-tree snapshot stage0 hands the ARC firmware.
 *
 * stage0 (src/stage0, big-endian, with Open Firmware alive) records the whole tree; the ARC
 * firmware (of.c, little-endian, with Open Firmware gone) answers finddevice, getprop, child,
 * peer and the rest from it.  Shared by both, so only constants and offsets live here.
 *
 * At DTSNAP_PHYS, as the byte stream little-endian code reads (stage0 reverses each doubleword
 * to get it there), all integers little-endian:
 *
 *   0x00  magic 'DTS1'           0x04  bytes used
 *   0x08  node count             0x0C  RAM size
 *   0x10  OEM disk blocks read (0: none)
 *   DTSNAP_NODES: DTSNAP_MAX_NODES node records of DTSNAP_NODE_SIZE bytes, in depth-first order
 *   (index 0 is the root), then strings, property tables and values, each 8-byte aligned.
 *
 * A node record: parent, first child and next peer as node indices (DTSNAP_NONE for none), the
 * offset of its full path (package-to-path, NUL-terminated), its property count and the offset
 * of its property table, and Open Firmware's own phandle (for diagnostics only).  A property
 * entry: the offset of its NUL-terminated name, its length, the offset of its value -- the
 * property's bytes exactly as getprop returned them (integers in a property are big-endian, as
 * IEEE 1275 encodes them, whatever mode the reader runs in).  Original. */
#pragma once

#define DTSNAP_PHYS        0x00F00000u   /* above KSEG0's 8 MB: FirmwareTemporary to NT */
#define DTSNAP_MAX         0x00080000u   /* 512 KB */
#define DTSNAP_MAGIC       0x31535444u   /* 'D','T','S','1' in memory order */
#define DTSNAP_NONE        0xFFFFFFFFu

#define DTSNAP_MAGIC_OFF   0x00
#define DTSNAP_BYTES_OFF   0x04
#define DTSNAP_COUNT_OFF   0x08
#define DTSNAP_RAM_OFF     0x0C
#define DTSNAP_OEMDISK_OFF 0x10
#define DTSNAP_NODES       0x40

#define DTSNAP_MAX_NODES   512
#define DTSNAP_NODE_SIZE   32
#define DTN_PARENT         0x00
#define DTN_CHILD          0x04
#define DTN_PEER           0x08
#define DTN_PATH           0x0C
#define DTN_NPROPS         0x10
#define DTN_PROPS          0x14
#define DTN_OFPH           0x18

#define DTSNAP_PROP_SIZE   12
#define DTP_NAME           0x00
#define DTP_LEN            0x04
#define DTP_VALUE          0x08
