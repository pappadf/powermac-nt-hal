/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * Original work of this project.  The register values are those powermac-os2's shim/control.c
 * (the same author's) programs, which follow Linux's controlfb.c (Paul Mackerras's driver for
 * these machines, GPL) for the 13-inch monitor's 640x480 raster.  No lines copied.
 *
 * control.c -- the text console on a Power Macintosh 7500/8500's own display, Control
 * (343S1154), when no 54M30 card is fitted: so text-mode Setup's boot messages and a bugcheck
 * appear on the monitor as well as on ttya.
 *
 * Control is device 11 on the Chaos bridge, a PCI bus of its own beside Bandit.  Open Firmware
 * assigns its two BARs only when it opens the display, so they are set here when it has not:
 * the 4 KB register block at 0x94000000 and the 64 MB VRAM aperture at 0x90000000, of which the
 * usable view is the upper half from +0x800000.  The pixel clock is a synthesiser behind Cuda's
 * I2C (device 0x50, three registers); the depth and the VRAM bank are RaDACal registers in
 * Grand Central at +0x1B000.
 *
 * Byte order.  Chaos, unlike Bandit, passes the CPU's byte lanes through unchanged, so in the
 * CPU's little-endian mode a store to address A reaches the bus at A ^ 7 for a byte and A ^ 4
 * for a word: a little-endian register at p is written with stwbrx at p ^ 4, and a VRAM byte at
 * p with a byte store at p ^ 7 (the console's swizzle, vga.c).  RaDACal is in Grand Central,
 * behind Bandit: plain byte stores, as for every other Grand Central register.
 *
 * Only the HAL draws here directly.  NT's display driver for Control (drivers/control) draws
 * into a shadow in RAM and copies it across with the bytes put right; it relies on the mode set
 * here. */
#include "hal.h"

#define CHAOS_CFG_ADDR 0xF0800000u
#define CHAOS_CFG_DATA 0xF0C00000u
#define CONTROL_DEV    11
#define CONTROL_ID     0x0003106Bu      /* what Chaos reports at Control's IDSEL */
#define VCI_WINDOW     0x90000000u      /* Open Firmware's choices, used when it made none */
#define VCI_REGS       0x94000000u
#define RADACAL        0xF301B000u

static VOID LeWr(ULONG va, ULONG v) { __asm__ volatile("stwbrx %0,0,%1; eieio" ::"r"(v), "r"(va ^ 4) : "memory"); }
static ULONG LeRd(ULONG va) { ULONG v; __asm__ volatile("lwbrx %0,0,%1; eieio" : "=r"(v) : "r"(va ^ 4) : "memory"); return v; }

static ULONG ChaosRead(ULONG reg)
{
    LeWr((ULONG)IOV(CHAOS_CFG_ADDR), (1u << CONTROL_DEV) | reg);
    return LeRd((ULONG)IOV(CHAOS_CFG_DATA));
}
static VOID ChaosWrite(ULONG reg, ULONG v)
{
    LeWr((ULONG)IOV(CHAOS_CFG_ADDR), (1u << CONTROL_DEV) | reg);
    LeWr((ULONG)IOV(CHAOS_CFG_DATA), v);
}

static ULONG HalpCtlRegs;               /* virtual address of the register block */
ULONG HalpControlFb;                    /* physical address of pixel 0, once the mode is set */
#define CTL(r) (HalpCtlRegs + 0x10u * (r))
enum { C_PIPED = 8, C_CTRL = 18, C_START = 19, C_PITCH = 20, C_VRAM_ATTR = 22, C_MODE = 23, C_RFRCNT = 24, C_INTR_ENA = 25 };

static VOID RaDacal(UCHAR reg, UCHAR v) { MmioWrite8(RADACAL + 0x00, reg); MmioWrite8(RADACAL + 0x20, v); }
static VOID RaDacalColour(UCHAR index, UCHAR r, UCHAR g, UCHAR b)
{
    MmioWrite8(RADACAL + 0x00, index);
    MmioWrite8(RADACAL + 0x30, r); MmioWrite8(RADACAL + 0x30, g); MmioWrite8(RADACAL + 0x30, b);
}

static VOID VramWr8(volatile UCHAR *va, UCHAR v) { *(volatile UCHAR *)((ULONG)va ^ 7) = v; __asm__ volatile("eieio" ::: "memory"); }
static UCHAR VramRd8(volatile UCHAR *va) { UCHAR v = *(volatile UCHAR *)((ULONG)va ^ 7); __asm__ volatile("eieio" ::: "memory"); return v; }

BOOLEAN HalpControlInit(PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    ULONG id = ChaosRead(0x00);
    if (id != CONTROL_ID) { HalpPrint("HAL: no Control on Chaos (id %x): the console stays on ttya\n", id); return FALSE; }
    ULONG vram = ChaosRead(0x18) & ~0xFu, regs = ChaosRead(0x14) & ~0xFu;
    if (vram == 0 || vram == 0xFFFFFFF0u) { ChaosWrite(0x18, VCI_WINDOW); vram = VCI_WINDOW; }
    if (regs == 0 || regs == 0xFFFFFFF0u) { ChaosWrite(0x14, VCI_REGS); regs = VCI_REGS; }
    ChaosWrite(0x04, ChaosRead(0x04) | 0x0002u);   /* memory decoding */
    HalpCtlRegs = (ULONG)MmMapIoSpace((PHYSICAL_ADDRESS)regs, 0x1000, FALSE);
    /* the two 2 MB banks' views, +0x800000 (bank 1) and +0xE00000 (bank 2 alone) */
    volatile UCHAR *view = (volatile UCHAR *)MmMapIoSpace((PHYSICAL_ADDRESS)(vram + 0x800000u), 0x800000, FALSE);
    if (!HalpCtlRegs || !view) { HalpPrint("HAL: Control: cannot map registers %x / VRAM %x\n", regs, vram); return FALSE; }

    LeWr(CTL(C_CTRL), 0x400 | 0x3B);                /* display off */
    static const UCHAR clk[3] = { 14, 27, 2 };      /* 30.13 MHz: the 640x480 dot clock */
    for (ULONG i = 0; i < 3; i++)
        if (!HalpCudaIicWrite(0x50, (UCHAR)(i + 1), clk[i])) HalpPrint("HAL: Control: Cuda did not take the pixel clock\n");

    /* which banks are fitted (controlfb.c's probe) */
    LeWr(CTL(C_VRAM_ATTR), 0x31);
    VramWr8(view + 0x600000u, 0xB3); VramWr8(view + 0x600001u, 0x71);
    BOOLEAN bank2 = VramRd8(view + 0x600000u) == 0xB3 && VramRd8(view + 0x600001u) == 0x71;
    LeWr(CTL(C_VRAM_ATTR), 0x39);
    VramWr8(view, 0x5A); VramWr8(view + 1, 0xC7);
    BOOLEAN bank1 = VramRd8(view) == 0x5A && VramRd8(view + 1) == 0xC7;
    BOOLEAN big = bank1 && bank2;
    ULONG attr = big ? 0x51 : bank1 ? 0x31 : 0x39;
    ULONG fboff = bank1 ? 0 : 0x600000u;

    RaDacal(0x20, big ? 0x20 : 0x10);
    RaDacal(0x21, bank1 ? 1 : 0);
    RaDacal(0x10, 0);
    RaDacal(0x11, 0);
    /* vswin..hserr: the ROM's 640x480 raster; the pipe delay for 8 bits a pixel */
    static const USHORT t[16] = { 1045, 1042, 82, 43, 4, 1048, 1050, 0, 430, 393, 73, 31, 431, 16, 216, 400 };
    for (ULONG i = 0; i < 16; i++) LeWr(CTL(1 + i), i + 1 == C_PIPED ? 73u - (big ? 13u : 9u) : t[i]);
    LeWr(CTL(C_PITCH), 640);
    LeWr(CTL(C_MODE), big ? 3 : 2);
    LeWr(CTL(C_VRAM_ATTR), attr);
    LeWr(CTL(C_START), 0);
    LeWr(CTL(C_RFRCNT), 0x1E5);
    LeWr(CTL(C_INTR_ENA), 0);
    RaDacalColour(0, 0x00, 0x00, 0xAA);              /* paper: blue */
    RaDacalColour(1, 0xFF, 0xFF, 0xFF);              /* ink: white */

    /* pixel 0 sits 16 bytes into the view (controlfb.c's CTRLFB_OFF) */
    if (!HalpFbStart(LoaderBlock, view + fboff, 16, 7, 640, 640, 480)) return FALSE;
    LeWr(CTL(C_CTRL), 0x3B);                        /* display on */
    HalpControlFb = vram + 0x800000u + fboff + 16;
    HalpPrint("HAL: Control console 640x480, %s VRAM, pixel 0 at %x\n", big ? "4 MB" : "2 MB", HalpControlFb);
    return TRUE;
}
