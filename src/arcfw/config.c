/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * config.c -- the ARC configuration tree, built from Open Firmware's device tree.
 *
 * The shape is the one Microsoft's veneer produced for this machine, read out of its memory on
 * 5 November 2026 (docs/2026-11-05-the-arc-firmware.md): the root is ArcSystem "device-tree"
 * with a device-specific resource of firmware strings; under it the processor with its two
 * primary caches, the secondary cache, "memory", and one MultiFunctionAdapter "PCI" per Bandit
 * whose children are the SCSI adapters ("NCRC810", the identifier symc810.sys matches) with the
 * targets the firmware found on each bus, and -- flattened out of Grand Central -- the network,
 * serial, floppy, keyboard, pointer and display controllers, each with its peripheral.  Where the
 * veneer got this machine wrong it is fixed here rather than reproduced: the SCSI adapters carry
 * their real registers and interrupt (the HAL's HalpFixConfigTree did that repair), the floppy
 * is a DiskController with a FloppyDiskPeripheral (ledger row 17), and the third SCSI adapter
 * has its own key.  Identifiers that NT's drivers match on (NCRC810, PCAT_ENHANCED, PS2 MOUSE)
 * are kept exactly.  Original. */
#include "arcfw.h"
#include "of.h"

static PARC_NODE root;
static ULONG scsi_adapters;

static PARC_NODE new_node(PARC_NODE parent, CONFIGURATION_CLASS cls, CONFIGURATION_TYPE type, ULONG key,
                          ULONG flags, const CHAR *ident)
{
    PARC_NODE n = zalloc(sizeof *n);
    n->Component.Class = cls; n->Component.Type = type; n->Component.Flags = flags;
    n->Component.Version = 2; n->Component.Revision = 0;
    n->Component.Key = key; n->Component.AffinityMask = 0xFFFFFFFFu;
    if (ident) { n->Component.Identifier = strdup_(ident); n->Component.IdentifierLength = strlen(ident) + 1; }
    n->Parent = parent;
    if (parent) {
        PARC_NODE *p = &parent->Child;
        while (*p) p = &(*p)->Peer;
        *p = n;
    }
    return n;
}

/* a CM_PARTIAL_RESOURCE_LIST with n descriptors and `extra` device-specific bytes after them */
static PCM_PARTIAL_RESOURCE_LIST new_list(PARC_NODE n, ULONG count, ULONG extra)
{
    ULONG size = 8 + count * sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR) + extra;
    PCM_PARTIAL_RESOURCE_LIST l = zalloc(size);
    l->Version = 1; l->Revision = 2; l->Count = count;
    n->ConfigurationData = l;
    n->Component.ConfigurationDataLength = size;
    return l;
}
static VOID desc_port(PCM_PARTIAL_RESOURCE_DESCRIPTOR d, ULONG start, ULONG len)
{ d->Type = CmResourceTypePort; d->ShareDisposition = CmResourceShareDeviceExclusive; d->Flags = CM_RESOURCE_PORT_IO; d->u.Port.StartLo = start; d->u.Port.Length = len; }
static VOID desc_memory(PCM_PARTIAL_RESOURCE_DESCRIPTOR d, ULONG start, ULONG len)
{ d->Type = CmResourceTypeMemory; d->ShareDisposition = CmResourceShareDeviceExclusive; d->Flags = 0; d->u.Memory.StartLo = start; d->u.Memory.Length = len; }
static VOID desc_interrupt(PCM_PARTIAL_RESOURCE_DESCRIPTOR d, ULONG level, ULONG vector)
{ d->Type = CmResourceTypeInterrupt; d->ShareDisposition = CmResourceShareDeviceExclusive; d->Flags = CM_RESOURCE_INTERRUPT_LEVEL_SENSITIVE; d->u.Interrupt.Level = level; d->u.Interrupt.Vector = vector; d->u.Interrupt.Affinity = 1; }
/* the veneer's placeholder: a null descriptor with a length in it, which nothing reads */
static VOID desc_null(PCM_PARTIAL_RESOURCE_DESCRIPTOR d, ULONG len)
{ d->Type = CmResourceTypeNull; d->ShareDisposition = CmResourceShareDeviceExclusive; d->Flags = 0; d->u.Port.Length = len; }

