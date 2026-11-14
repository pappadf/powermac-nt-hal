/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * Original work of this project.  The video port driver's structures below are DDK-documented
 * interface facts, re-declared here in our own words; VIDEO_HW_INITIALIZATION_DATA's and
 * VIDEO_PORT_CONFIG_INFO's offsets were read from what the shipped VGA.SYS (NT 4.0 build 1381,
 * PowerPC) stores into them, and the IOCTL codes are the ones the shipped framebuf.dll sends.
 * Nothing derives from leaked Windows NT source.  See PROVENANCE.md.
 *
 * pmctrl.sys -- a video miniport for a Power Macintosh 7500/8500's own display, Control
 * (343S1154), for NT's generic frame buffer display driver, framebuf.dll, and for text-mode
 * Setup's frame buffer console.
 *
 * The HAL sets the mode, 640x480 in 8-bit colour (src/control.c): the pixel clock is behind
 * Cuda, which only the HAL may talk to.  This driver offers that one mode.
 *
 * Why a shadow.  Control sits behind the Chaos bridge, which does not reverse byte lanes as
 * Bandit does; with the CPU little-endian, a byte stored at A reaches VRAM at A ^ 7, so an image
 * drawn straight into VRAM shows with every 8 pixels mirrored.  Nothing in NT can be told to
 * draw that way.  So framebuf.dll and Setup are given a "frame buffer" in RAM -- physically
 * contiguous, at its system-space address -- and every 50 ms a timer
 * DPC compares it, 8 bytes at a time, with what it last sent, and writes the groups that changed
 * to VRAM with the order put right.  A palette write goes straight to the RaDACal. */
#include "nt.h"

/* ---- the video port driver's interface ------------------------------------------------------ */

typedef struct _VIDEO_HW_INITIALIZATION_DATA {   /* VGA.SYS: size 0x28, FindAdapter at 8 */
    ULONG HwInitDataSize;                /* 0x00 */
    INTERFACE_TYPE AdapterInterfaceType; /* 0x04 */
    PVOID HwFindAdapter;                 /* 0x08  descriptors */
    PVOID HwInitialize;                  /* 0x0C */
    PVOID HwInterrupt;                   /* 0x10 */
    PVOID HwStartIO;                     /* 0x14 */
    ULONG HwDeviceExtensionSize;         /* 0x18 */
    ULONG StartingDeviceNumber;          /* 0x1C */
    PVOID HwResetHw;                     /* 0x20 */
    PVOID HwTimer;                       /* 0x24 */
} VIDEO_HW_INITIALIZATION_DATA;
_Static_assert(sizeof(VIDEO_HW_INITIALIZATION_DATA) == 0x28, "VIDEO_HW_INITIALIZATION_DATA is 0x28 bytes");

typedef struct _VIDEO_PORT_CONFIG_INFO {        /* VGA.SYS's FindAdapter: 0x18..0x34 */
    ULONG Length;                        /* 0x00 */
    ULONG SystemIoBusNumber;             /* 0x04 */
    INTERFACE_TYPE AdapterInterfaceType; /* 0x08 */
    ULONG BusInterruptLevel;             /* 0x0C */
    ULONG BusInterruptVector;            /* 0x10 */
    KINTERRUPT_MODE InterruptMode;       /* 0x14 */
    ULONG NumEmulatorAccessEntries;      /* 0x18 */
    PVOID EmulatorAccessEntries;         /* 0x1C */
    ULONG EmulatorAccessEntriesContext;  /* 0x20 */
    ULONG Pad;                           /* 0x24  alignment for the next */
    PHYSICAL_ADDRESS VdmPhysicalVideoMemoryAddress; /* 0x28 */
    ULONG VdmPhysicalVideoMemoryLength;  /* 0x30 */
    ULONG HardwareStateSize;             /* 0x34 */
} VIDEO_PORT_CONFIG_INFO, *PVIDEO_PORT_CONFIG_INFO;
_Static_assert(__builtin_offsetof(VIDEO_PORT_CONFIG_INFO, VdmPhysicalVideoMemoryAddress) == 0x28, "VDM address at 0x28");
_Static_assert(__builtin_offsetof(VIDEO_PORT_CONFIG_INFO, HardwareStateSize) == 0x34, "HardwareStateSize at 0x34");

