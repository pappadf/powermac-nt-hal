/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * of.c -- the Open Firmware services this firmware was written against, answered without the
 * ROM.  The ROM stays big-endian and is left behind by stage0 (src/stage0), which hands over a
 * snapshot of its device tree (dtsnap.h).  From that:
 *
 *   - the device tree: finddevice, child/peer/parent, getprop and friends, package-to-path.
 *     setprop (the ARC environment's SetEnvironmentVariable) keeps its values in the heap, in
 *     front of the snapshot's;
 *   - devices, opened by path as the ROM's were: an ESCC channel (polled, as Open Firmware left
 *     it programmed), the floppy (served from the image boot.of read into RAM, include/
 *     oemdisk.h -- NT reads its OEM files from there too), and a 53C825A's bus and the disks and
 *     CD-ROMs on it (scsi.c), read at byte offsets as the ROM's SCSI package read them;
 *   - time from the time base, reset and power-off through Cuda (cuda.c).
 *
 * The APIs are those of the client interface this firmware first used, so the configuration
 * tree, the memory map, the environment and the file systems above are unchanged.  Original. */
#include "arcfw.h"
#include "of.h"
#include "dtsnap.h"
#include "../../include/oemdisk.h"

OF_PHANDLE of_chosen;
OF_IHANDLE of_stdout, of_stdin;

static const UCHAR *snap;
static ULONG nnodes;

static ULONG rd(ULONG off) { return *(const ULONG *)(snap + off); }
static ULONG node(ULONG idx) { return DTSNAP_NODES + idx * DTSNAP_NODE_SIZE; }
static BOOLEAN valid(OF_PHANDLE ph) { return snap && ph >= 1 && ph <= nnodes; }
static ULONG idx_of(OF_PHANDLE ph) { return ph - 1; }
static OF_PHANDLE ph_of(ULONG idx) { return idx == DTSNAP_NONE ? 0 : idx + 1; }
static const CHAR *path_of(ULONG idx) { return (const CHAR *)snap + rd(node(idx) + DTN_PATH); }

/* the last component of a node's path: name[@unit] */
static const CHAR *component(ULONG idx)
{
    const CHAR *p = path_of(idx), *c = p;
    for (; *p; p++) if (*p == '/') c = p + 1;
    return c;
}

/* --- the device tree ------------------------------------------------------------------- */

OF_PHANDLE of_peer(OF_PHANDLE n)
{
    if (n == 0) return snap ? 1 : 0;                           /* peer(0) is the root */
    return valid(n) ? ph_of(rd(node(idx_of(n)) + DTN_PEER)) : 0;
}
OF_PHANDLE of_child(OF_PHANDLE n) { return valid(n) ? ph_of(rd(node(idx_of(n)) + DTN_CHILD)) : 0; }
OF_PHANDLE of_parent(OF_PHANDLE n) { return valid(n) ? ph_of(rd(node(idx_of(n)) + DTN_PARENT)) : 0; }

LONG of_package_to_path(OF_PHANDLE n, CHAR *buf, ULONG len)
{
    if (!valid(n) || !len) return -1;
    const CHAR *p = path_of(idx_of(n));
    ULONG l = strlen(p);
    ULONG c = l < len - 1 ? l : len - 1;
    memcpy(buf, p, c); buf[c] = 0;
    return (LONG)l;
}

/* hex unit addresses compare as numbers where both are hex ("F2000000" = "f2000000"); others
 * (sd@3,0) compare as strings, case-blind */
static BOOLEAN unit_eq(const CHAR *a, ULONG al, const CHAR *b, ULONG bl)
{
    while (al > 1 && *a == '0') a++, al--;
    while (bl > 1 && *b == '0') b++, bl--;
    if (al != bl) return FALSE;
    for (ULONG i = 0; i < al; i++) if (toupper(a[i]) != toupper(b[i])) return FALSE;
    return TRUE;
}

/* does the node's own component match name[@unit]?  A query without a unit matches any unit;
 * a node without one (the ROM's generic "sd" child) matches any queried unit */
