/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * Original work of this project.  The ADB register-0 format and the Extended Keyboard II key
 * codes are from Apple's ADB documentation and the emulator's own adb.c; the PC scancodes are
 * the AT keyboard's scan code set 1.  Nothing is copied from another driver.
 *
 * kbd.c — \Device\KeyboardPort0: ADB keyboard packets in, KEYBOARD_INPUT_DATA out.
 *
 * kbdclass connects with IOCTL_INTERNAL_KEYBOARD_CONNECT and hands us its device and a class
 * service routine; from then on every key becomes one KEYBOARD_INPUT_DATA -- a set-1 make code,
 * KEY_BREAK on release, KEY_E0 for the keys that carry the 0xE0 prefix on a PC -- delivered to
 * that routine at DISPATCH_LEVEL, which is where the HAL's DPC already has us. */
#include "adbport.h"

NTSTATUS AdbFinish(PIRP Irp, NTSTATUS Status, ULONG Information);

#define E0 0x100
/* Apple Extended Keyboard II, as the keys arrive on the wire.  Two things about the arrows: the
 * bus sends 0x3B..0x3E for them, and 0x7B..0x7E -- which Apple's *virtual* key-code table also
 * calls arrows -- are the right-hand Shift, Option and Control.  The emulator's
 * keyboard.down("down") sends the virtual 0x7D, so on it the arrows are 0x3D et al.; both rows
 * below are correct for what each code means on the wire. */
static const USHORT AdbToSet1[128] = {
    /* 00 */ 0x1E, 0x1F, 0x20, 0x21, 0x23, 0x22, 0x2C, 0x2D,   /* A S D F H G Z X */
    /* 08 */ 0x2E, 0x2F, 0x56, 0x30, 0x10, 0x11, 0x12, 0x13,   /* C V § B Q W E R */
    /* 10 */ 0x15, 0x14, 0x02, 0x03, 0x04, 0x05, 0x07, 0x06,   /* Y T 1 2 3 4 6 5 */
    /* 18 */ 0x0D, 0x0A, 0x08, 0x0C, 0x09, 0x0B, 0x1B, 0x18,   /* = 9 7 - 8 0 ] O */
    /* 20 */ 0x16, 0x1A, 0x17, 0x19, 0x1C, 0x26, 0x24, 0x28,   /* U [ I P Return L J ' */
    /* 28 */ 0x25, 0x27, 0x2B, 0x33, 0x35, 0x31, 0x32, 0x34,   /* K ; \ , / N M . */
    /* 30 */ 0x0F, 0x39, 0x29, 0x0E, E0|0x1C, 0x01, 0x1D, E0|0x5B,  /* Tab Space ` Delete KpEnter Esc Control Command */
    /* 38 */ 0x2A, 0x3A, 0x38, E0|0x4B, E0|0x4D, E0|0x50, E0|0x48, 0,  /* Shift CapsLock Option Left Right Down Up (Fn) */
    /* 40 */ 0, 0x53, 0, 0x37, 0, 0x4E, 0, 0x45,               /* - Kp. - Kp* - Kp+ - Clear/NumLock */
    /* 48 */ 0, 0, 0, E0|0x35, E0|0x1C, 0, 0x4A, 0,            /* - - - Kp/ KpEnter - Kp- - */
    /* 50 */ 0, 0x0D, 0x52, 0x4F, 0x50, 0x51, 0x4B, 0x4C,      /* - Kp= Kp0 Kp1 Kp2 Kp3 Kp4 Kp5 */
    /* 58 */ 0x4D, 0x47, 0, 0x48, 0x49, 0, 0, 0,               /* Kp6 Kp7 - Kp8 Kp9 - - - */
    /* 60 */ 0x3F, 0x40, 0x41, 0x3D, 0x42, 0x43, 0, 0x57,      /* F5 F6 F7 F3 F8 F9 - F11 */
    /* 68 */ 0, E0|0x37, 0, 0x46, 0, 0x44, 0, 0x58,            /* - F13/PrtSc - F14/ScrLk - F10 - F12 */
    /* 70 */ 0, 0, E0|0x52, E0|0x47, E0|0x49, E0|0x53, 0x3E, E0|0x4F,  /* - F15 Help/Ins Home PgUp FwdDel F4 End */
    /* 78 */ 0x3C, E0|0x51, 0x3B, 0x36, E0|0x38, E0|0x1D, 0, 0,        /* F2 PgDn F1 RShift ROption RControl - Power */
};

static VOID AdbKeyboardDeliver(PADB_EXTENSION Ext, USHORT make, BOOLEAN release)
{
    KEYBOARD_INPUT_DATA kid;
    kid.UnitId = 0;
    kid.MakeCode = make & 0xFF;
    kid.Flags = (USHORT)((release ? KEY_BREAK : KEY_MAKE) | ((make & E0) ? KEY_E0 : 0));
    kid.Reserved = 0;
    kid.ExtraInformation = 0;
    if (!Ext->Enabled || Ext->Connect.ClassService == NULL) return;
    ULONG consumed = 0;
    AdbCallDesc4(Ext->Connect.ClassService, (ULONG)Ext->Connect.ClassDeviceObject,
                 (ULONG)&kid, (ULONG)(&kid + 1), (ULONG)&consumed);
    Ext->Delivered++;
}

/* Talk Register 0 from a keyboard: two bytes, each (release << 7) | code, 0xFF meaning no key.
 * The power key reports 0x7F in both bytes and is not a key NT has. */
