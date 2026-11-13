/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * stage0.c -- the boot floppy's big-endian front end: Open Firmware left as the factory set it.
 *
 * The ARC firmware (src/arcfw) runs little-endian, as Windows NT does.  It used to be started by
 * a firmware already running little-endian, which on these machines means `setenv
 * little-endian? true` and `reset-all`: a persistent NVRAM setting Apple never supported, which
 * has left real Macs needing an NVRAM reset (at least one could not be recovered).  stage0
 * instead runs as an ordinary big-endian client of Open Firmware and switches the CPU and the
 * PCI bridges to little-endian itself, until the next reset -- the approach of maciNTosh's
 * loader on later Macs.
 *
 * With the firmware still alive it takes everything the ARC firmware used to ask it for later:
 *
 *   - a snapshot of the whole device tree (every node, its path, every property), which the
 *     ARC firmware serves its configuration tree, memory map and environment from (of.c);
 *   - the bridges' addresses, for the switch;
 *   - the OEM disk boot.of read into RAM (include/oemdisk.h), whose header it writes.
 *
 * Then it leaves the firmware (stage0.S): temporary BATs; the ARC firmware copied to its link
 * address, the snapshot to DTSNAP_PHYS and the OEM disk left in place, each laid out for a
 * little-endian reader; the bridges switched; into the ARC firmware in real mode.
 *
 * The little-endian layout.  A 60x in little-endian mode does not swap bytes: it munges the
 * address (a byte at A is read at A ^ 7, a word at A ^ 4) and reads the bytes there in its
 * usual order.  So a byte stream S that little-endian code is to see as S must sit in memory
 * with each aligned doubleword's eight bytes reversed -- what a lane-reversing bridge's DMA
 * would have left, and what the SWIM3's DMA did leave when Open Firmware read the floppy
 * little-endian.  stage0 writes every structure as the byte stream little-endian code reads
 * (integers least significant byte first), then reverses each doubleword.  Original. */
#include <stdint.h>
#include "../../include/oemdisk.h"
#include "../arcfw/dtsnap.h"

typedef uint32_t u32;
typedef uint8_t u8;

u32 stage0_cif;
u8 stage0_stack[16384] __attribute__((aligned(16)));
static u8 snap[DTSNAP_MAX] __attribute__((aligned(8)));

extern const u8 stage0_payload[], stage0_payload_end[];
int stage0_cif_call(u32 *args);
void stage0_take(void);
void stage0_flush(u32 addr, u32 len);
void stage0_enter(u32 entry, u32 blob, u32 ram, const u32 *bandits, u32 n) __attribute__((noreturn));

#define ARCFW_BASE  0x00050000u     /* src/arcfw/arcfw.ld */
#define STAGE0_BASE 0x00600000u     /* stage0.ld */

/* --- the client interface (big-endian: the firmware's own mode) -------------------------- */

static u32 cif(const char *svc, int nargs, int nrets, const u32 *in, u32 *out)
{
    u32 a[16];
    a[0] = (u32)svc; a[1] = (u32)nargs; a[2] = (u32)nrets;
    for (int i = 0; i < nargs; i++) a[3 + i] = in[i];
    for (int i = 0; i < nrets; i++) a[3 + nargs + i] = 0;
    int r = stage0_cif_call(a);
    for (int i = 0; i < nrets; i++) out[i] = a[3 + nargs + i];
    return (u32)r;
}
static u32 slen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }
static u32 of_stdout;
static void puts_(const char *s)
{
    if (!of_stdout) return;
    u32 a[3] = { of_stdout, (u32)s, slen(s) }, r;
    cif("write", 3, 1, a, &r);
}
static void puthex(u32 v)
{
    char b[11] = "0x";
    for (int i = 0; i < 8; i++) b[2 + i] = "0123456789ABCDEF"[(v >> (28 - 4 * i)) & 15];
    b[10] = 0;
    puts_(b);
}
static void fail(const char *why) __attribute__((noreturn));
static void fail(const char *why)
{
    puts_("stage0: "); puts_(why); puts_("\r\n");
    cif("exit", 0, 0, 0, 0);
    for (;;) { }
}
static u32 finddevice(const char *path) { u32 a[1] = { (u32)path }, r = 0; return cif("finddevice", 1, 1, a, &r) ? DTSNAP_NONE : r; }
static u32 walk(const char *svc, u32 ph) { u32 a[1] = { ph }, r = 0; return cif(svc, 1, 1, a, &r) ? 0 : r; }
static int getprop(u32 ph, const char *name, void *buf, u32 len)
{
    u32 a[4] = { ph, (u32)name, (u32)buf, len }, r = 0;
    return cif("getprop", 4, 1, a, &r) ? -1 : (int)r;
}
static int getproplen(u32 ph, const char *name) { u32 a[2] = { ph, (u32)name }, r = 0; return cif("getproplen", 2, 1, a, &r) ? -1 : (int)r; }
static int nextprop(u32 ph, const char *prev, char *buf) { u32 a[3] = { ph, (u32)prev, (u32)buf }, r = 0; return cif("nextprop", 3, 1, a, &r) ? -1 : (int)r; }
static int pkgpath(u32 ph, char *buf, u32 len) { u32 a[3] = { ph, (u32)buf, len }, r = 0; return cif("package-to-path", 3, 1, a, &r) ? -1 : (int)r; }
static int streq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }

/* --- the snapshot (dtsnap.h), written as the little-endian byte stream ------------------- */

static u32 used;
static void put32(u32 off, u32 v) { snap[off] = (u8)v; snap[off + 1] = (u8)(v >> 8); snap[off + 2] = (u8)(v >> 16); snap[off + 3] = (u8)(v >> 24); }
static u32 alloc(u32 n)
{
    u32 at = (used + 7) & ~7u;
    if (at + n > DTSNAP_MAX) fail("the device tree does not fit the snapshot");
    used = at + n;
    return at;
}
static u32 put_bytes(const void *p, u32 n)
{
    u32 at = alloc(n);
    for (u32 i = 0; i < n; i++) snap[at + i] = ((const u8 *)p)[i];
    return at;
}

static u32 last_child[DTSNAP_MAX_NODES];
static u8 propbuf[65536];
static u32 nnodes;

static u32 node_rec(u32 idx) { return DTSNAP_NODES + idx * DTSNAP_NODE_SIZE; }

/* one node, linked under `parent`, with its path and all its properties */
static u32 add_node(u32 ph, u32 parent)
{
    if (nnodes == DTSNAP_MAX_NODES) fail("too many device-tree nodes");
    u32 idx = nnodes++, rec = node_rec(idx);
    put32(rec + DTN_PARENT, parent);
    put32(rec + DTN_CHILD, DTSNAP_NONE);
    put32(rec + DTN_PEER, DTSNAP_NONE);
    put32(rec + DTN_OFPH, ph);
    last_child[idx] = DTSNAP_NONE;
    if (parent != DTSNAP_NONE) {
        if (last_child[parent] == DTSNAP_NONE) put32(node_rec(parent) + DTN_CHILD, idx);
        else put32(node_rec(last_child[parent]) + DTN_PEER, idx);
        last_child[parent] = idx;
    }
    char path[256];
    int pl = pkgpath(ph, path, sizeof path - 1);
    if (pl < 0) pl = 0;
    path[pl] = 0;
    put32(rec + DTN_PATH, put_bytes(path, (u32)pl + 1));

    /* properties: count them, then a table of (name, length, value) triples */
    char prev[32], name[32];
    u32 np = 0;
    prev[0] = 0;
    while (nextprop(ph, prev, name) == 1) {
        np++;
        for (u32 i = 0; i < sizeof prev; i++) { prev[i] = name[i]; if (!name[i]) break; }
    }
    u32 tab = alloc(np * DTSNAP_PROP_SIZE), k = 0;
    prev[0] = 0;
    while (k < np && nextprop(ph, prev, name) == 1) {
        int len = getproplen(ph, name);
        u32 t = tab + k * DTSNAP_PROP_SIZE;
        put32(t + DTP_NAME, put_bytes(name, slen(name) + 1));
        if (len < 0 || (u32)len > sizeof propbuf || getprop(ph, name, propbuf, (u32)len) != len) len = 0;
        put32(t + DTP_LEN, (u32)len);
        put32(t + DTP_VALUE, put_bytes(propbuf, (u32)len));
        k++;
        for (u32 i = 0; i < sizeof prev; i++) { prev[i] = name[i]; if (!name[i]) break; }
    }
    put32(rec + DTN_NPROPS, k);
    put32(rec + DTN_PROPS, tab);
    return idx;
}

/* the whole tree, depth first, without recursion */
static void snapshot(u32 ram, u32 oemdisk_blocks)
{
    used = DTSNAP_NODES + DTSNAP_MAX_NODES * DTSNAP_NODE_SIZE;
    nnodes = 0;
    u32 stack_ph[64], stack_idx[64];
    int sp = 0;
    u32 root = finddevice("/");
    if (root == DTSNAP_NONE) fail("no root node");
    u32 ridx = add_node(root, DTSNAP_NONE);
    stack_ph[sp] = walk("child", root); stack_idx[sp] = ridx; sp++;
    while (sp > 0) {
        u32 ph = stack_ph[sp - 1], parent = stack_idx[sp - 1];
        if (!ph || ph == DTSNAP_NONE) { sp--; continue; }
        stack_ph[sp - 1] = walk("peer", ph);          /* the next sibling, when we come back */
        u32 idx = add_node(ph, parent);
        if (sp == 64) fail("the device tree is too deep");
        stack_ph[sp] = walk("child", ph); stack_idx[sp] = idx; sp++;
    }
    put32(DTSNAP_MAGIC_OFF, DTSNAP_MAGIC);
    put32(DTSNAP_BYTES_OFF, used);
    put32(DTSNAP_COUNT_OFF, nnodes);
    put32(DTSNAP_RAM_OFF, ram);
    put32(DTSNAP_OEMDISK_OFF, oemdisk_blocks);
}

