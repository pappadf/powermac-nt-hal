/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * Original work of this project.  The SCSI port driver's structures below are DDK-documented
 * interface facts, re-declared here in our own words and pinned with static asserts; their
 * offsets were read from what the shipped symc810.sys (NT 4.0 build 1381, PowerPC) stores into
 * them.  The MESH programming follows powermac-os2's shim/scsi.c (the same author's), which
 * follows Linux's mesh.c.  Nothing derives from leaked Windows NT source.  See PROVENANCE.md.
 *
 * mesh.sys -- a SCSI miniport for the internal bus of a Power Macintosh 7500 or 8500: Apple's
 * MESH cell (343S1146) in Grand Central at +0x18000, byte registers on 16-byte centres, its data
 * phases through DBDMA channel 10 at +0x8A00 and its interrupt Grand Central's 13.
 *
 * One command at a time, nothing disconnects, as the ARC firmware's polled driver does it
 * (src/arcfw/scsi.c), but driven by MESH's interrupts: each step of a command -- arbitrate,
 * select with ATN, the IDENTIFY message, the command bytes, the data, status, message in, bus
 * free -- is one MESH sequence, and the interrupt that ends it starts the next.  Data moves
 * through DBDMA, one descriptor per physically contiguous piece of the transfer.  A target that
 * goes to the status phase before its data is all sent (a short read, or an error) raises a
 * phase mismatch, which ends the data step.  Anything else unexpected resets the bus.
 *
 * Byte order: the CPU is little-endian, and Grand Central sits behind Bandit, which reverses the
 * byte lanes, so DBDMA's registers and descriptors -- little-endian on the bus -- are written
 * with natural stores, as the ARC firmware does. */
#include "nt.h"

/* ---- the SCSI port driver's interface ------------------------------------------------------ */

typedef struct _ACCESS_RANGE {
    PHYSICAL_ADDRESS RangeStart;
    ULONG RangeLength;
    BOOLEAN RangeInMemory;
    UCHAR Pad[3];
} ACCESS_RANGE, *PACCESS_RANGE;
_Static_assert(sizeof(ACCESS_RANGE) == 16, "ACCESS_RANGE is 16 bytes");

/* What FindAdapter fills in.  symc810.sys's stores (MaximumTransferLength at 0x18,
 * NumberOfPhysicalBreaks 0x1C, AccessRanges read at 0x38, NumberOfBuses 0x40, InitiatorBusId
 * 0x41, ScatterGather 0x49, Master 0x4A, AdapterScansDown 0x4C, Dma32BitAddresses 0x4F,
 * TaggedQueuing 0x53, MaximumNumberOfTargets 0x59, SlotNumber read at 0x5C) place every field
 * this driver touches; the structure continues past SlotNumber, which nothing here reads. */
typedef struct _PORT_CONFIGURATION_INFORMATION {
    ULONG Length;                        /* 0x00 */
    ULONG SystemIoBusNumber;             /* 0x04 */
    INTERFACE_TYPE AdapterInterfaceType; /* 0x08 */
    ULONG BusInterruptLevel;             /* 0x0C */
    ULONG BusInterruptVector;            /* 0x10 */
    KINTERRUPT_MODE InterruptMode;       /* 0x14 */
    ULONG MaximumTransferLength;         /* 0x18 */
    ULONG NumberOfPhysicalBreaks;        /* 0x1C */
    ULONG DmaChannel, DmaPort;           /* 0x20 */
    ULONG DmaWidth, DmaSpeed;            /* 0x28 */
    ULONG AlignmentMask;                 /* 0x30 */
    ULONG NumberOfAccessRanges;          /* 0x34 */
    PACCESS_RANGE AccessRanges;          /* 0x38 */
    PVOID Reserved;                      /* 0x3C */
    UCHAR NumberOfBuses;                 /* 0x40 */
    UCHAR InitiatorBusId[8];             /* 0x41 */
    BOOLEAN ScatterGather;               /* 0x49 */
    BOOLEAN Master;                      /* 0x4A */
    BOOLEAN CachesData;                  /* 0x4B */
    BOOLEAN AdapterScansDown;            /* 0x4C */
    BOOLEAN AtdiskPrimaryClaimed;        /* 0x4D */
    BOOLEAN AtdiskSecondaryClaimed;      /* 0x4E */
    BOOLEAN Dma32BitAddresses;           /* 0x4F */
    BOOLEAN DemandMode;                  /* 0x50 */
    BOOLEAN MapBuffers;                  /* 0x51 */
    BOOLEAN NeedPhysicalAddresses;       /* 0x52 */
    BOOLEAN TaggedQueuing;               /* 0x53 */
    BOOLEAN AutoRequestSense;            /* 0x54 */
    BOOLEAN MultipleRequestPerLu;        /* 0x55 */
    BOOLEAN ReceiveEvent;                /* 0x56 */
    BOOLEAN RealModeInitialized;         /* 0x57 */
    BOOLEAN BufferAccessScsiPortControlled; /* 0x58 */
    UCHAR MaximumNumberOfTargets;        /* 0x59 */
    UCHAR ReservedUchars[2];             /* 0x5A */
    ULONG SlotNumber;                    /* 0x5C */
} PORT_CONFIGURATION_INFORMATION, *PPORT_CONFIGURATION_INFORMATION;
_Static_assert(__builtin_offsetof(PORT_CONFIGURATION_INFORMATION, AccessRanges) == 0x38, "AccessRanges at 0x38");
_Static_assert(__builtin_offsetof(PORT_CONFIGURATION_INFORMATION, ScatterGather) == 0x49, "ScatterGather at 0x49");
_Static_assert(__builtin_offsetof(PORT_CONFIGURATION_INFORMATION, TaggedQueuing) == 0x53, "TaggedQueuing at 0x53");
_Static_assert(__builtin_offsetof(PORT_CONFIGURATION_INFORMATION, SlotNumber) == 0x5C, "SlotNumber at 0x5C");