static ULONG ilog2(ULONG v) { ULONG n = 0; while (v > 1) { v >>= 1; n++; } return n; }

/* ---- the processor and its caches --------------------------------------------------------- */
static VOID add_cpu(OF_PHANDLE cpu)
{
    ULONG pvr = 0, isz = 0x4000, dsz = 0x4000, iblk = 32, dblk = 32;
    of_get_int(cpu, "cpu-version", &pvr);
    of_get_int(cpu, "i-cache-size", &isz); of_get_int(cpu, "d-cache-size", &dsz);
    of_get_int(cpu, "i-cache-block-size", &iblk); of_get_int(cpu, "d-cache-block-size", &dblk);
    CHAR id[32];
    con_snprintf(id, sizeof id, "PowerPC(%u)", pvr >> 16 == 4 ? 604u : pvr >> 16 == 3 ? 603u : pvr >> 16 == 9 ? 604u : pvr >> 16);
    PARC_NODE p = new_node(root, ProcessorClass, CentralProcessor, 0, 0, id);
    /* the cache key: 0x01 | log2(line) in bits 16-23 | log2(pages) in bits 0-15, the ARC rule */
    new_node(p, CacheClass, PrimaryIcache, 0x01000000u | (ilog2(iblk) << 16) | ilog2(isz >> 12), 0, NULL);
    new_node(p, CacheClass, PrimaryDcache, 0x01000000u | (ilog2(dblk) << 16) | ilog2(dsz >> 12), 0, NULL);
    /* the secondary cache sits at the root, as the veneer placed it */
    for (OF_PHANDLE c = of_child(cpu); c; c = of_peer(c)) {
        CHAR dt[32];
        of_get_str(c, "device_type", dt, sizeof dt);
        if (strcmp(dt, "cache") == 0) {
            ULONG sz = 0x80000, blk = 8;
            of_get_int(c, "i-cache-size", &sz); of_get_int(c, "i-cache-block-size", &blk);
            new_node(root, CacheClass, SecondaryCache, 0x01000000u | (ilog2(blk) << 16) | ilog2(sz >> 12), 0, NULL);
        }
    }
}

/* ---- memory ------------------------------------------------------------------------------- */
static VOID add_memory(OF_PHANDLE mem)
{
    ULONG reg[8] = { 0, 0x04000000 };
    of_get_ints(mem, "reg", reg, sizeof reg);
    PARC_NODE m = new_node(root, MemoryClass, SystemMemory, 0, 0, "memory");
    PCM_PARTIAL_RESOURCE_LIST l = new_list(m, 1, 0);
    desc_memory(&l->PartialDescriptors[0], reg[0], reg[1]);
}

/* ---- SCSI: the controller, and the bus probed through Open Firmware ------------------------ */
static const CHAR *scsi_type_name(UCHAR t, CONFIGURATION_TYPE *ctl, CONFIGURATION_TYPE *per, ULONG *flags)
{
    switch (t & 0x1F) {
    case 0x00: *ctl = DiskController; *per = DiskPeripheral; *flags = ARC_FLAG_INPUT | ARC_FLAG_OUTPUT; return "disk";
    case 0x01: *ctl = TapeController; *per = TapePeripheral; *flags = ARC_FLAG_INPUT | ARC_FLAG_OUTPUT | ARC_FLAG_REMOVABLE; return "tape";
    case 0x04: *ctl = WormController; *per = DiskPeripheral; *flags = ARC_FLAG_INPUT | ARC_FLAG_OUTPUT | ARC_FLAG_REMOVABLE; return "worm";
    case 0x05: *ctl = CdromController; *per = FloppyDiskPeripheral; *flags = ARC_FLAG_INPUT | ARC_FLAG_READONLY | ARC_FLAG_REMOVABLE; return "cdrom";
    case 0x07: *ctl = DiskController; *per = FloppyDiskPeripheral; *flags = ARC_FLAG_INPUT | ARC_FLAG_OUTPUT | ARC_FLAG_REMOVABLE; return "disk";
    default: return NULL;
    }
}