typedef struct _STATUS_BLOCK { ULONG Status; ULONG Information; } STATUS_BLOCK, *PSTATUS_BLOCK;
typedef struct _VIDEO_REQUEST_PACKET {
    ULONG IoControlCode;
    PSTATUS_BLOCK StatusBlock;
    PVOID InputBuffer;
    ULONG InputBufferLength;
    PVOID OutputBuffer;
    ULONG OutputBufferLength;
} VIDEO_REQUEST_PACKET, *PVIDEO_REQUEST_PACKET;

typedef struct _VIDEO_NUM_MODES { ULONG NumModes, ModeInformationLength; } VIDEO_NUM_MODES;
typedef struct _VIDEO_MODE_INFORMATION {
    ULONG Length, ModeIndex, VisScreenWidth, VisScreenHeight, ScreenStride, NumberOfPlanes, BitsPerPlane,
          Frequency, XMillimeter, YMillimeter, NumberRedBits, NumberGreenBits, NumberBlueBits,
          RedMask, GreenMask, BlueMask, AttributeFlags, VideoMemoryBitmapWidth, VideoMemoryBitmapHeight,
          DriverSpecificAttributeFlags;
} VIDEO_MODE_INFORMATION;
_Static_assert(sizeof(VIDEO_MODE_INFORMATION) == 0x50, "VIDEO_MODE_INFORMATION is 0x50 bytes");
typedef struct _VIDEO_MEMORY { PVOID RequestedVirtualAddress; } VIDEO_MEMORY;
typedef struct _VIDEO_MEMORY_INFORMATION { PVOID VideoRamBase; ULONG VideoRamLength; PVOID FrameBufferBase; ULONG FrameBufferLength; } VIDEO_MEMORY_INFORMATION;
typedef struct _VIDEO_CLUT { USHORT NumEntries, FirstEntry; struct { UCHAR Red, Green, Blue, Unused; } Entry[1]; } VIDEO_CLUT;

#define VIDEO_MODE_COLOR           0x01
#define VIDEO_MODE_GRAPHICS        0x02
#define VIDEO_MODE_PALETTE_DRIVEN  0x04
#define VIDEO_MODE_MANAGED_PALETTE 0x08

/* CTL_CODE(FILE_DEVICE_VIDEO, n, METHOD_BUFFERED, FILE_ANY_ACCESS) */
#define VIDEO_IOCTL(n) (0x00230000u | ((n) << 2))
#define IOCTL_VIDEO_QUERY_AVAIL_MODES     VIDEO_IOCTL(0x100)
#define IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES VIDEO_IOCTL(0x101)
#define IOCTL_VIDEO_QUERY_CURRENT_MODE    VIDEO_IOCTL(0x102)
#define IOCTL_VIDEO_SET_CURRENT_MODE      VIDEO_IOCTL(0x103)
#define IOCTL_VIDEO_RESET_DEVICE          VIDEO_IOCTL(0x104)
#define IOCTL_VIDEO_SET_COLOR_REGISTERS   VIDEO_IOCTL(0x107)
#define IOCTL_VIDEO_MAP_VIDEO_MEMORY      VIDEO_IOCTL(0x116)
#define IOCTL_VIDEO_UNMAP_VIDEO_MEMORY    VIDEO_IOCTL(0x117)

#define NO_ERROR                 0
#define ERROR_INVALID_FUNCTION   1
#define ERROR_DEV_NOT_EXIST      55
#define ERROR_INVALID_PARAMETER  87
#define ERROR_INSUFFICIENT_BUFFER 122