typedef struct _SCSI_REQUEST_BLOCK {
    USHORT Length;                       /* 0x00 */
    UCHAR Function;                      /* 0x02 */
    UCHAR SrbStatus;                     /* 0x03 */
    UCHAR ScsiStatus;                    /* 0x04 */
    UCHAR PathId, TargetId, Lun;         /* 0x05 */
    UCHAR QueueTag, QueueAction;         /* 0x08 */
    UCHAR CdbLength;                     /* 0x0A */
    UCHAR SenseInfoBufferLength;         /* 0x0B */
    ULONG SrbFlags;                      /* 0x0C */
    ULONG DataTransferLength;            /* 0x10 */
    ULONG TimeOutValue;                  /* 0x14 */
    PVOID DataBuffer;                    /* 0x18 */
    PVOID SenseInfoBuffer;               /* 0x1C */
    struct _SCSI_REQUEST_BLOCK *NextSrb; /* 0x20 */
    PVOID OriginalRequest;               /* 0x24 */
    PVOID SrbExtension;                  /* 0x28 */
    ULONG QueueSortKey;                  /* 0x2C */
    UCHAR Cdb[16];                       /* 0x30 */
} SCSI_REQUEST_BLOCK, *PSCSI_REQUEST_BLOCK;
_Static_assert(sizeof(SCSI_REQUEST_BLOCK) == 0x40, "SCSI_REQUEST_BLOCK is 0x40 bytes");

/* symc810.sys's DriverEntry: HwInitializationDataSize 0x4C at 0, AdapterInterfaceType 4, the
 * six routines 8..0x20, DeviceExtensionSize 0x24, SpecificLuExtensionSize 0x28, SrbExtensionSize
 * 0x2C, NumberOfAccessRanges 0x30, NeedPhysicalAddresses 0x39, VendorIdLength 0x3E, VendorId
 * 0x40, DeviceIdLength 0x46, DeviceId 0x48. */
typedef struct _HW_INITIALIZATION_DATA {
    ULONG HwInitializationDataSize;      /* 0x00 */
    INTERFACE_TYPE AdapterInterfaceType; /* 0x04 */
    PVOID HwInitialize;                  /* 0x08  descriptors, all of them */
    PVOID HwStartIo;                     /* 0x0C */
    PVOID HwInterrupt;                   /* 0x10 */
    PVOID HwFindAdapter;                 /* 0x14 */
    PVOID HwResetBus;                    /* 0x18 */
    PVOID HwDmaStarted;                  /* 0x1C */
    PVOID HwAdapterState;                /* 0x20 */
    ULONG DeviceExtensionSize;           /* 0x24 */
    ULONG SpecificLuExtensionSize;       /* 0x28 */
    ULONG SrbExtensionSize;              /* 0x2C */
    ULONG NumberOfAccessRanges;          /* 0x30 */
    PVOID Reserved;                      /* 0x34 */
    BOOLEAN MapBuffers;                  /* 0x38 */
    BOOLEAN NeedPhysicalAddresses;       /* 0x39 */
    BOOLEAN TaggedQueuing;               /* 0x3A */
    BOOLEAN AutoRequestSense;            /* 0x3B */
    BOOLEAN MultipleRequestPerLu;        /* 0x3C */
    BOOLEAN ReceiveEvent;                /* 0x3D */
    USHORT VendorIdLength;               /* 0x3E */
    PVOID VendorId;                      /* 0x40 */
    USHORT ReservedUshort;               /* 0x44 */
    USHORT DeviceIdLength;               /* 0x46 */
    PVOID DeviceId;                      /* 0x48 */
} HW_INITIALIZATION_DATA;
_Static_assert(sizeof(HW_INITIALIZATION_DATA) == 0x4C, "HW_INITIALIZATION_DATA is 0x4C bytes");