/* Which targets to ask.  Every absent target costs Open Firmware a selection timeout --
 * 0.38 billion instructions each on the emulator, 2.3 billion per controller for a scan of
 * 0..15 (measured 5 November), which is more than the rest of the boot to the kernel -- and
 * the veneer's tree never had anything but target 0.  So: target 0, plus whatever the boot
 * variables name as scsi(this)disk(N), plus ARCFW-SCSI-TARGETS on /options ('0,3,5' per
 * controller key, 'all' for everything) for a machine with more. */
static ULONG wanted_targets(ULONG ctl_key)
{
    ULONG mask = 1;
    static const CHAR *vars[] = { "SYSTEMPARTITION", "OSLOADPARTITION", "OSLOADER", "ARCFW-SCSI-TARGETS" };
    for (ULONG v = 0; v < 4; v++) {
        const CHAR *val = env_lookup(vars[v]);
        if (!val) continue;
        if (v == 3) {
            if (strcmp(val, "all") == 0) return 0xFFFF;
            for (const CHAR *e = val; *e; e++) if (*e >= '0' && *e <= '9') mask |= 1u << (ULONG)(*e - '0');
            continue;
        }
        for (const CHAR *e = val; *e; e++) {
            if (strncmp(e, "scsi(", 5) == 0) {
                ULONG c = 0; const CHAR *q = e + 5;
                while (*q >= '0' && *q <= '9') c = c * 10 + (ULONG)(*q++ - '0');
                if (c != ctl_key || strncmp(q, ")disk(", 6) != 0) continue;
                ULONG t = 0; q += 6;
                while (*q >= '0' && *q <= '9') t = t * 10 + (ULONG)(*q++ - '0');
                if (t < 16) mask |= 1u << t;
            }
        }
    }
    return mask;
}

static VOID probe_scsi(PARC_NODE adapter, OF_PHANDLE ctl, const CHAR *ctl_path)
{
    ULONG initiator = 7;
    of_get_int(ctl, "scsi-initiator-id", &initiator);
    OF_IHANDLE ih = of_open(ctl_path);
    if (!ih) { con_printf("arcfw: cannot open %s to probe it\n", ctl_path); return; }
    static UCHAR inq[256] __attribute__((aligned(16)));
    static UCHAR cdb[6] __attribute__((aligned(16)));
    ULONG wanted = wanted_targets(adapter->Component.Key);
    for (ULONG target = 0; target < 16; target++) {
        if (target == initiator || !(wanted & (1u << target))) continue;
        /* set-address ( lun target -- ), target on top: the array lists the top first, as the
         * veneer's registers do (r8 = target, r9 = 0) */
        ULONG in[2] = { target, 0 }, out[2];
        if (of_call_method("set-address", ih, 2, 0, in, NULL)) continue;
        memset(inq, 0, sizeof inq);
        cdb[0] = 0x12; cdb[1] = 0; cdb[2] = 0; cdb[3] = 0; cdb[4] = 0xFF; cdb[5] = 0;
        /* short-data-command ( data-len cmd-adr cmd-len -- hwresult data-adr ): the veneer passes
         * (6, cdb, 0xFF) top first and takes two results, a status that must be 0 and the
         * package's own buffer holding the INQUIRY data */
        ULONG cin[3] = { 6, (ULONG)cdb, 0xFF };
        if (of_call_method("short-data-command", ih, 3, 2, cin, out)) continue;
        if (out[0] != 0) continue;                          /* hardware error / no selection */
        UCHAR *data = (UCHAR *)out[1] ? (UCHAR *)out[1] : inq;
        if (data[0] == 0x7F || (data[0] & 0xE0) == 0x60) continue;
        CONFIGURATION_TYPE ct, pt; ULONG flags;
        if (!scsi_type_name(data[0], &ct, &pt, &flags)) continue;
        CHAR ident[32];
        memcpy(ident, data + 8, 28); ident[28] = 0;
        for (ULONG i = 0; i < 28; i++) if (ident[i] < 0x20 || ident[i] > 0x7E) ident[i] = ' ';
        DBG(DBG_CONFIG, "arcfw: %s target %u: type %02x '%s'\n", ctl_path, target, data[0], ident);
        PARC_NODE c = new_node(adapter, ControllerClass, ct, target, flags, ident);
        c->OfPhandle = ctl; c->Unit = target;
        c->OfPath = zalloc(strlen(ctl_path) + 16);
        con_snprintf(c->OfPath, strlen(ctl_path) + 16, "%s/sd@%x,0", ctl_path, target);
        PARC_NODE p = new_node(c, PeripheralClass, pt, 0, flags, NULL);
        p->OfPath = c->OfPath; p->Unit = target;
    }
    of_close(ih);
}

