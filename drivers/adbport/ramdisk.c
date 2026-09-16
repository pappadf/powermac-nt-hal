/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * Original work of this project.  Nothing here derives from leaked Windows NT source.
 *
 * ramdisk.c — \Device\Floppy0: the OEM disk image the boot floppy left in RAM, served as a
 * removable disk so Setup can copy the OEM files onto the hard disk under NT.
 *
 * The HAL found the image, checked it and fenced it off (src/oemdisk.c); this file maps it and
 * answers reads, writes and the disk IOCTLs setupdd and fastfat send a floppy.  Writes land in
 * RAM and vanish at reboot, which is the right behaviour for a disk whose only job is to be
 * read once. */
#include "adbport.h"

NTSTATUS AdbFinish(PIRP Irp, NTSTATUS Status, ULONG Information);

NTSTATUS AdbDiskInitialize(PADB_EXTENSION Ext)
{
    ULONG phys = 0, bytes = 0;
    if (!HalAnsOemDiskQuery(&phys, &bytes)) {
        AdbLog("adbport: the HAL reports no OEM disk image; \\Device\\Floppy0 not created");
        return STATUS_NO_SUCH_DEVICE;
    }
    PVOID va = MmMapIoSpace((PHYSICAL_ADDRESS)phys, bytes, TRUE);
    if (va == NULL) {
        AdbLog("adbport: MmMapIoSpace(%x, %x) failed", phys, bytes);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Ext->Image = va;
    Ext->ImageBytes = bytes;
    AdbLog("adbport: OEM disk %d bytes at %x mapped at %x", bytes, phys, (ULONG)va);
    return STATUS_SUCCESS;
}

static VOID AdbDiskGeometry(PADB_EXTENSION Ext, PDISK_GEOMETRY g)
{
    ULONG sectors = Ext->ImageBytes / OEMDISK_BLOCK;
    g->MediaType = F3_1Pt44_512;
    g->TracksPerCylinder = 2;
    g->SectorsPerTrack = 18;
    g->BytesPerSector = OEMDISK_BLOCK;
    g->Cylinders = sectors / (2 * 18);
}

NTSTATUS AdbDiskReadWrite(PADB_EXTENSION Ext, PIRP Irp, PIO_STACK_LOCATION Sp)
{
    ULONG length = Sp->Parameters.Read.Length;
    ULONG offset = Sp->Parameters.Read.ByteOffsetLow;
    if (Sp->Parameters.Read.ByteOffsetHigh != 0 || offset > Ext->ImageBytes || length > Ext->ImageBytes - offset ||
        (offset & (OEMDISK_BLOCK - 1)) || (length & (OEMDISK_BLOCK - 1)))
        return AdbFinish(Irp, STATUS_INVALID_PARAMETER, 0);
    if (length == 0) return AdbFinish(Irp, STATUS_SUCCESS, 0);
    if (Irp->MdlAddress == NULL) return AdbFinish(Irp, STATUS_INVALID_PARAMETER, 0);
    PUCHAR buf = MmGetSystemAddressForMdl(Irp->MdlAddress);
    if (buf == NULL) return AdbFinish(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    if (Sp->MajorFunction == IRP_MJ_READ) RtlMoveMemory(buf, Ext->Image + offset, length);
    else                                  RtlMoveMemory(Ext->Image + offset, buf, length);
    return AdbFinish(Irp, STATUS_SUCCESS, length);
}

NTSTATUS AdbDiskControl(PADB_EXTENSION Ext, PIRP Irp, PIO_STACK_LOCATION Sp)
{
    ULONG code = Sp->Parameters.DeviceIoControl.IoControlCode;
    ULONG outlen = Sp->Parameters.DeviceIoControl.OutputBufferLength;
    switch (code) {
    case IOCTL_DISK_GET_DRIVE_GEOMETRY:
    case IOCTL_DISK_GET_MEDIA_TYPES:
        if (outlen < sizeof(DISK_GEOMETRY)) return AdbFinish(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        AdbDiskGeometry(Ext, Irp->SystemBuffer);
        return AdbFinish(Irp, STATUS_SUCCESS, sizeof(DISK_GEOMETRY));
    case IOCTL_DISK_CHECK_VERIFY:
        /* the medium is present and has not changed; a caller that asked for the change count gets 0 */
        if (outlen >= sizeof(ULONG)) { *(PULONG)Irp->SystemBuffer = 0; return AdbFinish(Irp, STATUS_SUCCESS, sizeof(ULONG)); }
        return AdbFinish(Irp, STATUS_SUCCESS, 0);
    case IOCTL_DISK_IS_WRITABLE:
    case IOCTL_DISK_VERIFY:
        return AdbFinish(Irp, STATUS_SUCCESS, 0);
    case IOCTL_DISK_GET_PARTITION_INFO:     /* a floppy has none; floppy.sys says the same */
    case IOCTL_DISK_FORMAT_TRACKS:
        return AdbFinish(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
    return AdbFinish(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}