ULONG VideoPortInitialize(PVOID Argument1, PVOID Argument2, VIDEO_HW_INITIALIZATION_DATA *Data, PVOID HwContext);
PVOID VideoPortGetDeviceBase(PVOID Ext, PHYSICAL_ADDRESS IoAddress, ULONG NumberOfUchars, UCHAR InIoSpace);
NTSTATUS RtlWriteRegistryValue(ULONG RelativeTo, const WCHAR *Path, const WCHAR *ValueName, ULONG ValueType, PVOID ValueData, ULONG ValueLength);
typedef struct _KTIMER { ULONG Opaque[16] __attribute__((aligned(8))); } KTIMER, *PKTIMER;  /* 40 bytes on NT 4.0 */
VOID KeInitializeTimer(PKTIMER Timer);
BOOLEAN KeSetTimerEx(PKTIMER Timer, LARGE_INTEGER DueTime, LONG Period, PKDPC Dpc);
VOID HalDisplayString(PCHAR String);

/* ---- the machine ----------------------------------------------------------------------------- */

#define CHAOS_CFG_ADDR 0xF0800000u
#define CHAOS_CFG_DATA 0xF0C00000u
#define CONTROL_DEV    11
#define CONTROL_ID     0x0003106Bu
#define RADACAL        0xF301B000u
#define C_VRAM_ATTR    22

#define WIDTH   640
#define HEIGHT  480
#define FB_SIZE (WIDTH * HEIGHT)
#define SHADOW_SIZE ((FB_SIZE + 0xFFFu) & ~0xFFFu)

typedef struct _CTL_EXTENSION {
    ULONG CfgAddr, CfgData;            /* virtual addresses of Chaos's configuration ports */
    volatile UCHAR *Dac;               /* RaDACal */
    volatile UCHAR *Vram;              /* pixel 0 */
    ULONG *Shadow;                     /* the frame buffer NT draws in: cached, in system space */
    PHYSICAL_ADDRESS ShadowPhys;
    ULONG *Sent;                       /* what VRAM holds, as of the last pass */
    BOOLEAN Running;
    KDPC Dpc;
    KTIMER Timer;
} CTL_EXTENSION, *PCTL_EXTENSION;

static VOID Log(const char *s) { HalDisplayString((PCHAR)s); }
static VOID LogHex(const char *s, ULONG v)
{
    char b[96]; ULONG n = 0;
    while (*s && n < 80) b[n++] = *s++;
    for (int sh = 28; sh >= 0; sh -= 4) b[n++] = "0123456789abcdef"[(v >> sh) & 15];
    b[n++] = '\n'; b[n] = 0;
    HalDisplayString(b);
}

/* Chaos passes the lanes through: a little-endian word at p is stwbrx/lwbrx at p ^ 4 */
static VOID LeWr(ULONG va, ULONG v) { __asm__ volatile("stwbrx %0,0,%1; eieio" ::"r"(v), "r"(va ^ 4) : "memory"); }
static ULONG LeRd(ULONG va) { ULONG v; __asm__ volatile("lwbrx %0,0,%1; eieio" : "=r"(v) : "r"(va ^ 4) : "memory"); return v; }
static ULONG ChaosRead(PCTL_EXTENSION x, ULONG reg) { LeWr(x->CfgAddr, (1u << CONTROL_DEV) | reg); return LeRd(x->CfgData); }

/* ---- the copy ------------------------------------------------------------------------------- */

/* Eight pixels b0..b7 to VRAM.  A word stored at A with the CPU little-endian lands at A ^ 4
 * with its most significant byte first, so pixels b0..b3 are the word (b0 << 24 | .. | b3) at
 * offset ^ 4, and b4..b7 the same at the offset itself. */
static VOID CtlPassDpc(PVOID Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    PCTL_EXTENSION x = Context;
    volatile ULONG *src = x->Shadow;
    ULONG *sent = x->Sent;
    ULONG vram = (ULONG)x->Vram;
    for (ULONG i = 0; i < FB_SIZE / 4; i += 8) {
        for (ULONG k = 0; k < 8; k += 2) {
            ULONG a = src[i + k], b = src[i + k + 1];
            if (a == sent[i + k] && b == sent[i + k + 1]) continue;
            sent[i + k] = a; sent[i + k + 1] = b;
            ULONG off = (i + k) * 4;
            *(volatile ULONG *)(vram + (off ^ 4)) = __builtin_bswap32(a);
            *(volatile ULONG *)(vram + off) = __builtin_bswap32(b);
        }
    }
    __asm__ volatile("sync" ::: "memory");
}
DEFINE_DESC(CtlPassDpc);

