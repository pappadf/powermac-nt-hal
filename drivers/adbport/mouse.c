/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * Original work of this project.  The ADB mouse register-0 format is from Apple's ADB
 * documentation and the emulator's adb.c.  Nothing is copied from another driver.
 *
 * mouse.c — \Device\PointerPort0: ADB mouse packets in, MOUSE_INPUT_DATA out.
 *
 * Talk Register 0 from an ADB mouse is two bytes: (button-up << 7) | dy, then (button2-up << 7)
 * | dx, each delta a 7-bit two's-complement count with positive meaning right and down -- the
 * same sense NT uses.  A one-button mouse reports its missing second button as always up. */
#include "adbport.h"

NTSTATUS AdbFinish(PIRP Irp, NTSTATUS Status, ULONG Information);

static ULONG AdbButtons;   /* bit 0 = left down, bit 1 = right down; to turn levels into edges */

static LONG Sign7(UCHAR v) { v &= 0x7F; return (v & 0x40) ? (LONG)v - 0x80 : (LONG)v; }

VOID AdbPointerPacket(PADB_EXTENSION Ext, const UCHAR *Data, ULONG Length)
{
    Ext->Packets++;
    if (Length < 2) return;
    ULONG now = ((Data[0] & 0x80) ? 0 : 1) | ((Data[1] & 0x80) ? 0 : 2);
    MOUSE_INPUT_DATA mid;
    mid.UnitId = 0;
    mid.Flags = MOUSE_MOVE_RELATIVE;
    mid.ButtonFlags = 0;
    if ((now & 1) && !(AdbButtons & 1)) mid.ButtonFlags |= MOUSE_LEFT_BUTTON_DOWN;
    if (!(now & 1) && (AdbButtons & 1)) mid.ButtonFlags |= MOUSE_LEFT_BUTTON_UP;
    if ((now & 2) && !(AdbButtons & 2)) mid.ButtonFlags |= MOUSE_RIGHT_BUTTON_DOWN;
    if (!(now & 2) && (AdbButtons & 2)) mid.ButtonFlags |= MOUSE_RIGHT_BUTTON_UP;
    AdbButtons = now;
    mid.ButtonData = 0;
    mid.RawButtons = now;
    mid.LastY = Sign7(Data[0]);
    mid.LastX = Sign7(Data[1]);
    mid.ExtraInformation = 0;
    if (mid.ButtonFlags == 0 && mid.LastX == 0 && mid.LastY == 0) return;
    if (!Ext->Enabled || Ext->Connect.ClassService == NULL) return;
    ULONG consumed = 0;
    AdbCallDesc4(Ext->Connect.ClassService, (ULONG)Ext->Connect.ClassDeviceObject,
                 (ULONG)&mid, (ULONG)(&mid + 1), (ULONG)&consumed);
    Ext->Delivered++;
}

NTSTATUS AdbPointerControl(PADB_EXTENSION Ext, PIRP Irp, PIO_STACK_LOCATION Sp)
{
    ULONG code = Sp->Parameters.DeviceIoControl.IoControlCode;
    ULONG inlen = Sp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outlen = Sp->Parameters.DeviceIoControl.OutputBufferLength;
    KIRQL old;
    switch (code) {
    case IOCTL_INTERNAL_MOUSE_CONNECT: {
        if (inlen < sizeof(CONNECT_DATA)) return AdbFinish(Irp, STATUS_INVALID_PARAMETER, 0);
        PCONNECT_DATA cd = Sp->Parameters.DeviceIoControl.Type3InputBuffer;
        KeAcquireSpinLock(&Ext->Lock, &old);
        Ext->Connect = *cd;
        KeReleaseSpinLock(&Ext->Lock, old);
        AdbLog("adbport: mouse class connected, service %x", (ULONG)cd->ClassService);
        return AdbFinish(Irp, STATUS_SUCCESS, 0);
    }
    case IOCTL_INTERNAL_MOUSE_DISCONNECT:
        KeAcquireSpinLock(&Ext->Lock, &old);
        Ext->Connect.ClassService = NULL; Ext->Connect.ClassDeviceObject = NULL; Ext->Enabled = FALSE;
        KeReleaseSpinLock(&Ext->Lock, old);
        return AdbFinish(Irp, STATUS_SUCCESS, 0);
    case IOCTL_INTERNAL_MOUSE_ENABLE:
        Ext->Enabled = TRUE;
        return AdbFinish(Irp, STATUS_SUCCESS, 0);
    case IOCTL_INTERNAL_MOUSE_DISABLE:
        Ext->Enabled = FALSE;
        return AdbFinish(Irp, STATUS_SUCCESS, 0);
    case IOCTL_MOUSE_QUERY_ATTRIBUTES: {
        if (outlen < sizeof(MOUSE_ATTRIBUTES)) return AdbFinish(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        MOUSE_ATTRIBUTES *a = Irp->SystemBuffer;
        RtlZeroMemory(a, sizeof(*a));
        a->MouseIdentifier = 1;         /* "i8042" hardware id: what mouclass expects of a port device */
        a->NumberOfButtons = 2;
        a->SampleRate = 60;
        a->InputDataQueueLength = 100;
        return AdbFinish(Irp, STATUS_SUCCESS, sizeof(*a));
    }
    }
    return AdbFinish(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}
