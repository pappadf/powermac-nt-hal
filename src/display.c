/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * UPSTREAM: the OEM-font glyph layout this shares with vga.c was rewritten from reading
 *   Wack0/entii-for-workcubes, halartx/source/display.c (GPL-2.0, commit c9b041da).
 *   The ESCC code is original.  No lines copied.  See PROVENANCE.md.
 *
 * display.c — HalDisplayString on the Network Server's ttya (ESCC channel A).  The firmware left
 * the channel configured (it was its own console); the HAL only polls RR0[Tx empty] and writes
 * the data register.  A small formatter backs the HAL's own diagnostics. */
#include "hal.h"
#include <stdarg.h>

static BOOLEAN HalpDisplayOwned = TRUE;
static ULONG HalpColumn;

VOID HalpPutChar(UCHAR c)
{
    ULONG spin = 0;
    if (c == '\n') { HalpPutChar('\r'); HalpColumn = 0; }
    /* reading the control register with the pointer at 0 returns RR0 */
    while (!(MmioRead8(ESCC_A_CTRL) & SCC_RR0_TX_EMPTY)) {
        if (++spin > 2000000) break;       /* never hang the console */
    }
    MmioWrite8(ESCC_A_DATA, c);
    if (c >= ' ') HalpColumn++;
}

static VOID HalpPutString(const char *s)
{
    while (*s) HalpPutChar((UCHAR)*s++);
}

static VOID HalpPutHex(ULONG v, int width)
{
    char buf[9]; int i;
    for (i = 7; i >= 0; i--) { buf[i] = "0123456789abcdef"[v & 15]; v >>= 4; }
    buf[8] = 0;
    HalpPutString(buf + (8 - (width ? width : 8)));
}

static VOID HalpPutDec(ULONG v)
{
    char buf[11]; int i = 10; buf[10] = 0;
    do { buf[--i] = '0' + v % 10; v /= 10; } while (v);
    HalpPutString(buf + i);
}

/* %s %c %x (8 digits) %X (4 digits) %d %% */
VOID HalpPrint(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') { HalpPutChar((UCHAR)*fmt); continue; }
        fmt++;
        switch (*fmt) {
        case 's': { const char *s = va_arg(ap, const char *); HalpPutString(s ? s : "(null)"); break; }
        case 'c': HalpPutChar((UCHAR)va_arg(ap, int)); break;
        case 'x': HalpPutHex(va_arg(ap, ULONG), 8); break;
        case 'X': HalpPutHex(va_arg(ap, ULONG), 4); break;
        case 'd': HalpPutDec(va_arg(ap, ULONG)); break;
        case '%': HalpPutChar('%'); break;
        default: HalpPutChar('%'); HalpPutChar((UCHAR)*fmt); break;
        }
    }
    va_end(ap);
}

BOOLEAN HalpInitializeDisplay(PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    UNREFERENCED_PARAMETER(LoaderBlock);
    HalpDisplayOwned = TRUE;
    return TRUE;
}

VOID HalDisplayString(PCHAR String)
{
    /* ttya always mirrors; the Cirrus console shows it when active. */
    for (PCHAR p = String; *p; p++) {
        HalpPutChar((UCHAR)*p);
        if (HalpFbActive) HalpFbPutChar((UCHAR)*p);
    }
}

VOID HalAcquireDisplayOwnership(PVOID ResetDisplayParameters)
{
    UNREFERENCED_PARAMETER(ResetDisplayParameters);
    /* The console is a serial line: keep printing after a display driver takes the video. */
    HalpDisplayOwned = TRUE;
}

VOID HalQueryDisplayParameters(PULONG WidthInCharacters, PULONG HeightInLines, PULONG CursorColumn, PULONG CursorRow)
{
    if (HalpFbActive) { *WidthInCharacters = HalpFbCols; *HeightInLines = HalpFbRows; *CursorColumn = HalpFbCol; *CursorRow = HalpFbRow; return; }
    *WidthInCharacters = 80; *HeightInLines = 24; *CursorColumn = HalpColumn; *CursorRow = 0;
}

VOID HalSetDisplayParameters(ULONG CursorColumn, ULONG CursorRow)
{
    if (HalpFbActive) {
        if (CursorColumn <= HalpFbCols) HalpFbCol = CursorColumn;
        if (CursorRow <= HalpFbRows) HalpFbRow = CursorRow;
        return;
    }
    HalpColumn = CursorColumn;
}