static BOOLEAN comp_match(ULONG idx, const CHAR *q, ULONG ql)
{
    const CHAR *c = component(idx);
    ULONG cl = strlen(c), qn = 0, cn = 0;
    while (qn < ql && q[qn] != '@') qn++;
    while (cn < cl && c[cn] != '@') cn++;
    if (qn != cn) return FALSE;
    for (ULONG i = 0; i < qn; i++) if (toupper(q[i]) != toupper(c[i])) return FALSE;
    if (qn == ql || cn == cl) return TRUE;
    return unit_eq(q + qn + 1, ql - qn - 1, c + cn + 1, cl - cn - 1);
}

OF_PHANDLE of_finddevice(const CHAR *path)
{
    if (!snap || !path) return OF_NONE;
    CHAR buf[256];
    const CHAR *p = path;
    if (*p != '/') {
        /* an alias: the first component through /aliases */
        ULONG n = 0;
        while (p[n] && p[n] != '/' && p[n] != ':') n++;
        CHAR name[32];
        if (n == 0 || n >= sizeof name) return OF_NONE;
        memcpy(name, p, n); name[n] = 0;
        OF_PHANDLE al = of_finddevice("/aliases");
        if (al == OF_NONE) return OF_NONE;
        LONG l = of_get_str(al, name, buf, sizeof buf);
        if (l <= 0) return OF_NONE;
        ULONG bl = strlen(buf);
        if (bl + strlen(p + n) + 1 > sizeof buf) return OF_NONE;
        strcpy(buf + bl, p + n);
        p = buf;
        if (*p != '/') return OF_NONE;
    }
    ULONG cur = 0;                                              /* the root */
    while (*p) {
        while (*p == '/') p++;
        if (!*p || *p == ':') break;
        ULONG ql = 0;
        while (p[ql] && p[ql] != '/' && p[ql] != ':') ql++;
        ULONG c = rd(node(cur) + DTN_CHILD), hit = DTSNAP_NONE;
        for (; c != DTSNAP_NONE; c = rd(node(c) + DTN_PEER))
            if (comp_match(c, p, ql)) { hit = c; break; }
        if (hit == DTSNAP_NONE) return OF_NONE;
        cur = hit;
        p += ql;
    }
    return ph_of(cur);
}

/* properties: setprop's values first (a small table in the heap), then the snapshot's */
typedef struct { ULONG node; CHAR name[32]; UCHAR *value; ULONG len; } SETPROP;
#define MAX_SETPROP 48
static SETPROP setprops[MAX_SETPROP];
static ULONG nsetprops;

static SETPROP *find_set(OF_PHANDLE n, const CHAR *name)
{
    for (ULONG i = 0; i < nsetprops; i++)
        if (setprops[i].node == n && strcmp(setprops[i].name, name) == 0) return &setprops[i];
    return NULL;
}
/* the snapshot's property table entry for `name`, or 0 */
static ULONG find_prop(OF_PHANDLE n, const CHAR *name)
{
    ULONG rec = node(idx_of(n)), np = rd(rec + DTN_NPROPS), tab = rd(rec + DTN_PROPS);
    for (ULONG i = 0; i < np; i++) {
        ULONG t = tab + i * DTSNAP_PROP_SIZE;
        if (strcmp((const CHAR *)snap + rd(t + DTP_NAME), name) == 0) return t;
    }
    return 0;
}