#define SRB_FUNCTION_EXECUTE_SCSI   0x00
#define SRB_FUNCTION_ABORT_COMMAND  0x10
#define SRB_FUNCTION_RESET_BUS      0x12
#define SRB_FUNCTION_RESET_DEVICE   0x13
#define SRB_FLAGS_DATA_IN           0x40
#define SRB_FLAGS_DATA_OUT          0x80
#define SRB_STATUS_SUCCESS          0x01
#define SRB_STATUS_ERROR            0x04
#define SRB_STATUS_INVALID_REQUEST  0x06
#define SRB_STATUS_SELECTION_TIMEOUT 0x0A
#define SRB_STATUS_BUS_RESET        0x0E
#define SRB_STATUS_DATA_OVERRUN     0x12
#define SRB_STATUS_PHASE_SEQUENCE_FAILURE 0x14
#define SP_RETURN_NOT_FOUND 0
#define SP_RETURN_FOUND     1
enum { RequestComplete, NextRequest, NextLuRequest, ResetDetected };

ULONG ScsiPortInitialize(PVOID Argument1, PVOID Argument2, HW_INITIALIZATION_DATA *Data, PVOID HwContext);
PVOID ScsiPortGetDeviceBase(PVOID Ext, INTERFACE_TYPE BusType, ULONG Bus, PHYSICAL_ADDRESS Address, ULONG Bytes, BOOLEAN InIoSpace);
/* SCSI_PHYSICAL_ADDRESS ScsiPortGetPhysicalAddress(...): a returned union, through the hidden
 * first argument (include/nt.h) */
VOID ScsiPortGetPhysicalAddress(PPHYSICAL_ADDRESS Result, PVOID Ext, PSCSI_REQUEST_BLOCK Srb, PVOID Va, PULONG Length);
PVOID ScsiPortGetUncachedExtension(PVOID Ext, PPORT_CONFIGURATION_INFORMATION Config, ULONG Bytes);
VOID ScsiPortNotification(ULONG Type, PVOID Ext, PVOID Arg);       /* variadic in the DDK */
VOID ScsiPortCompleteRequest(PVOID Ext, UCHAR PathId, UCHAR TargetId, UCHAR Lun, UCHAR SrbStatus);
VOID HalDisplayString(PCHAR String);
VOID KeStallExecutionProcessor(ULONG Microseconds);

/* ---- MESH and its DMA channel -------------------------------------------------------------- */

#define MESH_PHYS 0xF3018000u
#define DBDMA_PHYS 0xF3008A00u
#define MESH_IRQ  13

#define MR_COUNT_LO  0x0
#define MR_COUNT_HI  0x1
#define MR_FIFO      0x2
#define MR_SEQUENCE  0x3
#define MR_BUS0      0x4
#define MR_BUS1      0x5
#define MR_FIFO_CNT  0x6
#define MR_EXCEPTION 0x7
#define MR_ERROR     0x8
#define MR_INTR_MASK 0x9
#define MR_INTERRUPT 0xA
#define MR_SOURCE_ID 0xB
#define MR_DEST_ID   0xC
#define MR_SYNC      0xD
#define MR_MESH_ID   0xE
#define MR_SEL_TO    0xF

#define SEQ_DMA       0x80
#define SEQ_ATN       0x20
#define SEQ_ARBITRATE 0x1
#define SEQ_SELECT    0x2
#define SEQ_COMMAND   0x3
#define SEQ_STATUS    0x4
#define SEQ_DATAOUT   0x5
#define SEQ_DATAIN    0x6
#define SEQ_MSGOUT    0x7
#define SEQ_MSGIN     0x8
#define SEQ_BUSFREE   0x9
#define SEQ_DISRESEL  0xD
#define SEQ_RESETMESH 0xE
#define SEQ_FLUSHFIFO 0xF