/* --- leaving the firmware ------------------------------------------------------------------ */

static u32 bswap(u32 v) { return v << 24 | (v & 0xFF00u) << 8 | (v >> 8 & 0xFF00u) | v >> 24; }

/* src -> dst, each doubleword's bytes reversed (in place when src == dst) */
static void reverse_copy(u32 dst, const void *src, u32 len)
{
    const u32 *s = src;
    u32 *d = (u32 *)dst;
    for (u32 i = 0; i < (len + 7) / 8; i++) {
        u32 a = s[2 * i], b = s[2 * i + 1];
        d[2 * i] = bswap(b);
        d[2 * i + 1] = bswap(a);
    }
}

void stage0_main(void) __attribute__((noreturn));
void stage0_main(void)
{
    u32 chosen = finddevice("/chosen");
    if (chosen != DTSNAP_NONE) getprop(chosen, "stdout", &of_stdout, 4);
    puts_("\r\nstage0: powermac-nt-hal, Open Firmware in its factory big-endian mode\r\n");

    /* RAM: /memory's reg, every (base, size) pair summed */
    u32 reg[32], ram = 0;
    int rl = getprop(finddevice("/memory"), "reg", reg, sizeof reg);
    for (int i = 0; i + 1 < rl / 4; i += 2) ram += reg[i + 1];
    if (ram < (32u << 20)) fail("cannot size memory, or less than 32 MB");
    puts_("stage0: RAM "); puthex(ram); puts_("\r\n");

    /* the Bandit bridges: the root's children named "bandit" */
    u32 bandits[4], nb = 0;
    for (u32 ph = walk("child", finddevice("/")); ph && ph != DTSNAP_NONE && nb < 4; ph = walk("peer", ph)) {
        char name[32];
        int n = getprop(ph, "name", name, sizeof name - 1);
        if (n <= 0) continue;
        name[n] = 0;
        u32 r[2];
        if (streq(name, "bandit") && getprop(ph, "reg", r, sizeof r) >= 4) bandits[nb++] = r[0];
    }
    if (nb == 0 || bandits[0] != 0xF2000000u) fail("no Bandit at $F2000000: not a machine this firmware knows");
    for (u32 i = 0; i < nb; i++) { puts_("stage0: Bandit at "); puthex(bandits[i]); puts_("\r\n"); }

    /* the OEM disk boot.of read, if it did: its size and the blocks read-blocks returned */
    u32 oemblocks = 0, oembytes = 0;
    if (chosen == DTSNAP_NONE || getprop(chosen, "arcfw,oemdisk-blocks", &oemblocks, 4) != 4
        || getprop(chosen, "arcfw,oemdisk-bytes", &oembytes, 4) != 4 || oembytes > OEMDISK_MAX_BYTES)
        oemblocks = oembytes = 0;

    u32 len = (u32)(stage0_payload_end - stage0_payload);
    if (ARCFW_BASE + len > STAGE0_BASE) fail("the ARC firmware is too big for its place");

    snapshot(ram, oemblocks);
    puts_("stage0: device tree taken, "); puthex(nnodes); puts_(" nodes, "); puthex(used); puts_(" bytes\r\n");
    puts_("stage0: switching to little-endian mode (until the next reset)\r\n");

    stage0_take();          /* no more Open Firmware: nothing below calls it */

    reverse_copy(ARCFW_BASE, stage0_payload, len);
    stage0_flush(ARCFW_BASE, len);
    reverse_copy(DTSNAP_PHYS, snap, used);
    stage0_flush(DTSNAP_PHYS, used);
    if (oemblocks) {
        /* the header as NT reads it, then header page and image laid out little-endian */
        u32 *h = (u32 *)OEMDISK_PHYS;
        u32 bytes = oembytes;
        h[0] = bswap(OEMDISK_MAGIC); h[1] = bswap(OEMDISK_VERSION);
        h[2] = bswap(OEMDISK_IMAGE_PHYS); h[3] = bswap(bytes);
        h[4] = bswap(OEMDISK_BLOCK); h[5] = bswap(oemblocks); h[6] = 0; h[7] = 0;
        reverse_copy(OEMDISK_PHYS, (const void *)OEMDISK_PHYS, OEMDISK_HEADER_SIZE + bytes);
        stage0_flush(OEMDISK_PHYS, OEMDISK_HEADER_SIZE + bytes);
    }
    stage0_enter(ARCFW_BASE, DTSNAP_PHYS, ram, bandits, nb);
}