LONG of_getproplen(OF_PHANDLE n, const CHAR *name)
{
    if (!valid(n) || !name) return -1;
    SETPROP *s = find_set(n, name);
    if (s) return (LONG)s->len;
    ULONG t = find_prop(n, name);
    return t ? (LONG)rd(t + DTP_LEN) : -1;
}
LONG of_getprop(OF_PHANDLE n, const CHAR *name, PVOID buf, ULONG len)
{
    if (!valid(n) || !name) return -1;
    const UCHAR *v; ULONG l;
    SETPROP *s = find_set(n, name);
    if (s) { v = s->value; l = s->len; }
    else {
        ULONG t = find_prop(n, name);
        if (!t) return -1;
        v = snap + rd(t + DTP_VALUE); l = rd(t + DTP_LEN);
    }
    memcpy(buf, v, l < len ? l : len);
    return (LONG)l;
}
LONG of_nextprop(OF_PHANDLE n, const CHAR *prev, CHAR *name32)
{
    if (!valid(n)) return -1;
    ULONG rec = node(idx_of(n)), np = rd(rec + DTN_NPROPS), tab = rd(rec + DTN_PROPS), i = 0;
    if (prev && *prev) {
        for (; i < np; i++)
            if (strcmp((const CHAR *)snap + rd(tab + i * DTSNAP_PROP_SIZE + DTP_NAME), prev) == 0) break;
        if (i == np) return -1;
        i++;
    }
    if (i >= np) return 0;
    const CHAR *nm = (const CHAR *)snap + rd(tab + i * DTSNAP_PROP_SIZE + DTP_NAME);
    ULONG l = strlen(nm); if (l > 31) l = 31;
    memcpy(name32, nm, l); name32[l] = 0;
    return 1;
}
LONG of_setprop(OF_PHANDLE n, const CHAR *name, const VOID *buf, ULONG len)
{
    if (!valid(n) || !name || strlen(name) > 31) return -1;
    SETPROP *s = find_set(n, name);
    if (!s) {
        if (nsetprops == MAX_SETPROP) return -1;
        s = &setprops[nsetprops++];
        s->node = n; strcpy(s->name, name);
    }
    s->value = zalloc(len + 1);                                 /* the heap never frees: fine for a few */
    memcpy(s->value, buf, len);
    s->len = len;
    return (LONG)len;
}

/* --- devices ------------------------------------------------------------------------------- */

/* Grand Central's ESCC, at its fixed place on these machines (Open Firmware's ttya is channel
 * A); the page table maps it (mmu.c).  Channel B's registers come first. */
#define ESCC_BASE 0xF3013000u
#define ESCC_CTRL(ch) (ESCC_BASE + ((ch) ? 0x00u : 0x20u))
#define ESCC_DATA(ch) (ESCC_BASE + ((ch) ? 0x10u : 0x30u))
static inline VOID eieio(VOID) { __asm__ volatile("eieio" ::: "memory"); }
static VOID escc_put(ULONG ch, CHAR c)
{
    for (ULONG spin = 0; spin < 200000 && !(*(volatile UCHAR *)ESCC_CTRL(ch) & 0x04); spin++) eieio();
    *(volatile UCHAR *)ESCC_DATA(ch) = (UCHAR)c;
    eieio();
}
static LONG escc_get(ULONG ch)
{
    if (!(*(volatile UCHAR *)ESCC_CTRL(ch) & 0x01)) return -1;
    eieio();
    UCHAR c = *(volatile UCHAR *)ESCC_DATA(ch);
    eieio();
    return c;
}

typedef enum { DEV_FREE, DEV_ESCC, DEV_FLOPPY, DEV_SCSI_BUS, DEV_SCSI_UNIT } DEV_TYPE;
typedef struct {
    DEV_TYPE type;
    OF_PHANDLE node;
    ULONG chan;                 /* ESCC channel: 0 = A, 1 = B */
    SCSI_BUS *bus;
    ULONG target;
    ULONG block, blocks;        /* a SCSI unit's block size and count */
    ULONG pos_hi, pos_lo;
    CHAR path[160];
} INST;
#define MAX_INST 24
static INST inst[MAX_INST];
static UCHAR inquiry[96] __attribute__((aligned(32)));

static INST *get(OF_IHANDLE ih) { return (ih >= 1 && ih <= MAX_INST && inst[ih - 1].type != DEV_FREE) ? &inst[ih - 1] : NULL; }

/* the floppy's image in RAM, as stage0 left it: a header NT also reads, then the disk */
static const OEMDISK_HEADER *oemdisk(VOID)
{
    const OEMDISK_HEADER *h = (const OEMDISK_HEADER *)OEMDISK_PHYS;
    if (h->Magic != OEMDISK_MAGIC || h->Version != OEMDISK_VERSION || h->ImageBytes > OEMDISK_MAX_BYTES) return NULL;
    return h;
}

