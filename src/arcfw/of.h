/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * of.h -- what this firmware still calls "Open Firmware": the client-interface services it used
 * to ask the ROM for, now answered without it (of.c).  The ROM runs big-endian and is left
 * before this firmware starts (src/stage0); what it knew comes in a snapshot of its device tree
 * (dtsnap.h), and its devices are driven here.  A phandle names a device-tree node, an ihandle
 * an opened device; both are 32-bit cells, as in IEEE 1275-1994.  Original. */
#pragma once
#include "arcfw.h"

typedef ULONG OF_PHANDLE;
typedef ULONG OF_IHANDLE;
#define OF_NONE ((ULONG)-1)

/* the device tree, from the snapshot */
OF_PHANDLE of_finddevice(const CHAR *path);
OF_PHANDLE of_peer(OF_PHANDLE node);
OF_PHANDLE of_child(OF_PHANDLE node);
OF_PHANDLE of_parent(OF_PHANDLE node);
LONG of_getproplen(OF_PHANDLE node, const CHAR *name);
LONG of_getprop(OF_PHANDLE node, const CHAR *name, PVOID buf, ULONG len);
LONG of_nextprop(OF_PHANDLE node, const CHAR *prev, CHAR *name32);
LONG of_setprop(OF_PHANDLE node, const CHAR *name, const VOID *buf, ULONG len);
LONG of_package_to_path(OF_PHANDLE node, CHAR *buf, ULONG len);

/* devices: the ESCC's channels, the floppy (its image in RAM), the 53C825A buses and their
 * disks and CD-ROMs, opened by device-tree path */
OF_PHANDLE of_instance_to_package(OF_IHANDLE ih);
LONG of_instance_to_path(OF_IHANDLE ih, CHAR *buf, ULONG len);
OF_IHANDLE of_open(const CHAR *path);
VOID of_close(OF_IHANDLE ih);
LONG of_read(OF_IHANDLE ih, PVOID buf, ULONG len);
LONG of_write(OF_IHANDLE ih, const VOID *buf, ULONG len);
LONG of_seek(OF_IHANDLE ih, ULONG pos_hi, ULONG pos_lo);
/* call-method with up to 6 cells in and 3 out, top of stack first; returns 0 if it ran.  Only
 * a SCSI bus's set-address and short-data-command (the configuration probe) exist. */
LONG of_call_method(const CHAR *method, OF_IHANDLE ih, ULONG nin, ULONG nout, ULONG *in, ULONG *out);

/* memory: claims are bookkeeping for the memory descriptors (memory.c); the page table maps
 * KSEG0 and low memory one-to-one already (mmu.c), so a map is a no-op */
LONG of_map(ULONG phys, ULONG virt, ULONG size, LONG mode);
LONG of_claim_real(ULONG addr, ULONG size);                 /* claim phys = virt */

/* time, reset, power */
ULONG of_milliseconds(VOID);
ULONG of_timebase_hz(VOID);      /* the cpu node's timebase-frequency, in hertz */
LONG of_interpret(const CHAR *forth);                       /* "reset-all", "shut-down" */
VOID of_exit(VOID) __attribute__((noreturn));
VOID of_enter(VOID);

/* property helpers */
BOOLEAN of_get_int(OF_PHANDLE node, const CHAR *name, PULONG out);
LONG of_get_ints(OF_PHANDLE node, const CHAR *name, PULONG buf, ULONG len);   /* cells swapped to host order */
ULONG of_bswap(ULONG v);
LONG of_get_str(OF_PHANDLE node, const CHAR *name, CHAR *buf, ULONG len);   /* -1 if absent */
BOOLEAN of_has_prop(OF_PHANDLE node, const CHAR *name);

extern OF_PHANDLE of_chosen;
extern OF_IHANDLE of_stdout, of_stdin;
VOID of_init(ULONG snapshot);

/* mmu.c: the page table, the exception vectors, device windows */
extern ULONG arcfw_ram;
VOID mmu_init(VOID);                                        /* real mode, before anything else */
ULONG mmu_map_io(ULONG phys, ULONG size);                   /* a cache-inhibited window; its virtual address */

/* scsi.c: polled 53C825A */
typedef struct _SCSI_BUS SCSI_BUS;
SCSI_BUS *scsi_bus(OF_PHANDLE ctl);                         /* NULL if not a 53C825A this driver runs */
LONG scsi_command(SCSI_BUS *b, ULONG target, const UCHAR *cdb, ULONG cdblen, PVOID buf, ULONG len, BOOLEAN out);
LONG scsi_read_bytes(SCSI_BUS *b, ULONG target, ULONG block, ULONG pos_hi, ULONG pos_lo, PVOID buf, ULONG len);
LONG scsi_capacity(SCSI_BUS *b, ULONG target, PULONG blocks, PULONG block);

/* cuda.c: the system controller */
VOID cuda_reset_system(VOID) __attribute__((noreturn));
VOID cuda_power_down(VOID) __attribute__((noreturn));