#define BS0_REQ      0x20
#define BS1_RST      0x80
#define INT_CMDDONE  0x01
#define EXC_SELTO    0x01
#define EXC_PHASEMM  0x02

#define DMA_CONTROL    0x00
#define DMA_STATUS     0x04
#define DMA_CMDPTR     0x0C
#define DMA_RUN        0x8000u
#define DMA_ACTIVE     0x0400u
#define DB_OUTPUT_MORE 0x0000u
#define DB_OUTPUT_LAST 0x1000u
#define DB_INPUT_MORE  0x2000u
#define DB_INPUT_LAST  0x3000u
#define DB_STOP        0x7000u

#define MAX_TRANSFER 0xF000u           /* MESH's transfer count is 16 bits */
#define MAX_PIECES   17                /* 0xF000 bytes span at most 16 page boundaries */

enum { S_IDLE, S_ARBITRATE, S_SELECT, S_MSGOUT, S_COMMAND, S_DATA, S_STATUS, S_MSGIN, S_BUSFREE };

typedef struct _MESH_EXTENSION {
    volatile UCHAR *Regs;
    volatile ULONG *Dma;               /* index = byte offset / 4 */
    volatile ULONG *Desc;              /* MAX_PIECES + 1 DBDMA descriptors, 16 bytes each */
    ULONG DescPhys;
    PSCSI_REQUEST_BLOCK Srb;           /* the one command in progress */
    ULONG State;
    ULONG Length;                      /* the data the command moves */
    BOOLEAN Out;
    UCHAR Status;
    BOOLEAN Short;
} MESH_EXTENSION, *PMESH_EXTENSION;

/* ---- a little console ------------------------------------------------------------------------ */

static VOID MeshLog(const char *s, ULONG a, ULONG b)
{
    char buf[160];
    ULONG n = 0, arg = 0;
    for (; *s && n + 12 < sizeof buf; s++) {
        if (*s != '%') { buf[n++] = *s; continue; }
        ULONG v = arg++ ? b : a;
        for (int sh = 28; sh >= 0; sh -= 4) buf[n++] = "0123456789abcdef"[(v >> sh) & 15];
    }
    buf[n++] = '\n';
    buf[n] = 0;
    HalDisplayString(buf);
}

/* ---- the cell ---------------------------------------------------------------------------------- */

static inline VOID eieio(VOID) { __asm__ volatile("eieio" ::: "memory"); }
static UCHAR mr_rd(PMESH_EXTENSION x, ULONG r) { UCHAR v = x->Regs[16u * r]; eieio(); return v; }
static VOID mr_wr(PMESH_EXTENSION x, ULONG r, UCHAR v) { x->Regs[16u * r] = v; eieio(); }
static VOID mr_count(PMESH_EXTENSION x, ULONG n) { mr_wr(x, MR_COUNT_LO, (UCHAR)n); mr_wr(x, MR_COUNT_HI, (UCHAR)(n >> 8)); }
static VOID dma_wr(PMESH_EXTENSION x, ULONG o, ULONG v) { x->Dma[o / 4] = v; eieio(); }
static ULONG dma_rd(PMESH_EXTENSION x, ULONG o) { ULONG v = x->Dma[o / 4]; eieio(); return v; }

static VOID dma_stop(PMESH_EXTENSION x)
{
    dma_wr(x, DMA_CONTROL, 0xF000u << 16);    /* RUN, PAUSE, FLUSH and WAKE all clear */
    for (ULONG n = 0; n < 1000 && (dma_rd(x, DMA_STATUS) & DMA_ACTIVE); n++) KeStallExecutionProcessor(1);
}

/* the end of a data phase: a program that ran to its STOP has written everything and gone idle
 * by itself; one cut short (a phase mismatch) never will, and is stopped */
static VOID dma_finish(PMESH_EXTENSION x)
{
    for (ULONG n = 0; n < 1000 && (dma_rd(x, DMA_STATUS) & DMA_ACTIVE); n++) KeStallExecutionProcessor(1);
    dma_stop(x);
}

/* the cell to a known state, interrupts on.  RESETMESH ends with a command-done interrupt; it
 * is waited for and cleared here with the mask off, so it reaches no one. */
