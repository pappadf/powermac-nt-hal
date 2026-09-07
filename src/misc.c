/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * UPSTREAM: Wack0/entii-for-workcubes, halartx/source/ (GPL-2.0, commit c9b041da).
 *   Which of these routines may be stubs comes from reading `arccalls.c`
 *   and `kd.c`.
 *   Rewritten from reading: no lines were copied, and the code here was written fresh
 *   for Grand Central, Bandit and the ESCC.  See PROVENANCE.md.
 *
 * misc.c — firmware environment, real-time clock, beep, BIOS, kernel debugger port, partition
 * table forwarders, and the C runtime bits the compiler expects. */
#include "hal.h"

PUCHAR KdComPortInUse = NULL;

/* ---- the ARC firmware environment ------------------------------------------------------------
 * On an ARC machine these two read and write the firmware's NVRAM, and NT treats them as the
 * system environment: `NtQuerySystemEnvironmentValue` lands here.  Text-mode Setup asks for
 * SYSTEMPARTITION before it will offer to install anything, and writes OSLOADER, OSLOADPARTITION,
 * OSLOADFILENAME and LOADIDENTIFIER back at the end of the copy phase.
 *
 * The Network Server has no ARC NVRAM to back this with — its firmware is Open Firmware, whose
 * own `nvram` partitions are a different format with a different owner — so the store here is
 * plain memory, in the same "NAME=value\0…\0\0" shape the ARC spec uses.  It does not survive a
 * reboot; nothing on this machine boots from the ARC environment anyway, because Open Firmware's
 * boot script is what starts the veneer.  Persisting it belongs with whatever eventually replaces
 * the veneer.
 */
#define HALP_ENV_SIZE 2048
static CHAR HalpEnvironment[HALP_ENV_SIZE];

static BOOLEAN HalpEnvNameIs(const CHAR *entry, const CHAR *name)
{
    /* case-insensitive up to the entry's '=' — ARC names are conventionally upper case, but
     * nothing guarantees the caller spells them that way */
    while (*name && *entry && *entry != '=') {
        CHAR a = *entry++, b = *name++;
        if (a >= 'a' && a <= 'z') a = (CHAR)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (CHAR)(b - 'a' + 'A');
        if (a != b) return FALSE;
    }
    return (BOOLEAN)(*name == 0 && *entry == '=');
}

/* Bytes of the store in use, its final terminator included.  Every walk below is bounded by
 * this rather than by finding a NUL pair, so a store that somehow lost its terminator reports
 * the whole array instead of walking off the end of it. */
static ULONG HalpEnvUsed(VOID)
{
    ULONG i = 0;
    while (i < HALP_ENV_SIZE) {
        if (HalpEnvironment[i] == 0) return i + 1;
        while (i < HALP_ENV_SIZE && HalpEnvironment[i]) i++;
        i++;                                    /* step over this entry's own NUL */
    }
    return HALP_ENV_SIZE;
}

static CHAR *HalpEnvFind(const CHAR *name)
{
    ULONG used = HalpEnvUsed();
    for (ULONG i = 0; i + 1 < used; ) {
        if (HalpEnvNameIs(&HalpEnvironment[i], name)) return &HalpEnvironment[i];
        while (i < used && HalpEnvironment[i]) i++;
        i++;
    }
    return NULL;
}

/* Take one entry out, close the gap, and *clear what the move vacated*.  The clearing is the
 * point: an earlier version shifted bytes until it saw a NUL pair and left the tail alone, so
 * every re-set of a variable left stale structure behind — which the next removal then walked
 * into, scrambling the store (and, with the wrong tail, running past the end of it). */
static VOID HalpEnvRemove(CHAR *entry)
{
    ULONG used = HalpEnvUsed();
    ULONG at = (ULONG)(entry - HalpEnvironment);
    ULONG next = at;
    while (next < used && HalpEnvironment[next]) next++;
    next++;                                     /* first byte of whatever followed */
    if (next > used) return;
    ULONG gap = next - at, tail = used - next;
    for (ULONG i = 0; i < tail; i++) HalpEnvironment[at + i] = HalpEnvironment[next + i];
    for (ULONG i = used - gap; i < used; i++) HalpEnvironment[i] = 0;
}

static ULONG HalpStrLen(const CHAR *s) { ULONG n = 0; while (s[n]) n++; return n; }