static BOOLEAN name_is(OF_PHANDLE n, const CHAR *want)
{
    CHAR nm[32];
    if (of_get_str(n, "name", nm, sizeof nm) < 0) return FALSE;
    return strcmp(nm, want) == 0;
}
static BOOLEAN is_scsi_bus(OF_PHANDLE n)
{
    CHAR nm[32], dt[32];
    if (of_get_str(n, "name", nm, sizeof nm) >= 0 && strncasecmp(nm, "53c825", 6) == 0) return TRUE;
    return of_get_str(n, "device_type", dt, sizeof dt) >= 0 && strcmp(dt, "scsi") == 0;
}

OF_IHANDLE of_open(const CHAR *path)
{
    if (!path) return 0;
    ULONG i;
    for (i = 0; i < MAX_INST && inst[i].type != DEV_FREE; i++) { }
    if (i == MAX_INST) return 0;
    INST *d = &inst[i];
    memset(d, 0, sizeof *d);
    CHAR dev[160];
    ULONG n = 0;
    while (path[n] && path[n] != ':' && n + 1 < sizeof dev) { dev[n] = path[n]; n++; }
    dev[n] = 0;
    const CHAR *args = path[n] == ':' ? path + n + 1 : "";
    /* the ROM's file-system packages (":N,\file") are not offered: io.c and fat.c read files
     * through the raw device themselves */
    for (const CHAR *a = args; *a; a++) if (*a == ',') return 0;
    for (ULONG k = 0; k + 1 < sizeof d->path && path[k]; k++) d->path[k] = path[k];

    /* a disk or CD-ROM: <bus>/sd@T,L (or cd@), which the snapshot holds as a generic child or
     * not at all */
    const CHAR *last = dev;
    for (const CHAR *p = dev; *p; p++) if (*p == '/') last = p + 1;
    if ((toupper(last[0]) == 'S' || toupper(last[0]) == 'C') && toupper(last[1]) == 'D' && last[2] == '@') {
        CHAR parent[160];
        ULONG pl = (ULONG)(last - dev);
        memcpy(parent, dev, pl); parent[pl ? pl - 1 : 0] = 0;
        OF_PHANDLE b = of_finddevice(parent);
        if (b == OF_NONE || !is_scsi_bus(b)) return 0;
        SCSI_BUS *bus = scsi_bus(b);
        if (!bus) return 0;
        ULONG t = 0;
        for (const CHAR *p = last + 3; *p && *p != ','; p++) t = t * 16 + (ULONG)((*p >= '0' && *p <= '9') ? *p - '0' : (toupper(*p) - 'A' + 10) & 15);
        if (t > 15) return 0;
        ULONG blocks, bs;
        if (scsi_capacity(bus, t, &blocks, &bs) != 0) return 0;
        d->type = DEV_SCSI_UNIT; d->node = b; d->bus = bus; d->target = t; d->block = bs; d->blocks = blocks;
        return i + 1;
    }
    OF_PHANDLE nd = of_finddevice(dev);
    if (nd == OF_NONE) return 0;
    d->node = nd;
    if (name_is(nd, "ch-a") || name_is(nd, "ch-b")) { d->type = DEV_ESCC; d->chan = name_is(nd, "ch-b"); return i + 1; }
    if (name_is(nd, "swim3")) { if (!oemdisk()) return 0; d->type = DEV_FLOPPY; return i + 1; }
    if (is_scsi_bus(nd)) {
        d->bus = scsi_bus(nd);
        if (!d->bus) return 0;
        d->type = DEV_SCSI_BUS;
        return i + 1;
    }
    return 0;
}
VOID of_close(OF_IHANDLE ih) { INST *d = get(ih); if (d) d->type = DEV_FREE; }
OF_PHANDLE of_instance_to_package(OF_IHANDLE ih) { INST *d = get(ih); return d ? d->node : OF_NONE; }
LONG of_instance_to_path(OF_IHANDLE ih, CHAR *buf, ULONG len)
{
    INST *d = get(ih);
    if (!d || !len) return -1;
    ULONG l = strlen(d->path), c = l < len - 1 ? l : len - 1;
    memcpy(buf, d->path, c); buf[c] = 0;
    return (LONG)l;
}

static VOID advance(INST *d, ULONG n) { ULONG lo = d->pos_lo + n; if (lo < d->pos_lo) d->pos_hi++; d->pos_lo = lo; }