static VOID mesh_init(PMESH_EXTENSION x)
{
    dma_stop(x);
    mr_wr(x, MR_INTR_MASK, 0x00);
    mr_wr(x, MR_SEQUENCE, SEQ_RESETMESH);
    for (ULONG n = 0; n < 1000 && !(mr_rd(x, MR_INTERRUPT) & 7); n++) KeStallExecutionProcessor(1);
    mr_wr(x, MR_EXCEPTION, 0xFF);
    mr_wr(x, MR_ERROR, 0xFF);
    mr_wr(x, MR_INTERRUPT, 0xFF);
    mr_wr(x, MR_SOURCE_ID, 7);
    mr_wr(x, MR_SEL_TO, 25);                  /* 250 ms */
    mr_wr(x, MR_SYNC, 2);                     /* asynchronous */
    mr_wr(x, MR_SEQUENCE, SEQ_FLUSHFIFO);
    mr_wr(x, MR_INTR_MASK, 0x07);             /* command done, exception, error */
}

/* SCSI bus reset: every target lets go, whatever phase it was in */
static VOID mesh_reset_bus(PMESH_EXTENSION x)
{
    dma_stop(x);
    mr_wr(x, MR_INTR_MASK, 0x00);
    mr_wr(x, MR_BUS1, BS1_RST);
    KeStallExecutionProcessor(30);
    mr_wr(x, MR_BUS1, 0);
    mesh_init(x);
}

static VOID mesh_seq(PMESH_EXTENSION x, ULONG state, UCHAR seq) { x->State = state; mr_wr(x, MR_SEQUENCE, seq); }

/* the current command ends with `srb_status`; the port driver may send the next */
static VOID mesh_complete(PMESH_EXTENSION x, UCHAR srb_status)
{
    PSCSI_REQUEST_BLOCK srb = x->Srb;
    x->Srb = NULL;
    x->State = S_IDLE;
    if (!srb) return;
    srb->SrbStatus = srb_status;
    ScsiPortNotification(RequestComplete, x, srb);
    ScsiPortNotification(NextRequest, x, NULL);
}

/* something no command should do: reset the bus, fail the command, tell the port driver */
static VOID mesh_fail(PMESH_EXTENSION x, ULONG what, ULONG r)
{
    static ULONG logged;
    if (logged < 8) { logged++; MeshLog("mesh: failed in step %, events %; resetting the bus", what, r); }
    mesh_reset_bus(x);
    mesh_complete(x, SRB_STATUS_PHASE_SEQUENCE_FAILURE);
    ScsiPortNotification(ResetDetected, x, NULL);
}

/* the DBDMA program for the transfer: one descriptor per physically contiguous piece, the last
 * one *_LAST, then STOP.  FALSE if the buffer takes more pieces than there are descriptors. */
static BOOLEAN mesh_program(PMESH_EXTENSION x, PSCSI_REQUEST_BLOCK srb)
{
    ULONG done = 0, k = 0;
    while (done < x->Length) {
        if (k == MAX_PIECES) return FALSE;
        PHYSICAL_ADDRESS pa; ULONG len = 0;
        ScsiPortGetPhysicalAddress(&pa, x, srb, (PUCHAR)srb->DataBuffer + done, &len);
        if (len == 0) return FALSE;
        if (len > x->Length - done) len = x->Length - done;
        BOOLEAN last = done + len == x->Length;
        ULONG cmd = x->Out ? (last ? DB_OUTPUT_LAST : DB_OUTPUT_MORE) : (last ? DB_INPUT_LAST : DB_INPUT_MORE);
        volatile ULONG *d = x->Desc + 4 * k;
        d[0] = cmd << 16 | len;
        d[1] = LOW32(pa);
        d[2] = 0;
        d[3] = 0;
        done += len; k++;
    }
    volatile ULONG *d = x->Desc + 4 * k;
    d[0] = DB_STOP << 16; d[1] = d[2] = d[3] = 0;
    __asm__ volatile("sync" ::: "memory");
    return TRUE;
}

/* ---- the miniport's routines ------------------------------------------------------------------- */