ARC_STATUS HalGetEnvironmentVariable(PCHAR Variable, USHORT Length, PCHAR Buffer)
{
    CHAR *entry = Variable ? HalpEnvFind(Variable) : NULL;
    CHAR *value;
    USHORT i = 0;

    if (entry == NULL) {
        HalpPrint("HAL: env get '%s' -> ENOENT\n", Variable ? Variable : "(null)");
        return ENOENT;
    }
    for (value = entry; *value != '='; value++) { }
    value++;
    while (i + 1 < Length && value[i]) { Buffer[i] = value[i]; i++; }
    if (Length) Buffer[i] = 0;
    HalpPrint("HAL: env get '%s' -> '%s'\n", Variable, Buffer);
    return value[i] ? ENOMEM : ESUCCESS;
}

ARC_STATUS HalSetEnvironmentVariable(PCHAR Variable, PCHAR Value)
{
    CHAR *entry, *p;
    ULONG need, at;

    if (Variable == NULL || Value == NULL) return EINVAL;
    HalpPrint("HAL: env set '%s' = '%s'\n", Variable, Value);

    entry = HalpEnvFind(Variable);
    if (entry) HalpEnvRemove(entry);
    if (Value[0] == 0) return ESUCCESS;         /* setting a variable empty deletes it */

    at = HalpEnvUsed() - 1;                     /* the final terminator; the new entry lands on it */
    need = HalpStrLen(Variable) + 1 + HalpStrLen(Value) + 2;   /* NAME=VALUE\0 and the final \0 */
    if (at + need > HALP_ENV_SIZE) return ENOSPC;
    p = &HalpEnvironment[at];
    for (const CHAR *q = Variable; *q; ) *p++ = *q++;
    *p++ = '=';
    for (const CHAR *q = Value; *q; ) *p++ = *q++;
    *p++ = 0;
    *p = 0;
    return ESUCCESS;
}

/* Seed the environment with what the firmware vendor's configuration program would have written.
 * SYSTEMPARTITION is the one Setup will not start without: it names the FAT partition OSLOADER
 * goes in, and on real hardware ARCINST.EXE puts it in NVRAM when it creates that partition.
 * Derive it instead from the loader's own ARC disk list — the first disk whose partition table
 * SETUPLDR could read is the one an ARCINST run here would have picked. */
VOID HalpSeedEnvironment(PVOID ArcDiskInformation)
{
    PLIST_ENTRY head = (PLIST_ENTRY)ArcDiskInformation;
    CHAR path[80];

    if (head == NULL || HalpEnvFind("SYSTEMPARTITION") != NULL) return;
    for (PLIST_ENTRY e = head->Flink; e && e != head; e = e->Flink) {
        PARC_DISK_SIGNATURE s = (PARC_DISK_SIGNATURE)e;
        ULONG n = 0;
        if (!s->ValidPartitionTable || s->ArcName == NULL) continue;
        while (s->ArcName[n] && n < sizeof(path) - 16) { path[n] = s->ArcName[n]; n++; }
        for (const CHAR *q = "partition(1)"; *q; ) path[n++] = *q++;
        path[n] = 0;
        HalSetEnvironmentVariable("SYSTEMPARTITION", path);
        return;
    }
}

/* Until the Cuda RTC is read, report a fixed moment; NT wants a sane year. */
BOOLEAN HalQueryRealTimeClock(PTIME_FIELDS TimeFields)
{
    TimeFields->Year = 2026; TimeFields->Month = 9; TimeFields->Day = 6;
    TimeFields->Hour = 12; TimeFields->Minute = 0; TimeFields->Second = 0;
    TimeFields->Milliseconds = 0; TimeFields->Weekday = 0;
    return TRUE;
}

BOOLEAN HalSetRealTimeClock(PTIME_FIELDS TimeFields)
{
    UNREFERENCED_PARAMETER(TimeFields);
    return FALSE;
}

BOOLEAN HalMakeBeep(ULONG Frequency)
{
    UNREFERENCED_PARAMETER(Frequency);
    return FALSE;
}

