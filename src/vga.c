/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * UPSTREAM: the NT OEM-font layout (the glyph Map[] and its column-major bytes) was
 *   rewritten from reading Wack0/entii-for-workcubes, halartx/source/display.c
 *   (GPL-2.0, commit c9b041da).  Everything else is original work from the Cirrus
 *   GD543x manual.  No lines copied.  See PROVENANCE.md.
 *
 * vga.c — a text console on the Cirrus 54M30 (the Network Server's on-board video), so
 * text-mode Setup's screens and HAL bugchecks appear on the monitor as well as ttya.
 *
 * The firmware left the console on the serial line and never programmed the chip, so the HAL
 * does a from-scratch mode-set through the legacy VGA registers into 640x480 8bpp linear, sets
 * a two-entry palette (blue paper, white ink), and blits glyphs from the OEM font SETUPLDR
 * loaded (LOADER_PARAMETER_BLOCK.OemFontFile).  Register set derived from the Cirrus GD543x
 * manual; only the registers the display scans from need be correct.  The glyph layout (Map[]
 * offsets, column-major row bytes) follows the NT OEM font format. */
#include "hal.h"

enum { FG = 1, BG = 0 };   /* 8bpp palette indices: ink and paper */

BOOLEAN HalpFbActive = FALSE;
static volatile UCHAR *HalpFb;      /* mapped framebuffer */
ULONG HalpFbPhys;                   /* BAR0: the 1 MB linear VRAM, for the legacy-window alias in pci.c */
static ULONG HalpFbStride, HalpFbWpx, HalpFbHpx;
static POEM_FONT_FILE_HEADER HalpFont;
static ULONG HalpCharW, HalpCharH, HalpBytesPerRow;
ULONG HalpFbCols, HalpFbRows, HalpFbCol, HalpFbRow;

static VOID Seq(UCHAR i, UCHAR v)  { MmioWrite8(VGA_PORT(0x3C4), i); MmioWrite8(VGA_PORT(0x3C5), v); }
static UCHAR SeqRead(UCHAR i)      { MmioWrite8(VGA_PORT(0x3C4), i); return MmioRead8(VGA_PORT(0x3C5)); }
static VOID Crtc(UCHAR i, UCHAR v) { MmioWrite8(VGA_PORT(0x3D4), i); MmioWrite8(VGA_PORT(0x3D5), v); }
static VOID Gr(UCHAR i, UCHAR v)   { MmioWrite8(VGA_PORT(0x3CE), i); MmioWrite8(VGA_PORT(0x3CF), v); }

static VOID HalpDacEntry(UCHAR index, UCHAR r6, UCHAR g6, UCHAR b6)
{
    MmioWrite8(VGA_PORT(0x3C8), index);
    MmioWrite8(VGA_PORT(0x3C9), r6);
    MmioWrite8(VGA_PORT(0x3C9), g6);
    MmioWrite8(VGA_PORT(0x3C9), b6);
}

/* 640x480, 8 bits per pixel, linear framebuffer at VRAM offset 0. */
static VOID HalpVgaMode640x480x8(VOID)
{
    /* Miscellaneous Output: bit 0 puts the CRTC pair at the colour addresses ($3D4/$3D5) that
     * the rest of this file uses, and that a video driver taking the chip over will look for. */
    MmioWrite8(VGA_PORT(0x3C2), 0xE3);
    Seq(0x06, 0x12);   /* unlock the Cirrus extension registers */
    /* SR17[6] chooses where the memory-mapped BLT registers live once linear addressing is on:
     * '0' = the 256 bytes at B800:0, '1' = the last 256 bytes of linear space (TRM 9.13).  A
     * PC's video BIOS leaves it clear, and cirrus.sys assumes as much: it ORs bit 2 into
     * whatever it reads back and then writes its registers at B8000.  Open Firmware's own
     * Cirrus driver, which has run before us whenever the console is on the screen, leaves
     * SR17 = $62 -- bit 6 SET.  The driver then gets $66, enables linear addressing (SR07 =
     * $F1) and its register block moves out from under it: every BLT register write lands in
     * display memory as pixels, START reads back as $0, no BLT ever runs, and GUI-mode Setup
     * comes up as the Windows NT Setup backdrop with one grey button (17 September, browser
     * only: headless runs had the OF console on ttya, so OF never touched the card).  Clear
     * only bit 6; the rest of the register is OF's business. */
    Seq(0x17, (UCHAR)(SeqRead(0x17) & ~0x40u));
    Seq(0x01, 0x01);   /* 8 dots per character clock */
    Seq(0x07, 0x01);   /* extended packed-pixel mode, 8 bpp */
    Seq(0x04, 0x0E);   /* chain-4, no odd/even: a flat byte-per-pixel map */
    Gr(0x05, 0x40);    /* 256-colour shift mode */
    Gr(0x06, 0x01);    /* graphics mode, memory map at A0000 (unused: we use the linear BAR) */
    Crtc(0x11, 0x00);  /* clear write-protect on CR0-7 */
    Crtc(0x01, 0x4F);  /* horizontal display end: (79+1)*8 = 640 */
    Crtc(0x07, 0x02);  /* overflow: vertical display end bit 8 (479 = 0x1DF) */
    Crtc(0x12, 0xDF);  /* vertical display end low byte (479 & 0xFF) */
    Crtc(0x13, 0x50);  /* offset: 80 units * 8 = 640-byte stride */
    Crtc(0x0C, 0x00);  /* start address high */
    Crtc(0x0D, 0x00);  /* start address low */
    Crtc(0x1B, 0x00);  /* Cirrus start-address extension / scanline control */
    HalpFbStride = 640; HalpFbWpx = 640; HalpFbHpx = 480;
}

static VOID HalpFbClear(VOID)
{
    for (ULONG i = 0; i < HalpFbStride * HalpFbHpx; i++) HalpFb[i] = BG;
}

BOOLEAN HalpFbInit(PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    /* Confirm the 54M30 is where we expect and turn on its decoders. */
    UCHAR cfg[64];
    if (HalGetBusData(PCIConfiguration, C54M30_BUS, C54M30_DEV, cfg, sizeof cfg) < 64) { HalpPrint("HAL: 54M30 config read short\n"); return FALSE; }
    if (*(PUSHORT)&cfg[0] != C54M30_VENDOR || *(PUSHORT)&cfg[2] != C54M30_DEVID) {
        HalpPrint("HAL: 54M30 not found at bus %d dev %d (id %x)\n", C54M30_BUS, C54M30_DEV, *(PULONG)&cfg[0]);
        return FALSE;
    }
    /* enable I/O + memory decode (command register, offset 4) */
    { UCHAR c4[2] = { 0x03, 0x00 }; HalSetBusDataByOffset(PCIConfiguration, C54M30_BUS, C54M30_DEV, c4, 4, 2); }

    ULONG bar0 = *(PULONG)&cfg[0x10] & ~0xFu;   /* framebuffer aperture (PCI mem, OF-assigned 0x81000000) */
    ULONG bar1 = *(PULONG)&cfg[0x14];           /* I/O BAR (informational) */
    HalpPrint("HAL: 54M30 bar0 %x bar1 %x cmd %X\n", bar0, bar1, *(PUSHORT)&cfg[4]);
    if (bar0 == 0) { HalpPrint("HAL: 54M30 framebuffer BAR unassigned\n"); return FALSE; }
    HalpFbPhys = bar0;
    HalpFb = (volatile UCHAR *)MmMapIoSpace((PHYSICAL_ADDRESS)bar0, 0x100000, FALSE);   /* 1 MB VRAM, uncached */
    if (!HalpFb) { HalpPrint("HAL: 54M30 framebuffer map failed (bar %x)\n", bar0); return FALSE; }

    HalpFont = (POEM_FONT_FILE_HEADER)LoaderBlock->OemFontFile;
    if (!HalpFont) { HalpPrint("HAL: no OEM font; framebuffer console disabled\n"); return FALSE; }
    HalpCharW = HalpFont->PixelWidth; HalpCharH = HalpFont->PixelHeight;
    HalpBytesPerRow = (HalpCharW + 7) / 8;
    HalpPrint("HAL: OEM font %x  %dx%d  chars %x..%x\n", (ULONG)HalpFont, HalpCharW, HalpCharH,
              HalpFont->FirstCharacter, HalpFont->LastCharacter);
    if (HalpCharW == 0 || HalpCharH == 0) { HalpPrint("HAL: font geometry zero\n"); return FALSE; }

    HalpVgaMode640x480x8();
    HalpDacEntry(BG, 0x00, 0x00, 0x2A);   /* paper: blue */
    HalpDacEntry(FG, 0x3F, 0x3F, 0x3F);   /* ink: white */
    HalpFbClear();
    HalpFbCols = HalpFbWpx / HalpCharW; HalpFbRows = HalpFbHpx / HalpCharH;
    HalpFbCol = 0; HalpFbRow = 0;
    HalpFbActive = TRUE;
    HalpPrint("HAL: 54M30 console %dx%d, font %dx%d, %dx%d chars, fb %x\n",
              HalpFbWpx, HalpFbHpx, HalpCharW, HalpCharH, HalpFbCols, HalpFbRows, bar0);
    return TRUE;
}

static VOID HalpFbScroll(VOID)
{
    ULONG line = HalpCharH * HalpFbStride;
    ULONG keep = (HalpFbRows - 1) * line;
    for (ULONG i = 0; i < keep; i++) HalpFb[i] = HalpFb[i + line];
    for (ULONG i = 0; i < line; i++) HalpFb[keep + i] = BG;
}

static VOID HalpFbGlyph(UCHAR ch)
{
    if (ch < HalpFont->FirstCharacter || ch > HalpFont->LastCharacter) ch = HalpFont->DefaultCharacter;
    const UCHAR *glyph = (const UCHAR *)HalpFont + HalpFont->Map[ch - HalpFont->FirstCharacter].Offset;
    volatile UCHAR *base = HalpFb + HalpFbRow * HalpCharH * HalpFbStride + HalpFbCol * HalpCharW;
    for (ULONG y = 0; y < HalpCharH; y++) {
        ULONG bits = 0;
        for (ULONG b = 0; b < HalpBytesPerRow; b++) bits |= (ULONG)glyph[b * HalpCharH + y] << (24 - b * 8);
        volatile UCHAR *row = base + y * HalpFbStride;
        for (ULONG x = 0; x < HalpCharW; x++) { row[x] = (bits & 0x80000000u) ? FG : BG; bits <<= 1; }
    }
}

VOID HalpFbPutChar(UCHAR ch)
{
    if (!HalpFbActive) return;
    if (ch == '\r') { HalpFbCol = 0; return; }
    if (ch == '\n') {
        HalpFbCol = 0;
        if (HalpFbRow + 1 < HalpFbRows) HalpFbRow++;
        else HalpFbScroll();
        return;
    }
    if (HalpFbCol >= HalpFbCols) HalpFbPutChar('\n');
    HalpFbGlyph(ch);
    HalpFbCol++;
}

/* The physical address of the 54M30's linear VRAM, read from its BAR0 if HalpFbInit has not run
 * (a driver can ask HalTranslateBusAddress before phase 1 gets to the console).  0 if no card. */
ULONG HalpVgaVramPhys(VOID)
{
    if (HalpFbPhys) return HalpFbPhys;
    UCHAR cfg[0x14];
    if (HalGetBusData(PCIConfiguration, C54M30_BUS, C54M30_DEV, cfg, sizeof cfg) < 0x14) return 0;
    if (*(PUSHORT)&cfg[0] != C54M30_VENDOR || *(PUSHORT)&cfg[2] != C54M30_DEVID) return 0;
    HalpFbPhys = *(PULONG)&cfg[0x10] & ~0xFu;
    return HalpFbPhys;
}