static ULONG MeshFindAdapter(PVOID Ext, PVOID Context, PVOID BusInformation, PCHAR ArgumentString,
                             PPORT_CONFIGURATION_INFORMATION Config, PBOOLEAN Again)
{
    PMESH_EXTENSION x = Ext;
    *Again = FALSE;
    PACCESS_RANGE ar = Config->AccessRanges;
    MeshLog("mesh: FindAdapter bus %, slot %", Config->SystemIoBusNumber, Config->SlotNumber);
    MeshLog("mesh:   range 0 % length %", ar ? LOW32(ar[0].RangeStart) : 0, ar ? ar[0].RangeLength : 0);
    MeshLog("mesh:   interrupt level % vector %", Config->BusInterruptLevel, Config->BusInterruptVector);
    if (Config->SystemIoBusNumber != 0) return SP_RETURN_NOT_FOUND;

    x->Regs = ScsiPortGetDeviceBase(Ext, Config->AdapterInterfaceType, 0, MESH_PHYS, 0x100, FALSE);
    x->Dma = ScsiPortGetDeviceBase(Ext, Config->AdapterInterfaceType, 0, DBDMA_PHYS, 0x100, FALSE);
    if (!x->Regs || !x->Dma) { MeshLog("mesh: cannot map the cell (% %)", (ULONG)x->Regs, (ULONG)x->Dma); return SP_RETURN_NOT_FOUND; }
    UCHAR id = mr_rd(x, MR_MESH_ID);
    MeshLog("mesh: MESH id % at %", id, MESH_PHYS);
    if ((id & 0xE0) != 0xE0 || id == 0xFF) return SP_RETURN_NOT_FOUND;

    if (ar && Config->NumberOfAccessRanges >= 2) {
        ar[0].RangeStart = MESH_PHYS; ar[0].RangeLength = 0x100; ar[0].RangeInMemory = TRUE;
        ar[1].RangeStart = DBDMA_PHYS; ar[1].RangeLength = 0x100; ar[1].RangeInMemory = TRUE;
    }
    Config->BusInterruptLevel = MESH_IRQ;
    Config->BusInterruptVector = MESH_IRQ;
    Config->InterruptMode = LevelSensitive;
    Config->MaximumTransferLength = MAX_TRANSFER;
    Config->NumberOfPhysicalBreaks = MAX_PIECES - 1;
    Config->AlignmentMask = 0;
    Config->NumberOfBuses = 1;
    Config->InitiatorBusId[0] = 7;
    Config->ScatterGather = TRUE;
    Config->Master = TRUE;
    Config->Dma32BitAddresses = TRUE;
    Config->MapBuffers = TRUE;
    Config->NeedPhysicalAddresses = TRUE;
    Config->MaximumNumberOfTargets = 8;

    x->Desc = ScsiPortGetUncachedExtension(Ext, Config, 16 * (MAX_PIECES + 1));
    if (!x->Desc) { MeshLog("mesh: no memory for the DMA program", 0, 0); return SP_RETURN_NOT_FOUND; }
    PHYSICAL_ADDRESS pa; ULONG len = 0;
    ScsiPortGetPhysicalAddress(&pa, Ext, NULL, (PVOID)x->Desc, &len);
    x->DescPhys = LOW32(pa);
    MeshLog("mesh: the internal SCSI bus of a Power Macintosh 7500/8500 (powermac-nt-hal), DMA program at %", x->DescPhys, 0);
    return SP_RETURN_FOUND;
}
DEFINE_DESC(MeshFindAdapter);

static BOOLEAN MeshInitialize(PVOID Ext)
{
    PMESH_EXTENSION x = Ext;
    x->Srb = NULL;
    x->State = S_IDLE;
    mesh_init(x);
    return TRUE;
}
DEFINE_DESC(MeshInitialize);

static BOOLEAN MeshResetBus(PVOID Ext, ULONG PathId)
{
    PMESH_EXTENSION x = Ext;
    mesh_reset_bus(x);
    x->Srb = NULL;
    x->State = S_IDLE;
    ScsiPortCompleteRequest(Ext, (UCHAR)PathId, 0xFF, 0xFF, SRB_STATUS_BUS_RESET);
    ScsiPortNotification(NextRequest, Ext, NULL);
    return TRUE;
}
DEFINE_DESC(MeshResetBus);