BOOLEAN HalCallBios(ULONG Command, PULONG Eax, PULONG Ebx, PULONG Ecx, PULONG Edx, PULONG Esi, PULONG Edi, PULONG Ebp)
{
    UNREFERENCED_PARAMETER(Command); UNREFERENCED_PARAMETER(Eax); UNREFERENCED_PARAMETER(Ebx); UNREFERENCED_PARAMETER(Ecx);
    UNREFERENCED_PARAMETER(Edx); UNREFERENCED_PARAMETER(Esi); UNREFERENCED_PARAMETER(Edi); UNREFERENCED_PARAMETER(Ebp);
    return FALSE;
}

/* kernel debugger over ttyb would go here; report no debugger */
#define CP_GET_SUCCESS 0
#define CP_GET_NODATA 1
#define CP_GET_ERROR 2
BOOLEAN KdPortInitialize(PDEBUG_PARAMETERS Parameters, PLOADER_PARAMETER_BLOCK LoaderBlock, BOOLEAN Initialize)
{
    UNREFERENCED_PARAMETER(Parameters); UNREFERENCED_PARAMETER(LoaderBlock); UNREFERENCED_PARAMETER(Initialize);
    return FALSE;
}
ULONG KdPortGetByte(PUCHAR Input) { UNREFERENCED_PARAMETER(Input); return CP_GET_NODATA; }
ULONG KdPortPollByte(PUCHAR Input) { UNREFERENCED_PARAMETER(Input); return CP_GET_NODATA; }
VOID KdPortPutByte(UCHAR Output) { UNREFERENCED_PARAMETER(Output); }
VOID KdPortRestore(VOID) { }
VOID KdPortSave(VOID) { }

/* Partition support: NT 4 keeps these in the HAL.  Until the MBR/Apple partition map reader is
 * written, report no partitions; text-mode Setup then sees disks without partitions. */

/* Drive letters.  This is not cosmetic: `IoAssignDriveLetters` is where the NT device path the
 * loader booted from becomes a *DOS* path, and the kernel takes the rewritten string as the
 * system root.  Everything downstream that names a file with a rooted path — Setup handing
 * `\PPC\KBDUS.DLL` to `LdrLoadDll`, which resolves a rooted path against the current drive —
 * depends on there being a drive letter to resolve against.  With this a stub, that load fails
 * with STATUS_DLL_NOT_FOUND without the file system ever being asked.
 *
 * The assignment follows NT's order — floppies from A:, hard-disk partitions from C:, then
 * CD-ROMs — with one partition assumed per disk, because reading partition tables is
 * `IoReadPartitionTable`'s job and that is still a stub.  The boot device gets whichever letter
 * its own name lands on, and the system path is rewritten to use it. */
/* WCHAR is 16-bit here and the compiler's L"" is 32-bit, so the name is spelled out. */
static WCHAR HalpLinkName[] = { '\\','D','o','s','D','e','v','i','c','e','s','\\','A',':',0 };
static WCHAR HalpTargetName[64];

static VOID HalpMakeDosDevice(UCHAR letter, const char *target)
{
    UNICODE_STRING link, dev;
    ULONG i = 0;
    HalpLinkName[12] = letter;
    for (; target[i] && i < 63; i++) HalpTargetName[i] = (WCHAR)(UCHAR)target[i];
    HalpTargetName[i] = 0;
    RtlInitUnicodeString(&link, HalpLinkName);
    RtlInitUnicodeString(&dev, HalpTargetName);
    IoCreateSymbolicLink(&link, &dev);
}

/* sprintf is not available here; this is the only formatting the HAL needs. */
static ULONG HalpFormatDevice(char *out, const char *prefix, ULONG n, const char *suffix)
{
    ULONG i = 0;
    while (*prefix) out[i++] = *prefix++;
    if (n >= 10) out[i++] = (char)('0' + n / 10);
    out[i++] = (char)('0' + n % 10);
    while (*suffix) out[i++] = *suffix++;
    out[i] = 0;
    return i;
}

/* Does the counted string `b` begin the NUL-terminated `a`?  (Used the other way round too.) */
static BOOLEAN HalpPrefix(const char *s, PSTRING prefix)
{
    for (ULONG i = 0; i < prefix->Length; i++) if (s[i] != prefix->Buffer[i]) return FALSE;
    return TRUE;
}

static BOOLEAN HalpSameDevice(const char *a, PSTRING b)
{
    ULONG i = 0;
    for (; i < b->Length; i++) if (a[i] != b->Buffer[i]) return FALSE;
    return a[i] == 0;
}

