/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * Original work of this project.  The NT structure layouts below are DDK-documented interface
 * facts, re-declared here in our own words and pinned with static asserts; the IOCTL codes are
 * computed from the documented CTL_CODE formula.  Nothing derives from leaked Windows NT source
 * and nothing is copied from another driver.  See PROVENANCE.md.
 *
 * adbport.h — adbport.sys: the Apple Desktop Bus keyboard and mouse as NT port devices, and the
 * OEM disk image the boot floppy left in RAM as \Device\Floppy0.
 *
 * Why one driver does three things.  Text-mode Setup loads OEM drivers through exactly one prompt
 * that takes arbitrary kernel drivers, the SCSI one, and each pick costs the user a trip through
 * "S, Other, Enter".  Keyboard, mouse and the disk Setup copies the OEM files from are what an
 * ANS install needs and a stock CD cannot supply, so they arrive together.  maciNTosh's driver
 * has the same shape for the same reason; ours is written to our own contract (oemdisk.h) and
 * our own HAL.
 *
 * STATUS: a draft, never loaded.  Every offset marked "verify" below is what the DDK says and
 * what the disassembled shipped drivers appear to use, but the first load will be the test. */
#pragma once
#include "nt.h"
#include "oemdisk.h"

/* ---- the kernel objects a driver touches ---------------------------------------------------- */

typedef ULONG KSPIN_LOCK, *PKSPIN_LOCK;
typedef struct _KDEVICE_QUEUE { CSHORT Type, Size; LIST_ENTRY DeviceListHead; KSPIN_LOCK Lock; BOOLEAN Busy; UCHAR Pad[3]; } KDEVICE_QUEUE;
_Static_assert(sizeof(KDEVICE_QUEUE) == 20, "KDEVICE_QUEUE is 20 bytes on NT 4.0");

/* NT 4.0's DEVICE_OBJECT, 0xB8 bytes.  Only the fields this driver reads or writes are named;
 * the rest keep their place.  verify: DeviceExtension 0x28, Flags 0x1C, StackSize 0x30. */
typedef struct _DEVICE_OBJECT {
    CSHORT Type, Size;                  /* 0x00 */
    LONG ReferenceCount;                /* 0x04 */
    PDRIVER_OBJECT DriverObject;        /* 0x08 */
    PDEVICE_OBJECT NextDevice;          /* 0x0C */
    PDEVICE_OBJECT AttachedDevice;      /* 0x10 */
    PIRP CurrentIrp;                    /* 0x14 */
    PVOID Timer;                        /* 0x18 */
    ULONG Flags;                        /* 0x1C */
    ULONG Characteristics;              /* 0x20 */
    PVOID Vpb;                          /* 0x24 */
    PVOID DeviceExtension;              /* 0x28 */
    ULONG DeviceType;                   /* 0x2C */
    CHAR StackSize; UCHAR Pad0[3];      /* 0x30 */
    UCHAR Queue[40];                    /* 0x34  LIST_ENTRY | WAIT_CONTEXT_BLOCK */
    ULONG AlignmentRequirement;         /* 0x5C */
    KDEVICE_QUEUE DeviceQueue;          /* 0x60 */
    ULONG Dpc[8];                       /* 0x74  a KDPC, 32 bytes here; nt.h's KDPC is padded to 64 for the HAL's own use */
    ULONG ActiveThreadCount;            /* 0x94 */
    PVOID SecurityDescriptor;           /* 0x98 */
    KEVENT DeviceLock;                  /* 0x9C */
    USHORT SectorSize, Spare1;          /* 0xAC */
    PVOID DeviceObjectExtension;        /* 0xB0 */
    PVOID Reserved;                     /* 0xB4 */
} DEVICE_OBJECT;
_Static_assert(sizeof(DEVICE_OBJECT) == 0xB8, "DEVICE_OBJECT is 0xB8 bytes on NT 4.0");