static BOOLEAN MeshStartIo(PVOID Ext, PSCSI_REQUEST_BLOCK Srb)
{
    PMESH_EXTENSION x = Ext;
    switch (Srb->Function) {
    case SRB_FUNCTION_EXECUTE_SCSI:
        break;
    case SRB_FUNCTION_RESET_BUS:
    case SRB_FUNCTION_RESET_DEVICE:
    case SRB_FUNCTION_ABORT_COMMAND:
        /* nothing disconnects, so nothing is outstanding but what is on the bus: reset it */
        mesh_reset_bus(x);
        if (x->Srb) { PSCSI_REQUEST_BLOCK s = x->Srb; x->Srb = NULL; s->SrbStatus = SRB_STATUS_BUS_RESET; ScsiPortNotification(RequestComplete, Ext, s); }
        x->State = S_IDLE;
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        ScsiPortNotification(RequestComplete, Ext, Srb);
        ScsiPortNotification(NextRequest, Ext, NULL);
        return TRUE;
    default:
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        ScsiPortNotification(RequestComplete, Ext, Srb);
        ScsiPortNotification(NextRequest, Ext, NULL);
        return TRUE;
    }
    /* one logical unit per target, and never ourselves */
    if (Srb->Lun != 0 || Srb->TargetId >= 7 || Srb->PathId != 0 || Srb->CdbLength > 12) {
        Srb->SrbStatus = SRB_STATUS_SELECTION_TIMEOUT;
        ScsiPortNotification(RequestComplete, Ext, Srb);
        ScsiPortNotification(NextRequest, Ext, NULL);
        return TRUE;
    }
    x->Srb = Srb;
    x->Status = 0xFF;
    x->Short = FALSE;
    x->Out = (Srb->SrbFlags & SRB_FLAGS_DATA_OUT) != 0;
    x->Length = (Srb->SrbFlags & (SRB_FLAGS_DATA_IN | SRB_FLAGS_DATA_OUT)) ? Srb->DataTransferLength : 0;
    if (x->Length > MAX_TRANSFER || (x->Length && !mesh_program(x, Srb))) {
        mesh_complete(x, SRB_STATUS_INVALID_REQUEST);
        return TRUE;
    }
    /* arbitrate with our own ID in dest_id (MESH puts both IDs on the bus while it arbitrates)
     * and reselection off (the cell can hang in ARBITRATE on a reselection race), as Linux's
     * mesh.c does */
    mr_wr(x, MR_INTERRUPT, 0x07);
    mr_wr(x, MR_SEQUENCE, SEQ_FLUSHFIFO);
    mr_wr(x, MR_DEST_ID, 7);
    mr_wr(x, MR_SEQUENCE, SEQ_DISRESEL);
    mesh_seq(x, S_ARBITRATE, SEQ_ARBITRATE);
    return TRUE;
}
DEFINE_DESC(MeshStartIo);

/* a REQ the target is about to raise: MESH drops ATN for a message only once the target asks
 * for it (Linux's mesh.c waits the same way).  Microseconds after selection on any target. */
static BOOLEAN mesh_wait_req(PMESH_EXTENSION x)
{
    for (ULONG n = 0; n < 2000; n++) {
        if (mr_rd(x, MR_BUS0) & BS0_REQ) return TRUE;
        KeStallExecutionProcessor(1);
    }
    return FALSE;
}