static VOID add_scsi_adapter(PARC_NODE parent, OF_PHANDLE node, const CHAR *path, BOOLEAN pci)
{
    CHAR model[32];
    of_get_str(node, "model", model, sizeof model);
    /* NCRC810 is the identifier the HAL repairs and NT's symc810 miniport serves: the
     * Network Server's on-board 53C825As (model "NCR,53C825"), and a Symbios card in a Power
     * Macintosh slot, whose node carries no model -- the card has no FCode -- only its PCI ids */
    ULONG vendor = 0;
    of_get_int(node, "vendor-id", &vendor);
    const CHAR *ident = strncmp(model, "NCR,", 4) == 0 || (pci && vendor == 0x1000) ? "NCRC810" : "UNKNOWN SCSI";
    PARC_NODE a = new_node(parent, AdapterClass, ScsiAdapter, scsi_adapters++, 0, ident);
    a->OfPhandle = node; a->OfPath = strdup_(path);
    if (pci) {
        /* the real registers and interrupt, from assigned-addresses and AAPL,interrupts; the
         * veneer's list had a port at 0 and interrupt 1, which the HAL had to repair */
        ULONG aa[40]; LONG n = of_get_ints(node, "assigned-addresses", aa, sizeof aa);
        ULONG mem = 0, memlen = 0, io = 0, iolen = 0, irq = 0;
        for (LONG i = 0; i + 4 < n / 4 + 1 && i + 4 <= n / 4; i += 5) {
            ULONG space = (aa[i] >> 24) & 3;
            if (space == 1 && !io) { io = aa[i + 2]; iolen = aa[i + 4]; }
            if (space == 2 && !mem && aa[i + 4] == 0x100) { mem = aa[i + 2]; memlen = aa[i + 4]; }
        }
        of_get_int(node, "AAPL,interrupts", &irq);
        PCM_PARTIAL_RESOURCE_LIST l = new_list(a, 3, 0);
        desc_memory(&l->PartialDescriptors[0], mem, memlen ? memlen : 0x100);
        desc_port(&l->PartialDescriptors[1], io, iolen ? iolen : 0x100);
        desc_interrupt(&l->PartialDescriptors[2], irq, irq);
        probe_scsi(a, node, path);
    } else {
        PCM_PARTIAL_RESOURCE_LIST l = new_list(a, 1, 0);
        desc_null(&l->PartialDescriptors[0], 0x200);
    }
}