#define IRP_MJ_CREATE                   0x00
#define IRP_MJ_CLOSE                    0x02
#define IRP_MJ_FLUSH_BUFFERS            0x09
#define IRP_MJ_QUERY_VOLUME_INFORMATION 0x0A
#define IRP_MJ_DEVICE_CONTROL           0x0E
#define IRP_MJ_INTERNAL_DEVICE_CONTROL  0x0F
#define IRP_MJ_SHUTDOWN                 0x10
#define IRP_MJ_MAXIMUM_FUNCTION         0x1B

/* NT 4.0's DRIVER_OBJECT, 0xA8 bytes.  The dispatch table holds { entry, toc } descriptors, so
 * every routine stored there goes through DEFINE_DESC (nt.h) -- see STORY.md wall 49.
 * verify: DriverName at 0x1C is what videoprt reads for its `\Driver\Vga` test, so that one is
 * confirmed from a shipped binary. */
typedef struct _DRIVER_OBJECT {
    CSHORT Type, Size;                  /* 0x00 */
    PDEVICE_OBJECT DeviceObject;        /* 0x04 */
    ULONG Flags;                        /* 0x08 */
    PVOID DriverStart;                  /* 0x0C */
    ULONG DriverSize;                   /* 0x10 */
    PVOID DriverSection;                /* 0x14 */
    PVOID DriverExtension;              /* 0x18 */
    UNICODE_STRING DriverName;          /* 0x1C */
    PUNICODE_STRING HardwareDatabase;   /* 0x24 */
    PVOID FastIoDispatch;               /* 0x28 */
    PVOID DriverInit;                   /* 0x2C */
    PVOID DriverStartIo;                /* 0x30 */
    PVOID DriverUnload;                 /* 0x34 */
    PVOID MajorFunction[IRP_MJ_MAXIMUM_FUNCTION + 1];   /* 0x38 */
} DRIVER_OBJECT;
_Static_assert(sizeof(DRIVER_OBJECT) == 0xA8, "DRIVER_OBJECT is 0xA8 bytes on NT 4.0");

/* NT 4.0's IO_STACK_LOCATION, 0x24 bytes.  The parameter union is 4-byte packed in NT's own
 * headers, which is why ByteOffset is two ULONGs here and not a LARGE_INTEGER the compiler would
 * align to 8.  verify: the 36-byte size; DeviceIoControl.IoControlCode at 0x0C. */
typedef struct _IO_STACK_LOCATION {
    UCHAR MajorFunction, MinorFunction, Flags, Control;    /* 0x00 */
    union {
        struct { ULONG Length; ULONG Key; ULONG ByteOffsetLow; ULONG ByteOffsetHigh; } Read;
        struct { ULONG OutputBufferLength; ULONG InputBufferLength; ULONG IoControlCode; PVOID Type3InputBuffer; } DeviceIoControl;
        ULONG Raw[4];
    } Parameters;                                          /* 0x04 */
    PDEVICE_OBJECT DeviceObject;                           /* 0x14 */
    PVOID FileObject;                                      /* 0x18 */
    PVOID CompletionRoutine;                               /* 0x1C */
    PVOID Context;                                         /* 0x20 */
} IO_STACK_LOCATION, *PIO_STACK_LOCATION;
_Static_assert(sizeof(IO_STACK_LOCATION) == 0x24, "IO_STACK_LOCATION is 0x24 bytes on NT 4.0");

/* NT 4.0's IRP, 0x70 bytes.  verify: IoStatus 0x18, SystemBuffer 0x0C, UserBuffer 0x3C,
 * Tail.Overlay.CurrentStackLocation 0x60. */