VOID IoAssignDriveLetters(PLOADER_PARAMETER_BLOCK LoaderBlock, PSTRING NtDeviceName, PUCHAR NtSystemPath, PSTRING NtSystemPathString)
{
    UNREFERENCED_PARAMETER(LoaderBlock);
    PCONFIGURATION_INFORMATION cfg = IoGetConfigurationInformation();
    char name[64];
    UCHAR bootLetter = 0;

    for (ULONG i = 0; i < cfg->FloppyCount && i < 2; i++) {
        HalpFormatDevice(name, "\\Device\\Floppy", i, "");
        HalpMakeDosDevice((UCHAR)('A' + i), name);
        if (HalpSameDevice(name, NtDeviceName)) bootLetter = (UCHAR)('A' + i);
    }
    UCHAR letter = 'C';
    for (ULONG i = 0; i < cfg->DiskCount && letter <= 'Z'; i++) {
        HalpFormatDevice(name, "\\Device\\Harddisk", i, "\\Partition1");
        HalpMakeDosDevice(letter, name);
        if (HalpSameDevice(name, NtDeviceName)) bootLetter = letter;
        letter++;
    }
    for (ULONG i = 0; i < cfg->CdRomCount && letter <= 'Z'; i++) {
        HalpFormatDevice(name, "\\Device\\CdRom", i, "");
        HalpMakeDosDevice(letter, name);
        if (HalpSameDevice(name, NtDeviceName)) bootLetter = letter;
        letter++;
    }

    /* NtDeviceName is a counted STRING, not NUL-terminated; copy it out to print it. */
    char shown[64];
    ULONG shownLen = NtDeviceName->Length < 63 ? NtDeviceName->Length : 63;
    for (ULONG i = 0; i < shownLen; i++) shown[i] = NtDeviceName->Buffer[i];
    shown[shownLen] = 0;
    HalpPrint("HAL: IoAssignDriveLetters: boot device '%s' -> %c:, %d floppy %d disk %d cdrom\n",
              shown, bootLetter ? bootLetter : '?', cfg->FloppyCount, cfg->DiskCount, cfg->CdRomCount);

    /* Rewrite the system path so it names the drive letter instead of the device.  The kernel
     * converts whatever we leave behind back to Unicode and uses it as the system root. */
    char in[80];
    ULONG inLen = NtSystemPathString->Length < 79 ? NtSystemPathString->Length : 79;
    for (ULONG i = 0; i < inLen; i++) in[i] = NtSystemPathString->Buffer[i];
    in[inLen] = 0;
    HalpPrint("HAL: system path in '%s' (len %d max %d)\n", in, NtSystemPathString->Length,
              NtSystemPathString->MaximumLength);
    if (!bootLetter) return;

    /* The kernel arrives with the boot path already spelled as a DOS path on a *guessed* drive
     * ("C:\PPC"); all that is missing is the letter the boot device really got.  Overwriting
     * just that character is what an x86 HAL does, and it is what makes a rooted path like
     * "\PPC\KBDUS.DLL" resolve — the loader completes it with the current drive. */
    char out[80];
    ULONG n = 0;
    if (inLen >= 2 && in[1] == ':') {
        out[n++] = (char)bootLetter;
        for (ULONG i = 1; i < inLen && n < 78; i++) out[n++] = in[i];
    } else {
        out[n++] = (char)bootLetter; out[n++] = ':';
        ULONG skip = (inLen >= NtDeviceName->Length && HalpPrefix(in, NtDeviceName)) ? NtDeviceName->Length : 0;
        if (skip == 0 && in[0] != '\\') out[n++] = '\\';
        for (ULONG i = skip; i < inLen && n < 78; i++) out[n++] = in[i];
    }
    out[n] = 0;
    if (n <= NtSystemPathString->MaximumLength) {
        for (ULONG i = 0; i <= n; i++) NtSystemPath[i] = (UCHAR)out[i];
        NtSystemPathString->Length = (USHORT)n;
        HalpPrint("HAL: system path -> '%s'\n", out);
    }
}
void *memset(void *s, int c, SIZE_T n) { UCHAR *p = s; while (n--) *p++ = (UCHAR)c; return s; }
void *memcpy(void *d, const void *s, SIZE_T n) { UCHAR *pd = d; const UCHAR *ps = s; while (n--) *pd++ = *ps++; return d; }
