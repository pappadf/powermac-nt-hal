/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * Original work of this project.  Nothing here derives from leaked Windows NT source.
 *
 * adbport.c — DriverEntry, the three devices, the dispatch table, and the ADB packet fan-out.
 *
 * The HAL owns the Cuda transport and the VIA interrupt.  It hands us each ADB packet from a
 * DPC, at DISPATCH_LEVEL, through the descriptor we register with HalPxiAdbSetCallback; we sort
 * packets by ADB address -- 2 is the keyboard, 3 the mouse -- and turn them into what kbdclass
 * and mouclass expect.  The OEM disk is a third device on the same driver object because Setup
 * loads OEM drivers through one prompt and every extra driver is another trip through it.
 *
 * STATUS: a draft, never loaded. */
#include "adbport.h"

PADB_EXTENSION AdbKeyboard, AdbPointer, AdbDisk;

/* ---- a small printf onto the HAL's console -------------------------------------------- */
static void put(char **p, char *end, char c) { if (*p < end) *(*p)++ = c; }
static void putnum(char **p, char *end, ULONG v, ULONG base, BOOLEAN neg)
{
    char tmp[12]; int n = 0;
    if (neg) put(p, end, '-');
    do { ULONG d = v % base; tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= base; } while (v);
    while (n) put(p, end, tmp[--n]);
}
VOID AdbLog(const char *fmt, ...)
{
    char buf[160], *p = buf, *end = buf + sizeof(buf) - 2;
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') { put(&p, end, *fmt); continue; }
        switch (*++fmt) {
        case 'd': { LONG v = __builtin_va_arg(ap, LONG); putnum(&p, end, v < 0 ? (ULONG)-v : (ULONG)v, 10, v < 0); break; }
        case 'x': putnum(&p, end, __builtin_va_arg(ap, ULONG), 16, FALSE); break;
        case 'c': put(&p, end, (char)__builtin_va_arg(ap, int)); break;
        case 's': { const char *s = __builtin_va_arg(ap, const char *); while (s && *s) put(&p, end, *s++); break; }
        case '%': put(&p, end, '%'); break;
        default:  put(&p, end, '%'); put(&p, end, *fmt); break;
        }
    }
    __builtin_va_end(ap);
    *p++ = '\n'; *p = 0;
    HalDisplayString(buf);
}

/* ---- IRP plumbing ----------------------------------------------------------------------- */
static NTSTATUS AdbComplete(PIRP Irp, NTSTATUS Status, ULONG Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IofCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static NTSTATUS AdbDispatchCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    return AdbComplete(Irp, STATUS_SUCCESS, 0);
}
DEFINE_DESC(AdbDispatchCreateClose);

static NTSTATUS AdbDispatchReadWrite(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PADB_EXTENSION ext = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    if (ext->Kind != AdbDeviceOemDisk) return AdbComplete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    return AdbDiskReadWrite(ext, Irp, sp);
}
DEFINE_DESC(AdbDispatchReadWrite);

static NTSTATUS AdbDispatchDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PADB_EXTENSION ext = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    switch (ext->Kind) {
    case AdbDeviceKeyboard: return AdbKeyboardControl(ext, Irp, sp);
    case AdbDevicePointer:  return AdbPointerControl(ext, Irp, sp);
    case AdbDeviceOemDisk:  return AdbDiskControl(ext, Irp, sp);
    }
    return AdbComplete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}
DEFINE_DESC(AdbDispatchDeviceControl);

static NTSTATUS AdbDispatchOther(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    return AdbComplete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}
DEFINE_DESC(AdbDispatchOther);

/* Shared by the per-device control routines: the whole IRP completion in one place. */
NTSTATUS AdbFinish(PIRP Irp, NTSTATUS Status, ULONG Information) { return AdbComplete(Irp, Status, Information); }

/* ---- the ADB packet callback, from the HAL's DPC ----------------------------------------
 *
 * Arguments are what cuda.c's HalpAdbDeliver passes: the Cuda status byte, the ADB command
 * byte the packet answers (address in the top nibble, command bits 3:2 -- 11 is Talk -- and the
 * register in bits 1:0), then the register bytes.  Only Talk Register 0 carries input. */