LONG of_read(OF_IHANDLE ih, PVOID buf, ULONG len)
{
    INST *d = get(ih);
    if (!d) return -1;
    switch (d->type) {
    case DEV_ESCC: {
        /* Open Firmware's console read did not block; nor does this one */
        ULONG got = 0;
        while (got < len) { LONG c = escc_get(d->chan); if (c < 0) break; ((UCHAR *)buf)[got++] = (UCHAR)c; }
        return (LONG)got;
    }
    case DEV_FLOPPY: {
        const OEMDISK_HEADER *h = oemdisk();
        if (!h || d->pos_hi || d->pos_lo >= h->ImageBytes) return 0;
        ULONG n = h->ImageBytes - d->pos_lo; if (n > len) n = len;
        memcpy(buf, (const UCHAR *)OEMDISK_IMAGE_PHYS + d->pos_lo, n);
        advance(d, n);
        return (LONG)n;
    }
    case DEV_SCSI_UNIT: {
        ULONGLONG end = ((ULONGLONG)d->blocks) * d->block;
        ULONGLONG pos = ((ULONGLONG)d->pos_hi << 32) | d->pos_lo;
        if (pos >= end) return 0;
        if (pos + len > end) len = (ULONG)(end - pos);
        if (scsi_read_bytes(d->bus, d->target, d->block, d->pos_hi, d->pos_lo, buf, len) != 0) return -1;
        advance(d, len);
        return (LONG)len;
    }
    default:
        return -1;
    }
}
LONG of_write(OF_IHANDLE ih, const VOID *buf, ULONG len)
{
    INST *d = get(ih);
    if (!d || d->type != DEV_ESCC) return -1;                  /* the disks are read only here */
    for (ULONG k = 0; k < len; k++) escc_put(d->chan, ((const CHAR *)buf)[k]);
    return (LONG)len;
}
LONG of_seek(OF_IHANDLE ih, ULONG pos_hi, ULONG pos_lo)
{
    INST *d = get(ih);
    if (!d || (d->type != DEV_FLOPPY && d->type != DEV_SCSI_UNIT)) return -1;
    d->pos_hi = pos_hi; d->pos_lo = pos_lo;
    return 0;
}

/* The configuration probe's two SCSI bus methods.  The arrays hold the stack top first:
 * set-address ( lun target -- ) is { target, lun }; short-data-command ( data-len cmd-adr
 * cmd-len -- hwresult data-adr ) is { cmd-len, cmd-adr, data-len } in and { hwresult,
 * data-adr } out, as config.c reads them. */
LONG of_call_method(const CHAR *method, OF_IHANDLE ih, ULONG nin, ULONG nout, ULONG *in, ULONG *out)
{
    INST *d = get(ih);
    if (!d || d->type != DEV_SCSI_BUS) return -1;
    if (strcmp(method, "set-address") == 0 && nin >= 1) { d->target = in[0]; return 0; }
    if (strcmp(method, "short-data-command") == 0 && nin >= 3 && nout >= 2 && in[0] <= 12) {
        /* The polled script moves exactly the length it is given and calls an early change of
         * phase a failure, so an INQUIRY asks for the standard 36 bytes, which every target
         * sends in full, rather than the 255 the probe allows for. */
        UCHAR cdb[12];
        memcpy(cdb, (const UCHAR *)in[1], in[0]);
        ULONG dlen = in[2] < sizeof inquiry ? in[2] : sizeof inquiry;
        if (cdb[0] == 0x12 && in[0] == 6) { dlen = 36; cdb[4] = 36; }
        memset(inquiry, 0, sizeof inquiry);
        LONG st = scsi_command(d->bus, d->target, cdb, in[0], inquiry, dlen, FALSE);
        out[0] = st == 0 ? 0 : (ULONG)-1;
        out[1] = (ULONG)inquiry;
        return 0;
    }
    return -1;
}

/* --- memory, time, reset ----------------------------------------------------------------- */

LONG of_map(ULONG phys, ULONG virt, ULONG size, LONG mode) { (void)phys; (void)virt; (void)size; (void)mode; return 0; }
LONG of_claim_real(ULONG addr, ULONG size)
{
    /* KSEG0 is mapped onto the first 8 MB, so a program linked there is placed physically at
     * its address less 0x80000000; the memory descriptors record it (load.c) */
    ULONG p = addr & 0x7FFFFFFFu;
    if (p + size > ARC_KSEG0_SIZE && addr >= ARC_KSEG0) return -1;
    return 0;
}