/* ---- Grand Central's devices, as children of the PCI adapter ------------------------------- */
static VOID add_gc_devices(PARC_NODE pci, OF_PHANDLE gc, const CHAR *gc_path)
{
    ULONG serial_key = 0;
    for (OF_PHANDLE c = of_child(gc); c; c = of_peer(c)) {
        CHAR name[32], dt[32], path[128];
        of_get_str(c, "name", name, sizeof name);
        of_get_str(c, "device_type", dt, sizeof dt);
        of_package_to_path(c, path, sizeof path);
        if (strcmp(dt, "scsi") == 0) {
            add_scsi_adapter(pci, c, path, FALSE);
        } else if (strcmp(dt, "network") == 0) {
            PARC_NODE n = new_node(pci, ControllerClass, NetworkController, 0, ARC_FLAG_INPUT | ARC_FLAG_OUTPUT, "UNKNOWN NETWORK");
            n->OfPhandle = c; n->OfPath = strdup_(path);
            PCM_PARTIAL_RESOURCE_LIST l = new_list(n, 1, 0); desc_null(&l->PartialDescriptors[0], 0x100);
            new_node(n, PeripheralClass, NetworkPeripheral, 0, ARC_FLAG_INPUT | ARC_FLAG_OUTPUT, NULL);
        } else if (strcmp(name, "escc") == 0) {
            for (OF_PHANDLE ch = of_child(c); ch; ch = of_peer(ch)) {
                CHAR cname[32], cpath[128];
                of_get_str(ch, "name", cname, sizeof cname);
                of_package_to_path(ch, cpath, sizeof cpath);
                ULONG flags = ARC_FLAG_CONSOLEIN | ARC_FLAG_CONSOLEOUT | ARC_FLAG_INPUT | ARC_FLAG_OUTPUT;
                PARC_NODE s = new_node(pci, ControllerClass, SerialController, serial_key, flags, cname);
                s->OfPhandle = ch; s->OfPath = strdup_(cpath);
                PCM_PARTIAL_RESOURCE_LIST l = new_list(s, 2, 8);
                desc_null(&l->PartialDescriptors[0], 0x100);
                PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &l->PartialDescriptors[1];
                d->Type = CmResourceTypeDeviceSpecific; d->ShareDisposition = CmResourceShareDeviceExclusive;
                d->u.DeviceSpecificData.DataSize = 8;
                static const UCHAR veneer_serial[8] = { 0x01, 0x00, 0x02, 0x00, 0x00, 0x20, 0x1c, 0x00 };
                memcpy((UCHAR *)l + 8 + 2 * sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR), veneer_serial, 8);
                PARC_NODE line = new_node(s, PeripheralClass, LinePeripheral, serial_key, flags, NULL);
                line->OfPath = s->OfPath;
                serial_key++;
            }
        } else if (strcmp(name, "swim3") == 0) {
            /* ledger row 17 by construction: the floppy drive is multi(0)disk(0)fdisk(0) */
            ULONG flags = ARC_FLAG_INPUT | ARC_FLAG_OUTPUT | ARC_FLAG_REMOVABLE;
            PARC_NODE f = new_node(pci, ControllerClass, DiskController, 0, flags, "swim3");
            f->OfPhandle = c; f->OfPath = strdup_(path);
            PCM_PARTIAL_RESOURCE_LIST l = new_list(f, 1, 0); desc_null(&l->PartialDescriptors[0], 0x100);
            PARC_NODE fd = new_node(f, PeripheralClass, FloppyDiskPeripheral, 0, flags, NULL);
            fd->OfPath = f->OfPath;
        } else if (strcmp(name, "via-cuda") == 0) {
            for (OF_PHANDLE adb = of_child(c); adb; adb = of_peer(adb)) {
                CHAR an[32]; of_get_str(adb, "name", an, sizeof an);
                if (strcmp(an, "adb") != 0) continue;
                for (OF_PHANDLE dev = of_child(adb); dev; dev = of_peer(dev)) {
                    CHAR dn[32]; of_get_str(dev, "name", dn, sizeof dn);
                    if (strcmp(dn, "keyboard") == 0) {
                        PARC_NODE k = new_node(pci, ControllerClass, KeyboardController, 0, ARC_FLAG_CONSOLEIN | ARC_FLAG_INPUT, "adb");
                        new_node(k, PeripheralClass, KeyboardPeripheral, 0, ARC_FLAG_CONSOLEIN | ARC_FLAG_INPUT, "PCAT_ENHANCED");
                    } else if (strcmp(dn, "mouse") == 0) {
                        PARC_NODE m = new_node(pci, ControllerClass, PointerController, 0, ARC_FLAG_INPUT, "adb");
                        new_node(m, PeripheralClass, PointerPeripheral, 0, ARC_FLAG_INPUT, "PS2 MOUSE");
                    }
                }
            }
        }
    }
    (void)gc_path;
}