static VOID AdbPacket(ULONG Status, ULONG Command, PUCHAR Data, ULONG Length)
{
    UNREFERENCED_PARAMETER(Status);
    ULONG address = (Command >> 4) & 0xF, reg = Command & 3, cmd = (Command >> 2) & 3;
    if (cmd != 3 || reg != 0 || Length == 0) return;
    if (address == 2 && AdbKeyboard) AdbKeyboardPacket(AdbKeyboard, Data, Length);
    else if (address == 3 && AdbPointer) AdbPointerPacket(AdbPointer, Data, Length);
}
DEFINE_DESC(AdbPacket);

/* ---- DriverEntry -------------------------------------------------------------------------- */
#define L16(s) ((const WCHAR *)(u ## s))

static NTSTATUS AdbCreate(PDRIVER_OBJECT DriverObject, const WCHAR *Name, ULONG Type, ULONG Characteristics,
                          ADB_DEVICE_KIND Kind, PADB_EXTENSION *Out)
{
    UNICODE_STRING name;
    PDEVICE_OBJECT dev = NULL;
    RtlInitUnicodeString(&name, Name);
    NTSTATUS st = IoCreateDevice(DriverObject, sizeof(ADB_EXTENSION), &name, Type, Characteristics, FALSE, &dev);
    if (st != STATUS_SUCCESS || dev == NULL) {
        AdbLog("adbport: IoCreateDevice failed %x", st);
        *Out = NULL;
        return st ? st : STATUS_INSUFFICIENT_RESOURCES;
    }
    PADB_EXTENSION ext = dev->DeviceExtension;
    RtlZeroMemory(ext, sizeof(*ext));
    ext->Kind = Kind;
    ext->Self = dev;
    KeInitializeSpinLock(&ext->Lock);
    *Out = ext;
    return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);
    AdbLog("adbport: ADB keyboard, mouse and OEM disk for the Apple Network Server (powermac-nt-hal)");

    for (ULONG i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) DriverObject->MajorFunction[i] = DESC(AdbDispatchOther);
    DriverObject->MajorFunction[IRP_MJ_CREATE] = DESC(AdbDispatchCreateClose);
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = DESC(AdbDispatchCreateClose);
    DriverObject->MajorFunction[IRP_MJ_READ] = DESC(AdbDispatchReadWrite);
    DriverObject->MajorFunction[IRP_MJ_WRITE] = DESC(AdbDispatchReadWrite);
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DESC(AdbDispatchDeviceControl);
    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = DESC(AdbDispatchDeviceControl);

    NTSTATUS st = AdbCreate(DriverObject, L16("\\Device\\KeyboardPort0"), FILE_DEVICE_KEYBOARD, 0, AdbDeviceKeyboard, &AdbKeyboard);
    if (st != STATUS_SUCCESS) return st;
    st = AdbCreate(DriverObject, L16("\\Device\\PointerPort0"), FILE_DEVICE_MOUSE, 0, AdbDevicePointer, &AdbPointer);
    if (st != STATUS_SUCCESS) return st;

    /* The OEM disk is optional: without one the keyboard still works, and the HAL has already
     * said on its console why there is none. */
    st = AdbCreate(DriverObject, L16("\\Device\\Floppy0"), FILE_DEVICE_DISK,
                   FILE_REMOVABLE_MEDIA | FILE_FLOPPY_DISKETTE, AdbDeviceOemDisk, &AdbDisk);
    if (st == STATUS_SUCCESS) {
        AdbDisk->Self->Flags |= DO_DIRECT_IO;
        AdbDisk->Self->SectorSize = OEMDISK_BLOCK;
        st = AdbDiskInitialize(AdbDisk);
        if (st != STATUS_SUCCESS) {
            IoDeleteDevice(AdbDisk->Self);
            AdbDisk = NULL;
        } else {
            /* IoAssignDriveLetters -- ours, in the HAL -- counts floppies from here to hand out A:. */
            IoGetConfigurationInformation()->FloppyCount++;
        }
    }

    HalPxiAdbSetCallback(DESC(AdbPacket));
    HalPxiAdbAutopoll(0xFFFF);
    AdbKeyboard->Self->Flags &= ~DO_DEVICE_INITIALIZING;
    AdbPointer->Self->Flags &= ~DO_DEVICE_INITIALIZING;
    if (AdbDisk) AdbDisk->Self->Flags &= ~DO_DEVICE_INITIALIZING;
    AdbLog("adbport: up; keyboard, pointer%s", AdbDisk ? ", OEM disk as \\Device\\Floppy0" : ", no OEM disk");
    return STATUS_SUCCESS;
}