typedef struct _IRP {
    CSHORT Type; USHORT Size;           /* 0x00 */
    PMDL MdlAddress;                    /* 0x04 */
    ULONG Flags;                        /* 0x08 */
    PVOID SystemBuffer;                 /* 0x0C  AssociatedIrp.SystemBuffer */
    LIST_ENTRY ThreadListEntry;         /* 0x10 */
    IO_STATUS_BLOCK IoStatus;           /* 0x18 */
    CHAR RequestorMode; BOOLEAN PendingReturned; CHAR StackCount; CHAR CurrentLocation;   /* 0x20 */
    BOOLEAN Cancel; UCHAR CancelIrql; CHAR ApcEnvironment; UCHAR AllocationFlags;          /* 0x24 */
    PIO_STATUS_BLOCK UserIosb;          /* 0x28 */
    PKEVENT UserEvent;                  /* 0x2C */
    ULONG Overlay[2];                   /* 0x30 */
    PVOID CancelRoutine;                /* 0x38 */
    PVOID UserBuffer;                   /* 0x3C */
    UCHAR TailHead[0x20];               /* 0x40  DeviceQueueEntry/DriverContext, Thread, AuxiliaryBuffer, ListEntry */
    PIO_STACK_LOCATION CurrentStackLocation;   /* 0x60 */
    PVOID OriginalFileObject;           /* 0x64 */
    UCHAR TailRest[8];                  /* 0x68  the KAPC alternative runs to 0x70 */
} IRP;
_Static_assert(sizeof(IRP) == 0x70, "IRP is 0x70 bytes on NT 4.0");

#define IoGetCurrentIrpStackLocation(Irp) ((Irp)->CurrentStackLocation)

#define MDL_MAPPED_TO_SYSTEM_VA      0x0001
#define MDL_SOURCE_IS_NONPAGED_POOL  0x0004
static inline PVOID MmGetSystemAddressForMdl(PMDL Mdl);

/* ---- the class-driver interfaces -------------------------------------------------------- */

typedef struct _KEYBOARD_INPUT_DATA { USHORT UnitId, MakeCode, Flags, Reserved; ULONG ExtraInformation; } KEYBOARD_INPUT_DATA, *PKEYBOARD_INPUT_DATA;
_Static_assert(sizeof(KEYBOARD_INPUT_DATA) == 12, "KEYBOARD_INPUT_DATA is 12 bytes");
#define KEY_MAKE  0
#define KEY_BREAK 1
#define KEY_E0    2

typedef struct _MOUSE_INPUT_DATA { USHORT UnitId, Flags, ButtonFlags, ButtonData; ULONG RawButtons; LONG LastX, LastY; ULONG ExtraInformation; } MOUSE_INPUT_DATA, *PMOUSE_INPUT_DATA;
_Static_assert(sizeof(MOUSE_INPUT_DATA) == 24, "MOUSE_INPUT_DATA is 24 bytes");
#define MOUSE_MOVE_RELATIVE      0
#define MOUSE_LEFT_BUTTON_DOWN   0x0001
#define MOUSE_LEFT_BUTTON_UP     0x0002
#define MOUSE_RIGHT_BUTTON_DOWN  0x0004
#define MOUSE_RIGHT_BUTTON_UP    0x0008

/* What kbdclass / mouclass hand a port driver on connect: their device, and the descriptor of
 * the routine that takes input packets -- called, per the DDK, at DISPATCH_LEVEL. */
typedef struct _CONNECT_DATA { PDEVICE_OBJECT ClassDeviceObject; PVOID ClassService; } CONNECT_DATA, *PCONNECT_DATA;

typedef struct _KEYBOARD_ATTRIBUTES {
    UCHAR KeyboardIdentifierType, KeyboardIdentifierSubtype;
    USHORT KeyboardMode, NumberOfFunctionKeys, NumberOfIndicators, NumberOfKeysTotal;
    ULONG InputDataQueueLength;
    struct { USHORT UnitId, Rate, Delay; } KeyRepeatMinimum, KeyRepeatMaximum;
} KEYBOARD_ATTRIBUTES;
typedef struct _KEYBOARD_INDICATOR_PARAMETERS { USHORT UnitId, LedFlags; } KEYBOARD_INDICATOR_PARAMETERS;
typedef struct _MOUSE_ATTRIBUTES { USHORT MouseIdentifier, NumberOfButtons, SampleRate, Pad; ULONG InputDataQueueLength; } MOUSE_ATTRIBUTES;

