/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 * heap.c -- a bump allocator for the firmware's own tables (configuration tree, strings, file
 * table, set environment values).  It is part of the image's bss, which crt0 clears and the
 * memory descriptors type FirmwareTemporary with the rest of the image: NT's loader copies
 * everything it wants out of the tree before the kernel runs, and the kernel reclaims the type.
 * Nothing is ever freed: the firmware builds its tables once. */
#include "arcfw.h"

#define HEAP_SIZE (128 * 1024)
static UCHAR heap[HEAP_SIZE] __attribute__((aligned(4096)));
static ULONG heap_used;

PVOID zalloc_aligned(ULONG n, ULONG align)
{
    ULONG at = (heap_used + align - 1) & ~(align - 1);
    if (at + n > HEAP_SIZE) fatal("firmware heap exhausted (%u + %u)", at, n);
    heap_used = at + n;
    memset(heap + at, 0, n);
    return heap + at;
}
PVOID zalloc(ULONG n) { return zalloc_aligned(n, 8); }
CHAR *strdup_(const CHAR *s) { CHAR *d = zalloc(strlen(s) + 1); strcpy(d, s); return d; }