static BOOLEAN MeshInterrupt(PVOID Ext)
{
    PMESH_EXTENSION x = Ext;
    UCHAR in = mr_rd(x, MR_INTERRUPT);
    if (!(in & 7)) return FALSE;
    ULONG r = in | (ULONG)mr_rd(x, MR_EXCEPTION) << 8 | (ULONG)mr_rd(x, MR_ERROR) << 16;
    mr_wr(x, MR_EXCEPTION, 0xFF);
    mr_wr(x, MR_ERROR, 0xFF);
    mr_wr(x, MR_INTERRUPT, 0x07);
    PSCSI_REQUEST_BLOCK srb = x->Srb;
    if (!srb) return TRUE;                    /* nothing in progress: a reset's echo */
    BOOLEAN clean = r == INT_CMDDONE;

    switch (x->State) {
    case S_ARBITRATE:
        if (!clean) break;
        mr_wr(x, MR_DEST_ID, srb->TargetId);
        mesh_seq(x, S_SELECT, SEQ_SELECT | SEQ_ATN);
        return TRUE;
    case S_SELECT:
        if (r & (EXC_SELTO << 8)) {           /* nothing at this ID */
            mr_wr(x, MR_SEQUENCE, SEQ_FLUSHFIFO);
            mesh_complete(x, SRB_STATUS_SELECTION_TIMEOUT);
            return TRUE;
        }
        if (!clean || !mesh_wait_req(x)) break;
        /* IDENTIFY, LUN 0, no disconnect: the message's only byte, so ATN drops for it */
        mr_count(x, 1);
        mesh_seq(x, S_MSGOUT, SEQ_MSGOUT);
        mr_wr(x, MR_FIFO, 0x80);
        return TRUE;
    case S_MSGOUT:
        if (!clean) break;
        mr_count(x, srb->CdbLength);
        mesh_seq(x, S_COMMAND, SEQ_COMMAND);
        for (ULONG i = 0; i < srb->CdbLength; i++) mr_wr(x, MR_FIFO, srb->Cdb[i]);
        return TRUE;
    case S_COMMAND:
        if (!clean) break;
        if (x->Length) {
            dma_stop(x);
            dma_wr(x, DMA_CMDPTR, x->DescPhys);
            dma_wr(x, DMA_CONTROL, DMA_RUN << 16 | DMA_RUN);
            mr_count(x, x->Length);
            mesh_seq(x, S_DATA, (x->Out ? SEQ_DATAOUT : SEQ_DATAIN) | SEQ_DMA);
            return TRUE;
        }
        mr_count(x, 1);
        mesh_seq(x, S_STATUS, SEQ_STATUS);
        return TRUE;
    case S_DATA:
        dma_finish(x);
        if (r & (EXC_PHASEMM << 8)) {         /* the target moved on early: a short transfer */
            ULONG left = mr_rd(x, MR_COUNT_LO) | (ULONG)mr_rd(x, MR_COUNT_HI) << 8;
            if (left > x->Length) left = x->Length;
            srb->DataTransferLength = x->Length - left;
            x->Short = TRUE;
            mr_wr(x, MR_SEQUENCE, SEQ_FLUSHFIFO);
        } else if (!clean) break;
        mr_count(x, 1);
        mesh_seq(x, S_STATUS, SEQ_STATUS);
        return TRUE;
    case S_STATUS:
        if (!clean || !mr_rd(x, MR_FIFO_CNT)) break;
        x->Status = mr_rd(x, MR_FIFO);
        mr_count(x, 1);
        mesh_seq(x, S_MSGIN, SEQ_MSGIN);
        return TRUE;
    case S_MSGIN:
        if (!clean) break;
        while (mr_rd(x, MR_FIFO_CNT)) (void)mr_rd(x, MR_FIFO);   /* COMMAND COMPLETE */
        mesh_seq(x, S_BUSFREE, SEQ_BUSFREE);
        return TRUE;
    case S_BUSFREE:
        if (!clean) break;
        srb->ScsiStatus = x->Status;
        mesh_complete(x, x->Status != 0 ? SRB_STATUS_ERROR : x->Short ? SRB_STATUS_DATA_OVERRUN : SRB_STATUS_SUCCESS);
        return TRUE;
    default:
        return TRUE;
    }
    mesh_fail(x, x->State, r);
    return TRUE;
}
DEFINE_DESC(MeshInterrupt);

NTSTATUS DriverEntry(PVOID DriverObject, PVOID RegistryPath)
{
    HW_INITIALIZATION_DATA d;
    UCHAR *p = (UCHAR *)&d;
    for (ULONG i = 0; i < sizeof d; i++) p[i] = 0;
    d.HwInitializationDataSize = sizeof d;
    /* MESH is a cell of Grand Central, which is a PCI device (Apple 106B:0002, Bandit 1's device
     * 16): the port driver finds it by those ids, as it finds any PCI adapter, and hands this
     * driver its bus and slot.  A bus of type Internal is never offered: the machine's hardware
     * description has none. */
    d.AdapterInterfaceType = PCIBus;
    static char vendor[4] = { '1', '0', '6', 'b' }, device[4] = { '0', '0', '0', '2' };
    d.VendorId = vendor; d.VendorIdLength = 4;
    d.DeviceId = device; d.DeviceIdLength = 4;
    d.HwInitialize = DESC(MeshInitialize);
    d.HwStartIo = DESC(MeshStartIo);
    d.HwInterrupt = DESC(MeshInterrupt);
    d.HwFindAdapter = DESC(MeshFindAdapter);
    d.HwResetBus = DESC(MeshResetBus);
    d.DeviceExtensionSize = sizeof(MESH_EXTENSION);
    d.NumberOfAccessRanges = 2;
    d.MapBuffers = TRUE;
    d.NeedPhysicalAddresses = TRUE;
    ULONG st = ScsiPortInitialize(DriverObject, RegistryPath, &d, NULL);
    MeshLog("mesh: ScsiPortInitialize returned %", st, 0);
    return (NTSTATUS)st;
}