/* CTL_CODE(type, function, method, access) = (type << 16) | (access << 14) | (function << 2) | method */
#define IOCTL_INTERNAL_KEYBOARD_CONNECT     0x000B0203
#define IOCTL_INTERNAL_KEYBOARD_DISCONNECT  0x000B0403
#define IOCTL_INTERNAL_KEYBOARD_ENABLE      0x000B0803
#define IOCTL_INTERNAL_KEYBOARD_DISABLE     0x000B1003
#define IOCTL_KEYBOARD_QUERY_ATTRIBUTES     0x000B0000
#define IOCTL_KEYBOARD_SET_TYPEMATIC        0x000B0004
#define IOCTL_KEYBOARD_SET_INDICATORS       0x000B0008
#define IOCTL_KEYBOARD_QUERY_TYPEMATIC      0x000B0020
#define IOCTL_KEYBOARD_QUERY_INDICATORS     0x000B0040
#define IOCTL_KEYBOARD_QUERY_INDICATOR_TRANSLATION 0x000B0080
#define IOCTL_INTERNAL_MOUSE_CONNECT        0x000F0203
#define IOCTL_INTERNAL_MOUSE_DISCONNECT     0x000F0403
#define IOCTL_INTERNAL_MOUSE_ENABLE         0x000F0803
#define IOCTL_INTERNAL_MOUSE_DISABLE        0x000F1003
#define IOCTL_MOUSE_QUERY_ATTRIBUTES        0x000F0000
#define IOCTL_DISK_GET_DRIVE_GEOMETRY       0x00070000
#define IOCTL_DISK_GET_PARTITION_INFO       0x00074004
#define IOCTL_DISK_IS_WRITABLE              0x00070024
#define IOCTL_DISK_VERIFY                   0x00070014
#define IOCTL_DISK_FORMAT_TRACKS            0x0007C018
#define IOCTL_DISK_CHECK_VERIFY             0x00074800
#define IOCTL_DISK_GET_MEDIA_TYPES          0x00070C00

typedef struct _DISK_GEOMETRY { LARGE_INTEGER Cylinders; ULONG MediaType, TracksPerCylinder, SectorsPerTrack, BytesPerSector; } DISK_GEOMETRY, *PDISK_GEOMETRY;
_Static_assert(sizeof(DISK_GEOMETRY) == 24, "DISK_GEOMETRY is 24 bytes");
#define F3_1Pt44_512 2

#define FILE_DEVICE_DISK        0x07
#define FILE_DEVICE_KEYBOARD    0x0B
#define FILE_DEVICE_MOUSE       0x0F
#define FILE_REMOVABLE_MEDIA    0x01
#define FILE_FLOPPY_DISKETTE    0x04
#define DO_BUFFERED_IO          0x04
#define DO_DIRECT_IO            0x10
#define DO_DEVICE_INITIALIZING  0x80
#define IO_NO_INCREMENT         0

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS                 0x00000000
#endif
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER       0xC000000D
#endif
#ifndef STATUS_NO_SUCH_DEVICE
#define STATUS_NO_SUCH_DEVICE          0xC000000E
#endif
#ifndef STATUS_INVALID_DEVICE_REQUEST
#define STATUS_INVALID_DEVICE_REQUEST  0xC0000010
#endif
#ifndef STATUS_BUFFER_TOO_SMALL
#define STATUS_BUFFER_TOO_SMALL        0xC0000023
#endif
#ifndef STATUS_INSUFFICIENT_RESOURCES
#define STATUS_INSUFFICIENT_RESOURCES  0xC000009A
#endif
#ifndef STATUS_DEVICE_NOT_READY
#define STATUS_DEVICE_NOT_READY        0xC00000A3
#endif
#ifndef STATUS_IO_DEVICE_ERROR
#define STATUS_IO_DEVICE_ERROR         0xC0000185
#endif