static VOID CtlStart(PCTL_EXTENSION x)
{
    /* everything differs from VRAM: the first pass paints the whole screen */
    for (ULONG i = 0; i < FB_SIZE / 4; i++) { x->Shadow[i] = 0; x->Sent[i] = 0xFFFFFFFFu; }
    if (!x->Running) {
        x->Running = TRUE;
        KeSetTimerEx(&x->Timer, -500000LL, 50, &x->Dpc);   /* 50 ms from now, then every 50 ms */
    }
}

/* ---- the miniport's routines ------------------------------------------------------------------ */

static ULONG CtlFindAdapter(PVOID Ext, PVOID Context, WCHAR *Args, PVIDEO_PORT_CONFIG_INFO Config, PUCHAR Again)
{
    PCTL_EXTENSION x = Ext;
    *Again = 0;
    if (Config->SystemIoBusNumber != 0) return ERROR_DEV_NOT_EXIST;
    x->CfgAddr = (ULONG)VideoPortGetDeviceBase(Ext, CHAOS_CFG_ADDR, 0x1000, 0);
    x->CfgData = (ULONG)VideoPortGetDeviceBase(Ext, CHAOS_CFG_DATA, 0x1000, 0);
    if (!x->CfgAddr || !x->CfgData) return ERROR_DEV_NOT_EXIST;
    ULONG id = ChaosRead(x, 0x00);
    if (id != CONTROL_ID) return ERROR_DEV_NOT_EXIST;
    ULONG vram = ChaosRead(x, 0x18) & ~0xFu, regs = ChaosRead(x, 0x14) & ~0xFu;
    if (!vram || vram == 0xFFFFFFF0u || !regs || regs == 0xFFFFFFF0u) {
        Log("pmctrl: Control has no addresses: the HAL did not set its mode\n");
        return ERROR_DEV_NOT_EXIST;
    }
    ULONG regva = (ULONG)VideoPortGetDeviceBase(Ext, regs, 0x1000, 0);
    if (!regva) return ERROR_DEV_NOT_EXIST;
    ULONG attr = LeRd(regva + 0x10u * C_VRAM_ATTR);
    ULONG pix0 = vram + 0x800000u + (attr == 0x39 ? 0x600000u : 0) + 16;   /* as the HAL laid it out */
    ULONG page = pix0 & ~0xFFFu;
    PUCHAR v = VideoPortGetDeviceBase(Ext, page, FB_SIZE + (pix0 - page), 0);
    x->Dac = VideoPortGetDeviceBase(Ext, RADACAL, 0x100, 0);
    if (!v || !x->Dac) return ERROR_DEV_NOT_EXIST;
    x->Vram = v + (pix0 - page);

    x->Shadow = MmAllocateContiguousMemory(SHADOW_SIZE, 0x0FFFFFFFull);
    x->Sent = ExAllocatePool(0 /* NonPagedPool */, FB_SIZE);
    if (!x->Shadow || !x->Sent) { Log("pmctrl: no memory for the shadow frame buffer\n"); return ERROR_DEV_NOT_EXIST; }
    MmGetPhysicalAddress(&x->ShadowPhys, x->Shadow);
    KeInitializeDpc(&x->Dpc, DESC(CtlPassDpc), x);
    KeInitializeTimer(&x->Timer);

    Config->NumEmulatorAccessEntries = 0;
    Config->EmulatorAccessEntries = NULL;
    Config->EmulatorAccessEntriesContext = 0;
    Config->VdmPhysicalVideoMemoryAddress = 0;
    Config->VdmPhysicalVideoMemoryLength = 0;
    Config->HardwareStateSize = 0;

    LogHex("pmctrl: Control 640x480x8 (powermac-nt-hal); shadow frame buffer at ", LOW32(x->ShadowPhys));
    return NO_ERROR;
}
DEFINE_DESC(CtlFindAdapter);