static ULONG tb_per_ms;
static ULONG tb_lo(VOID) { ULONG t; __asm__ volatile("mftb %0" : "=r"(t)); return t; }
ULONG of_milliseconds(VOID)
{
    static ULONG last, ms, rem;
    ULONG now = tb_lo(), d = now - last;
    last = now;
    rem += d;
    ms += rem / tb_per_ms;
    rem %= tb_per_ms;
    return ms;
}

LONG of_interpret(const CHAR *forth)
{
    if (strcmp(forth, "reset-all") == 0) cuda_reset_system();
    if (strcmp(forth, "shut-down") == 0) cuda_power_down();
    return -1;
}
VOID of_exit(VOID)
{
    con_puts("\narcfw: stopped.  Open Firmware is not there to return to: reset or power the machine off.\n");
    for (;;) { }
}
VOID of_enter(VOID)
{
    con_puts("\narcfw: there is no firmware prompt to enter (Open Firmware was left at boot)\n");
}

/* --- property helpers ---------------------------------------------------------------------- */

ULONG of_bswap(ULONG v) { return (v << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24); }
LONG of_get_ints(OF_PHANDLE n, const CHAR *name, PULONG buf, ULONG len)
{
    /* integers in a property are big-endian (encode-int), whatever mode reads them */
    LONG l = of_getprop(n, name, buf, len);
    for (LONG i = 0; i + 4 <= l && (ULONG)i + 4 <= len; i += 4) buf[i / 4] = of_bswap(buf[i / 4]);
    return l;
}
BOOLEAN of_get_int(OF_PHANDLE n, const CHAR *name, PULONG out)
{
    ULONG v;
    if (of_get_ints(n, name, &v, 4) != 4) return FALSE;
    *out = v;
    return TRUE;
}
LONG of_get_str(OF_PHANDLE n, const CHAR *name, CHAR *buf, ULONG len)
{
    LONG l = of_getprop(n, name, buf, len - 1);
    if (l < 0) { buf[0] = 0; return -1; }
    buf[(ULONG)l < len - 1 ? (ULONG)l : len - 1] = 0;
    return l;
}
BOOLEAN of_has_prop(OF_PHANDLE n, const CHAR *name) { return of_getproplen(n, name) >= 0; }

/* the first node whose device_type is `type` */
static OF_PHANDLE find_type(OF_PHANDLE n, const CHAR *type)
{
    for (; n; n = of_peer(n)) {
        CHAR dt[32];
        if (of_get_str(n, "device_type", dt, sizeof dt) >= 0 && strcmp(dt, type) == 0) return n;
        OF_PHANDLE c = find_type(of_child(n), type);
        if (c) return c;
    }
    return 0;
}

VOID of_init(ULONG snapshot)
{
    snap = (const UCHAR *)snapshot;
    if (rd(DTSNAP_MAGIC_OFF) != DTSNAP_MAGIC) { snap = NULL; return; }
    nnodes = rd(DTSNAP_COUNT_OFF);
    of_chosen = of_finddevice("/chosen");
    /* the console: the ESCC's channel A, Open Firmware's ttya */
    for (ULONG k = 0; k < nnodes; k++) {
        if (name_is(ph_of(k), "ch-a")) {
            CHAR p[160];
            of_package_to_path(ph_of(k), p, sizeof p);
            of_stdout = of_stdin = of_open(p);
            break;
        }
    }
    /* the time base: the cpu node's timebase-frequency, else the bus clock / 4 */
    ULONG f = 0;
    OF_PHANDLE cpu = find_type(of_peer(0), "cpu");
    if (cpu && !of_get_int(cpu, "timebase-frequency", &f)) {
        ULONG bus = 0;
        if (of_get_int(cpu, "bus-frequency", &bus) || of_get_int(of_peer(0), "clock-frequency", &bus)) f = bus / 4;
    }
    tb_per_ms = f >= 1000000 ? f / 1000 : 10000;
}