VOID AdbKeyboardPacket(PADB_EXTENSION Ext, const UCHAR *Data, ULONG Length)
{
    Ext->Packets++;
    for (ULONG i = 0; i < Length && i < 2; i++) {
        UCHAR b = Data[i];
        if (b == 0xFF) continue;
        UCHAR code = b & 0x7F;
        if (code == 0x7F) continue;
        USHORT make = AdbToSet1[code];
        if (make == 0) continue;
        AdbKeyboardDeliver(Ext, make, (b & 0x80) != 0);
    }
}

NTSTATUS AdbKeyboardControl(PADB_EXTENSION Ext, PIRP Irp, PIO_STACK_LOCATION Sp)
{
    ULONG code = Sp->Parameters.DeviceIoControl.IoControlCode;
    ULONG inlen = Sp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outlen = Sp->Parameters.DeviceIoControl.OutputBufferLength;
    KIRQL old;
    switch (code) {
    case IOCTL_INTERNAL_KEYBOARD_CONNECT: {
        if (inlen < sizeof(CONNECT_DATA)) return AdbFinish(Irp, STATUS_INVALID_PARAMETER, 0);
        PCONNECT_DATA cd = Sp->Parameters.DeviceIoControl.Type3InputBuffer;
        KeAcquireSpinLock(&Ext->Lock, &old);
        Ext->Connect = *cd;
        KeReleaseSpinLock(&Ext->Lock, old);
        AdbLog("adbport: keyboard class connected, service %x", (ULONG)cd->ClassService);
        return AdbFinish(Irp, STATUS_SUCCESS, 0);
    }
    case IOCTL_INTERNAL_KEYBOARD_DISCONNECT:
        KeAcquireSpinLock(&Ext->Lock, &old);
        Ext->Connect.ClassService = NULL; Ext->Connect.ClassDeviceObject = NULL; Ext->Enabled = FALSE;
        KeReleaseSpinLock(&Ext->Lock, old);
        return AdbFinish(Irp, STATUS_SUCCESS, 0);
    case IOCTL_INTERNAL_KEYBOARD_ENABLE:
        Ext->Enabled = TRUE;
        return AdbFinish(Irp, STATUS_SUCCESS, 0);
    case IOCTL_INTERNAL_KEYBOARD_DISABLE:
        Ext->Enabled = FALSE;
        return AdbFinish(Irp, STATUS_SUCCESS, 0);
    case IOCTL_KEYBOARD_QUERY_ATTRIBUTES: {
        if (outlen < sizeof(KEYBOARD_ATTRIBUTES)) return AdbFinish(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        KEYBOARD_ATTRIBUTES *a = Irp->SystemBuffer;
        RtlZeroMemory(a, sizeof(*a));
        a->KeyboardIdentifierType = 4;          /* the enhanced 101/102-key layout NT's tables assume */
        a->KeyboardIdentifierSubtype = 0;
        a->KeyboardMode = 1;                    /* scan code set 1 */
        a->NumberOfFunctionKeys = 15;
        a->NumberOfIndicators = 3;
        a->NumberOfKeysTotal = 105;
        a->InputDataQueueLength = 100;
        a->KeyRepeatMinimum.Rate = 2;  a->KeyRepeatMinimum.Delay = 250;
        a->KeyRepeatMaximum.Rate = 30; a->KeyRepeatMaximum.Delay = 1000;
        return AdbFinish(Irp, STATUS_SUCCESS, sizeof(*a));
    }
    case IOCTL_KEYBOARD_QUERY_TYPEMATIC: {
        struct { USHORT UnitId, Rate, Delay; } *t = Irp->SystemBuffer;
        if (outlen < sizeof(*t)) return AdbFinish(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        t->UnitId = 0; t->Rate = 30; t->Delay = 250;
        return AdbFinish(Irp, STATUS_SUCCESS, sizeof(*t));
    }
    case IOCTL_KEYBOARD_QUERY_INDICATORS: {
        KEYBOARD_INDICATOR_PARAMETERS *p = Irp->SystemBuffer;
        if (outlen < sizeof(*p)) return AdbFinish(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        p->UnitId = 0; p->LedFlags = 0;
        return AdbFinish(Irp, STATUS_SUCCESS, sizeof(*p));
    }
    case IOCTL_KEYBOARD_QUERY_INDICATOR_TRANSLATION: {
        /* which make codes toggle which LEDs: NumLock, CapsLock, ScrollLock */
        struct { USHORT Count; struct { USHORT MakeCode, IndicatorFlags; } L[3]; } *t = Irp->SystemBuffer;
        if (outlen < sizeof(*t)) return AdbFinish(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        t->Count = 3;
        t->L[0].MakeCode = 0x45; t->L[0].IndicatorFlags = 0x02;
        t->L[1].MakeCode = 0x3A; t->L[1].IndicatorFlags = 0x04;
        t->L[2].MakeCode = 0x46; t->L[2].IndicatorFlags = 0x01;
        return AdbFinish(Irp, STATUS_SUCCESS, sizeof(*t));
    }
    case IOCTL_KEYBOARD_SET_TYPEMATIC:
    case IOCTL_KEYBOARD_SET_INDICATORS:
        /* An ADB keyboard's LEDs are reachable through Listen Register 2; not worth a Cuda
         * round trip from a driver that exists to get through Setup.  Accepted and ignored. */
        return AdbFinish(Irp, STATUS_SUCCESS, 0);
    }
    return AdbFinish(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}