static BOOLEAN CtlInitialize(PVOID Ext) { return TRUE; }
DEFINE_DESC(CtlInitialize);

static VOID CtlModeInfo(VIDEO_MODE_INFORMATION *m)
{
    UCHAR *p = (UCHAR *)m;
    for (ULONG i = 0; i < sizeof *m; i++) p[i] = 0;
    m->Length = sizeof *m;
    m->ModeIndex = 0;
    m->VisScreenWidth = WIDTH; m->VisScreenHeight = HEIGHT; m->ScreenStride = WIDTH;
    m->NumberOfPlanes = 1; m->BitsPerPlane = 8;
    m->Frequency = 67;                     /* the 13-inch monitor's raster */
    m->XMillimeter = 320; m->YMillimeter = 240;
    m->NumberRedBits = m->NumberGreenBits = m->NumberBlueBits = 8;
    m->AttributeFlags = VIDEO_MODE_COLOR | VIDEO_MODE_GRAPHICS | VIDEO_MODE_PALETTE_DRIVEN | VIDEO_MODE_MANAGED_PALETTE;
    m->VideoMemoryBitmapWidth = WIDTH; m->VideoMemoryBitmapHeight = HEIGHT;
}

static BOOLEAN CtlStartIO(PVOID Ext, PVIDEO_REQUEST_PACKET r)
{
    PCTL_EXTENSION x = Ext;
    ULONG st = NO_ERROR, info = 0;
    switch (r->IoControlCode) {
    case IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES:
        if (r->OutputBufferLength < sizeof(VIDEO_NUM_MODES)) { st = ERROR_INSUFFICIENT_BUFFER; break; }
        ((VIDEO_NUM_MODES *)r->OutputBuffer)->NumModes = 1;
        ((VIDEO_NUM_MODES *)r->OutputBuffer)->ModeInformationLength = sizeof(VIDEO_MODE_INFORMATION);
        info = sizeof(VIDEO_NUM_MODES);
        break;
    case IOCTL_VIDEO_QUERY_AVAIL_MODES:
    case IOCTL_VIDEO_QUERY_CURRENT_MODE:
        if (r->OutputBufferLength < sizeof(VIDEO_MODE_INFORMATION)) { st = ERROR_INSUFFICIENT_BUFFER; break; }
        CtlModeInfo(r->OutputBuffer);
        info = sizeof(VIDEO_MODE_INFORMATION);
        break;
    case IOCTL_VIDEO_SET_CURRENT_MODE:
        if (r->InputBufferLength < 4 || (*(PULONG)r->InputBuffer & 0x7FFFFFFFu) != 0) { st = ERROR_INVALID_PARAMETER; break; }
        CtlStart(x);
        break;
    case IOCTL_VIDEO_RESET_DEVICE:
        break;
    case IOCTL_VIDEO_MAP_VIDEO_MEMORY: {
        /* The shadow is RAM, which the video port cannot map (it translates the address as one
         * on the adapter's bus, and RAM is not).  Both callers run in kernel mode -- text-mode
         * Setup and, on NT 4.0, framebuf.dll inside the kernel's GDI -- so they are given the
         * shadow's own system-space address, which is valid in every process. */
        if (r->InputBufferLength < sizeof(VIDEO_MEMORY) || r->OutputBufferLength < sizeof(VIDEO_MEMORY_INFORMATION)) { st = ERROR_INSUFFICIENT_BUFFER; break; }
        VIDEO_MEMORY_INFORMATION *mi = r->OutputBuffer;
        mi->VideoRamBase = x->Shadow; mi->VideoRamLength = SHADOW_SIZE;
        mi->FrameBufferBase = x->Shadow; mi->FrameBufferLength = FB_SIZE;
        info = sizeof(VIDEO_MEMORY_INFORMATION);
        break;
    }
    case IOCTL_VIDEO_UNMAP_VIDEO_MEMORY:
        break;                             /* nothing was mapped */
    case IOCTL_VIDEO_SET_COLOR_REGISTERS: {
        VIDEO_CLUT *c = r->InputBuffer;
        if (r->InputBufferLength < 4 || c->FirstEntry + c->NumEntries > 256 ||
            r->InputBufferLength < 4 + 4u * c->NumEntries) { st = ERROR_INVALID_PARAMETER; break; }
        x->Dac[0x00] = (UCHAR)c->FirstEntry; __asm__ volatile("eieio" ::: "memory");
        for (ULONG i = 0; i < c->NumEntries; i++) {
            x->Dac[0x30] = c->Entry[i].Red; __asm__ volatile("eieio" ::: "memory");
            x->Dac[0x30] = c->Entry[i].Green; __asm__ volatile("eieio" ::: "memory");
            x->Dac[0x30] = c->Entry[i].Blue; __asm__ volatile("eieio" ::: "memory");
        }
        break;
    }
    default:
        st = ERROR_INVALID_FUNCTION;
        break;
    }
    r->StatusBlock->Status = st;
    r->StatusBlock->Information = info;
    return TRUE;
}
DEFINE_DESC(CtlStartIO);