/* ---- kernel and HAL routines this driver imports (adbport.imports) ---------------------- */
NTSTATUS IoCreateDevice(PDRIVER_OBJECT DriverObject, ULONG ExtensionSize, PUNICODE_STRING Name,
                        ULONG DeviceType, ULONG Characteristics, BOOLEAN Exclusive, PDEVICE_OBJECT *Out);
VOID IoDeleteDevice(PDEVICE_OBJECT DeviceObject);
VOID IofCompleteRequest(PIRP Irp, CHAR PriorityBoost);
PCONFIGURATION_INFORMATION IoGetConfigurationInformation(VOID);
VOID KeAcquireSpinLock(PKSPIN_LOCK Lock, PKIRQL OldIrql);
VOID KeReleaseSpinLock(PKSPIN_LOCK Lock, KIRQL OldIrql);
PVOID MmMapLockedPages(PMDL Mdl, CHAR AccessMode);
VOID RtlZeroMemory(PVOID Dst, ULONG Length);
VOID RtlMoveMemory(PVOID Dst, const VOID *Src, ULONG Length);
BOOLEAN HalPxiCommandAdb(UCHAR Command, PUCHAR Data, UCHAR Length, BOOLEAN Poll);
VOID HalPxiAdbSetCallback(PVOID CallbackDescriptor);
VOID HalPxiAdbAutopoll(USHORT Mask);
BOOLEAN HalAnsOemDiskQuery(PULONG PhysicalBase, PULONG Bytes);
VOID HalDisplayString(PCHAR String);
/* thunk.S: call a { entry, toc } descriptor with four arguments -- how the class service is reached */
ULONG AdbCallDesc4(PVOID Desc, ULONG a1, ULONG a2, ULONG a3, ULONG a4);

static inline PVOID MmGetSystemAddressForMdl(PMDL Mdl)
{
    if (Mdl->MdlFlags & (MDL_MAPPED_TO_SYSTEM_VA | MDL_SOURCE_IS_NONPAGED_POOL)) return Mdl->MappedSystemVa;
    return MmMapLockedPages(Mdl, KernelMode);
}

/* ---- this driver ------------------------------------------------------------------------- */
typedef enum { AdbDeviceKeyboard = 1, AdbDevicePointer = 2, AdbDeviceOemDisk = 3 } ADB_DEVICE_KIND;

typedef struct _ADB_EXTENSION {
    ADB_DEVICE_KIND Kind;
    PDEVICE_OBJECT Self;
    /* keyboard and pointer: who to hand input to, once the class driver has connected */
    CONNECT_DATA Connect;
    BOOLEAN Enabled;
    KSPIN_LOCK Lock;
    ULONG Packets, Delivered;           /* counters, for the trace */
    /* OEM disk */
    PUCHAR Image;
    ULONG ImageBytes;
} ADB_EXTENSION, *PADB_EXTENSION;

extern PADB_EXTENSION AdbKeyboard, AdbPointer;
VOID AdbLog(const char *fmt, ...);
/* per-device dispatch, each in its own file */
NTSTATUS AdbKeyboardControl(PADB_EXTENSION Ext, PIRP Irp, PIO_STACK_LOCATION Sp);
VOID AdbKeyboardPacket(PADB_EXTENSION Ext, const UCHAR *Data, ULONG Length);
NTSTATUS AdbPointerControl(PADB_EXTENSION Ext, PIRP Irp, PIO_STACK_LOCATION Sp);
VOID AdbPointerPacket(PADB_EXTENSION Ext, const UCHAR *Data, ULONG Length);
NTSTATUS AdbDiskInitialize(PADB_EXTENSION Ext);
NTSTATUS AdbDiskReadWrite(PADB_EXTENSION Ext, PIRP Irp, PIO_STACK_LOCATION Sp);
NTSTATUS AdbDiskControl(PADB_EXTENSION Ext, PIRP Irp, PIO_STACK_LOCATION Sp);