/* ---- a PCI bridge (Bandit) and what hangs off it ------------------------------------------ */
static VOID add_bandit(OF_PHANDLE bandit, ULONG key)
{
    ULONG reg[4] = { 0xF2000000u, 0x02000000u };
    of_get_ints(bandit, "reg", reg, sizeof reg);
    PARC_NODE pci = new_node(root, AdapterClass, MultiFunctionAdapter, key, 0, "PCI");
    pci->OfPhandle = bandit;
    PCM_PARTIAL_RESOURCE_LIST l = new_list(pci, 1, 0);
    desc_port(&l->PartialDescriptors[0], reg[0], reg[1]);
    for (OF_PHANDLE c = of_child(bandit); c; c = of_peer(c)) {
        CHAR name[32], dt[32], path[128];
        of_get_str(c, "name", name, sizeof name);
        of_get_str(c, "device_type", dt, sizeof dt);
        of_package_to_path(c, path, sizeof path);
        /* A card without FCode gets a node named pci<vendor>,<device> and no device_type (a
         * Power Macintosh's slots, where a Symbios SCSI card or a Cirrus VGA card has none);
         * its PCI class code says what it is instead. */
        ULONG cls = 0xFFFFFFFFu;
        of_get_int(c, "class-code", &cls);
        if (strcmp(dt, "scsi") == 0 || (!dt[0] && (cls >> 8) == 0x0100u)) {
            add_scsi_adapter(pci, c, path, TRUE);
        } else if (strcmp(dt, "dbdma") == 0 || strcmp(name, "gc") == 0) {
            add_gc_devices(pci, c, path);
        } else if (strcmp(dt, "display") == 0 || (!dt[0] && (cls >> 16) == 0x03u)) {
            CHAR model[32]; of_get_str(c, "model", model, sizeof model);
            PARC_NODE v = new_node(pci, ControllerClass, DisplayController, 0, ARC_FLAG_CONSOLEOUT | ARC_FLAG_OUTPUT, model[0] ? model : name);
            v->OfPhandle = c; v->OfPath = strdup_(path);
            ULONG aa[20]; LONG n = of_get_ints(c, "assigned-addresses", aa, sizeof aa);
            ULONG fb = 0, fblen = 0x01000000;
            for (LONG i = 0; i + 4 < n / 4 + 1 && i + 4 <= n / 4; i += 5)
                if (((aa[i] >> 24) & 3) == 2 && aa[i + 4] >= 0x100000) { fb = aa[i + 2]; fblen = aa[i + 4]; }
            PCM_PARTIAL_RESOURCE_LIST dl = new_list(v, 1, 0);
            desc_memory(&dl->PartialDescriptors[0], fb, fblen);
            new_node(v, PeripheralClass, MonitorPeripheral, 0, ARC_FLAG_CONSOLEOUT | ARC_FLAG_OUTPUT, "1024x768");
        }
    }
}

/* The veneer's device-specific root data: a run of strings naming the firmware, then two pairs
 * this firmware adds for the HAL, which has no other way to learn them before it starts the
 * clock: the machine (Open Firmware's root `compatible`, first entry: "AAPL,8500" on a Power
 * Macintosh 8500) and the processor's timebase in hertz (the cpu node's timebase-frequency,
 * 11 MHz on a Network Server, 10 MHz on a 120 MHz 8500).  A HAL that does not look ignores
 * them; the kernel copies the data into the registry unread. */
static VOID add_root_data(OF_PHANDLE ofroot, ULONG tb_hz)
{
    static const CHAR head[] = "Firmware\0OpenFirmware2.26\0Veneer\0powermac-nt-hal,arcfw,0.1,GENERAL\0";
    static CHAR tail[sizeof head + 96];
    CHAR model[48] = "";
    of_get_str(ofroot, "compatible", model, sizeof model);    /* the first string of the list */
    ULONG at = sizeof head - 1;
    memcpy(tail, head, at);
    at += (ULONG)con_snprintf(tail + at, sizeof tail - at, "Model") + 1;
    at += (ULONG)con_snprintf(tail + at, sizeof tail - at, "%s", model) + 1;
    at += (ULONG)con_snprintf(tail + at, sizeof tail - at, "TimebaseHz") + 1;
    at += (ULONG)con_snprintf(tail + at, sizeof tail - at, "%u", tb_hz) + 1;
    ULONG dlen = at + 1;                                       /* and an empty string to end */
    tail[at] = 0;
    PCM_PARTIAL_RESOURCE_LIST l = new_list(root, 1, dlen);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &l->PartialDescriptors[0];
    d->Type = CmResourceTypeDeviceSpecific; d->ShareDisposition = CmResourceShareDeviceExclusive;
    d->u.DeviceSpecificData.DataSize = dlen;
    memcpy((UCHAR *)l + 8 + sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR), tail, dlen);
    DBG(DBG_CONFIG, "arcfw: machine '%s', timebase %u Hz\n", model, tb_hz);
}