/* The display driver GDI is to load: NT's frame buffer driver, which every install has
 * (TXTSETUP.SIF copies framebuf.dll from the CD), named in our Device0 key as the REG_MULTI_SZ
 * GDI reads.  Not through VideoPortSetRegistryParameters, which stores REG_BINARY: GDI's query of
 * a binary value copies it raw into a string it never allocated, and the kernel faults
 * (KMODE_EXCEPTION_NOT_HANDLED in RtlCopyMemory, found on the first GUI boot). */
static VOID CtlNameDisplayDriver(PUNICODE_STRING RegistryPath)
{
    static WCHAR path[256];
    static const WCHAR device0[] = u"\\Device0";
    ULONG n = RegistryPath ? RegistryPath->Length / 2 : 0;
    if (n == 0 || n + sizeof device0 / 2 > 256) return;
    for (ULONG i = 0; i < n; i++) path[i] = RegistryPath->Buffer[i];
    for (ULONG i = 0; i < sizeof device0 / 2; i++) path[n + i] = device0[i];
    static const WCHAR dll[] = u"framebuf\0";      /* the list's two terminators */
    NTSTATUS st = RtlWriteRegistryValue(0 /* RTL_REGISTRY_ABSOLUTE */, path, u"InstalledDisplayDrivers",
                                        7 /* REG_MULTI_SZ */, (PVOID)dll, sizeof dll);
    if (st != STATUS_SUCCESS) LogHex("pmctrl: cannot name the display driver in the registry: ", (ULONG)st);
}

NTSTATUS DriverEntry(PVOID DriverObject, PVOID RegistryPath)
{
    CtlNameDisplayDriver(RegistryPath);
    VIDEO_HW_INITIALIZATION_DATA d;
    UCHAR *p = (UCHAR *)&d;
    for (ULONG i = 0; i < sizeof d; i++) p[i] = 0;
    d.HwInitDataSize = sizeof d;
    /* Control is on Chaos, not on a bus NT knows; the video port asks for an adapter on each
     * bus of the type named, and FindAdapter looks for Control itself on bus 0 */
    d.AdapterInterfaceType = PCIBus;
    d.HwFindAdapter = DESC(CtlFindAdapter);
    d.HwInitialize = DESC(CtlInitialize);
    d.HwStartIO = DESC(CtlStartIO);
    d.HwDeviceExtensionSize = sizeof(CTL_EXTENSION);
    ULONG st = VideoPortInitialize(DriverObject, RegistryPath, &d, NULL);
    LogHex("pmctrl: VideoPortInitialize returned ", st);
    return (NTSTATUS)st;
}
