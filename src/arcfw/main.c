/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * main.c -- the ARC firmware shim for the Apple Network Server: what Microsoft's VENEER.EXE did
 * for Windows NT's loaders, written for this machine's Open Firmware.
 *
 * The sequence is the veneer's (its main, read in docs/2026-11-05-the-arc-firmware.md): the
 * system parameter block at 0x4000; KSEG0 mapped onto physical memory (mmu.c, before main);
 * the configuration tree from the device tree (the snapshot stage0 took); the firmware vector; the boot file
 * from the OSLOADER environment variable, loaded at its link address; the memory descriptors,
 * last, so that the loader's own pages are in them; argv and envp from the environment; and the
 * jump through the loader's entry descriptor.  Original. */
#include "arcfw.h"
#include "of.h"

#define ARCFW_VERSION "0.1"

static CHAR argbuf[12][200];
static PCHAR argv[14], envp[14];

static VOID add_arg(ULONG *argc, const CHAR *name, const CHAR *value)
{
    if (*argc >= 12) return;
    if (name) con_snprintf(argbuf[*argc], sizeof argbuf[0], "%s=%s", name, value);
    else con_snprintf(argbuf[*argc], sizeof argbuf[0], "%s", value);
    argv[*argc] = argbuf[*argc];
    (*argc)++;
}

VOID arcfw_main(ULONG snapshot)
{
    of_init(snapshot);
    con_printf("\nArcfw %s: ARC firmware for the Bandit machines (powermac-nt-hal)\n", ARCFW_VERSION);
    con_printf("arcfw: little-endian since stage0; Open Firmware left in its factory mode, nothing in NVRAM changed\n");
    const CHAR *dbg = env_lookup("ARCFW-DEBUG");
    if (dbg) { const CHAR *e; arcfw_debug = 0; for (e = dbg; *e; e++) arcfw_debug = arcfw_debug * 16 + ((*e >= '0' && *e <= '9') ? *e - '0' : (toupper(*e) - 'A' + 10) & 15); }

    /* 1. the system parameter block, the vector, the restart block, at 0x4000 */
    arc_init_spb();
    con_printf("arcfw: system parameter block at %x, %u vector entries\n", ARC_SPB_PHYS, FIRMWARE_VECTOR_ENTRIES);

    /* 2. KSEG0: 8 MB of physical memory at 0x80000000, where NT's loaders live and look -- in
     * the page table since mmu_init, as Open Firmware's MMU package mapped it for the veneer */

    /* 3. the configuration tree */
    config_build();
    con_printf("arcfw: configuration tree built\n");
    if (arcfw_debug & DBG_CONFIG) config_dump(config_root(), 0);

    /* 4. the boot file and its arguments, from the environment on /options */
    io_init();
    CHAR osloader[200], syspart[128], loadfile[128], loadpart[128], options[128], ident[128];
    if (env_get("OSLOADER", sizeof osloader, osloader) != ESUCCESS) fatal("no OSLOADER in the environment (set it on /options)");
    env_get("SYSTEMPARTITION", sizeof syspart, syspart);
    env_get("OSLOADFILENAME", sizeof loadfile, loadfile);
    env_get("OSLOADPARTITION", sizeof loadpart, loadpart);
    if (env_get("OSLOADOPTIONS", sizeof options, options) != ESUCCESS) strcpy(options, "NODEBUG");
    if (env_get("LOADIDENTIFIER", sizeof ident, ident) != ESUCCESS) strcpy(ident, "Windows NT");
    con_printf("arcfw: booting '%s'\n", osloader);

    PVOID entry, lo, hi;
    ARC_STATUS s = load_image(osloader, &entry, &lo, &hi);
    if (s != ESUCCESS) fatal("cannot load '%s' (ARC status %u)", osloader, s);
    con_printf("arcfw: loaded at %p..%p, entry descriptor %p\n", lo, hi, entry);

    /* 5. memory descriptors, now that the loader's pages are claimed */
    memory_init();

    /* 6. argv as the veneer built it: the loader path, the boot variables, the console */
    ULONG argc = 0;
    add_arg(&argc, NULL, osloader);
    add_arg(&argc, "OsLoader", osloader);
    add_arg(&argc, "SystemPartition", syspart);
    add_arg(&argc, "OSLoadFilename", loadfile);
    add_arg(&argc, "OSLoadPartition", loadpart);
    add_arg(&argc, "OSLoadOptions", options);
    add_arg(&argc, "LoadIdentifier", ident);
    add_arg(&argc, "ConsoleIn", "multi(0)serial(0)line(0)");
    add_arg(&argc, "ConsoleOut", "multi(0)serial(0)line(0)");
    argv[argc] = NULL;
    ULONG ne = 0;
    for (ULONG i = 1; i < argc; i++) envp[ne++] = argv[i];
    envp[ne] = NULL;

    con_printf("arcfw: starting the loader\n");
    load_run(entry, argc, argv, envp);
}