VOID config_build(VOID)
{
    OF_PHANDLE ofroot = of_finddevice("/");
    root = new_node(NULL, SystemClass, ArcSystem, 0, 0, "device-tree");
    root->OfPhandle = ofroot;
    add_root_data(ofroot, of_timebase_hz());
    OF_PHANDLE mem = OF_NONE;
    ULONG bandits = 0, bridges = 2;
    for (OF_PHANDLE c = of_child(ofroot); c; c = of_peer(c)) {
        CHAR name[32], dt[32];
        of_get_str(c, "name", name, sizeof name);
        of_get_str(c, "device_type", dt, sizeof dt);
        if (strcmp(dt, "cpu") == 0) add_cpu(c);
        else if (strcmp(dt, "memory") == 0) { mem = c; add_memory(c); }
    }
    /* the Bandits after the processor and memory, in address order (F2000000 before F4000000) */
    for (OF_PHANDLE c = of_child(ofroot); c; c = of_peer(c)) {
        CHAR dt[32]; of_get_str(c, "device_type", dt, sizeof dt);
        if (strcmp(dt, "pci") == 0) add_bandit(c, bandits++);
    }
    /* the Bandits' own PCI device nodes, as the veneer listed them */
    for (OF_PHANDLE c = of_child(ofroot); c; c = of_peer(c)) {
        CHAR dt[32]; of_get_str(c, "device_type", dt, sizeof dt);
        if (strcmp(dt, "pci") != 0) continue;
        for (OF_PHANDLE d = of_child(c); d; d = of_peer(d)) {
            CHAR name[32]; of_get_str(d, "name", name, sizeof name);
            if (strncmp(name, "pci106b", 7) == 0) new_node(root, AdapterClass, MultiFunctionAdapter, bridges++, 0, "pci106b-1");
        }
    }
    (void)mem;
}

PARC_NODE config_root(VOID) { return root; }

PARC_NODE config_from_component(PCONFIGURATION_COMPONENT c)
{
    /* the component is the first member of its node */
    return c ? (PARC_NODE)c : NULL;
}

/* ARC path names per type, the specification's table */
static const CHAR *type_names[] = {
    "arc", "cpu", "fpu", "pic", "pdc", "sic", "sdc", "sc", "eisa", "tc", "scsi", "dti", "multi", "disk",
    "tape", "cdrom", "worm", "serial", "net", "video", "par", "point", "key", "audio", "other", "rdisk",
    "fdisk", "tape", "modem", "monitor", "print", "pointer", "keyboard", "term", "other", "line",
    "network", "memory"
};

PARC_NODE config_lookup(const CHAR *path, const CHAR **rest)
{
    PARC_NODE cur = root, best = NULL;
    const CHAR *p = path;
    *rest = path;
    while (*p) {
        CHAR name[16]; ULONG i = 0;
        while (*p && *p != '(' && i + 1 < sizeof name) name[i++] = *p++;
        name[i] = 0;
        if (*p != '(') break;
        p++;
        ULONG key = atoul(p, &p);
        if (*p != ')') break;
        p++;
        if (strcasecmp(name, "partition") == 0) break;
        PARC_NODE found = NULL;
        for (PARC_NODE n = cur->Child; n; n = n->Peer) {
            const CHAR *tn = n->Component.Type < MaximumType ? type_names[n->Component.Type] : "";
            if (strcasecmp(tn, name) == 0 && n->Component.Key == key) { found = n; break; }
        }
        if (!found) break;
        cur = best = found;
        *rest = p;
    }
    return best;
}

VOID config_node_path(PARC_NODE n, CHAR *buf, ULONG len)
{
    if (!n || !n->Parent) { buf[0] = 0; return; }
    config_node_path(n->Parent, buf, len);
    ULONG at = strlen(buf);
    con_snprintf(buf + at, len - at, "%s(%u)", type_names[n->Component.Type], n->Component.Key);
}

VOID config_dump(PARC_NODE n, ULONG depth)
{
    for (; n; n = n->Peer) {
        con_printf("arcfw: ");
        for (ULONG i = 0; i < depth; i++) con_puts("  ");
        con_printf("%u/%u key %u flags %x '%s' cfg %u%s%s\n", n->Component.Class, n->Component.Type,
                   n->Component.Key, n->Component.Flags, n->Component.Identifier ? n->Component.Identifier : "",
                   n->Component.ConfigurationDataLength, n->OfPath ? " " : "", n->OfPath ? n->OfPath : "");
        if (n->Child) config_dump(n->Child, depth + 1);
    }
}
