/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * arcfw.h -- the ARC firmware shim for the Apple Network Server: common types and the ARC
 * interface it implements.  Original work, written from the ARC specification (the Advanced
 * RISC Computing Specification, 1.2: the firmware vector, the configuration tree, the memory
 * descriptors, the environment, the I/O calls) and IEEE 1275.  Nothing here is copied from
 * Microsoft's veneer; where its behaviour was measured (docs/2026-11-05-retiring-the-veneer.md
 * and the notes it cites), the measurement is quoted beside the code that matches it. */
#pragma once

typedef unsigned char UCHAR, BOOLEAN;
typedef char CHAR, *PCHAR;
typedef unsigned short USHORT, *PUSHORT;
typedef short SHORT, CSHORT;
typedef unsigned int ULONG, *PULONG;
typedef int LONG, *PLONG;
typedef unsigned long long ULONGLONG;
typedef long long LONGLONG;
typedef void VOID, *PVOID;
typedef const void *PCVOID;
typedef unsigned int size_t;
typedef __builtin_va_list va_list;
#define va_start(v, l) __builtin_va_start(v, l)
#define va_end(v) __builtin_va_end(v)
#define va_arg(v, t) __builtin_va_arg(v, t)
#define NULL ((void *)0)
#define TRUE 1
#define FALSE 0
typedef union { struct { ULONG LowPart; LONG HighPart; }; LONGLONG QuadPart; } LARGE_INTEGER;

/* ---- ARC ----------------------------------------------------------------------------------- */
typedef ULONG ARC_STATUS;
enum { ESUCCESS, E2BIG, EACCES, EAGAIN, EBADF, EBUSY, EFAULT, EINVAL, EIO, EISDIR, EMFILE, EMLINK,
       ENAMETOOLONG, ENODEV, ENOENT, ENOEXEC, ENOMEM, ENOSPC, ENOTDIR, ENOTTY, ENXIO, EROFS };

typedef enum { SystemClass, ProcessorClass, CacheClass, AdapterClass, ControllerClass, PeripheralClass, MemoryClass } CONFIGURATION_CLASS;
typedef enum {
    ArcSystem, CentralProcessor, FloatingPointProcessor, PrimaryIcache, PrimaryDcache, SecondaryIcache,
    SecondaryDcache, SecondaryCache, EisaAdapter, TcAdapter, ScsiAdapter, DtiAdapter, MultiFunctionAdapter,
    DiskController, TapeController, CdromController, WormController, SerialController, NetworkController,
    DisplayController, ParallelController, PointerController, KeyboardController, AudioController,
    OtherController, DiskPeripheral, FloppyDiskPeripheral, TapePeripheral, ModemPeripheral, MonitorPeripheral,
    PrinterPeripheral, PointerPeripheral, KeyboardPeripheral, TerminalPeripheral, OtherPeripheral,
    LinePeripheral, NetworkPeripheral, SystemMemory, MaximumType
} CONFIGURATION_TYPE;
#define ARC_FLAG_FAILED        0x01
#define ARC_FLAG_READONLY      0x02
#define ARC_FLAG_REMOVABLE     0x04
#define ARC_FLAG_CONSOLEIN     0x08
#define ARC_FLAG_CONSOLEOUT    0x10
#define ARC_FLAG_INPUT         0x20
#define ARC_FLAG_OUTPUT        0x40

typedef struct _CONFIGURATION_COMPONENT {
    ULONG Class, Type, Flags;
    USHORT Version, Revision;
    ULONG Key, AffinityMask, ConfigurationDataLength, IdentifierLength;
    PCHAR Identifier;
} CONFIGURATION_COMPONENT, *PCONFIGURATION_COMPONENT;

typedef enum { MemoryExceptionBlock, MemorySystemBlock, MemoryFree, MemoryBad, MemoryLoadedProgram,
               MemoryFirmwareTemporary, MemoryFirmwarePermanent, MemoryFreeContiguous, MemorySpecialMemory } MEMORY_TYPE;
typedef struct _MEMORY_DESCRIPTOR { ULONG MemoryType, BasePage, PageCount; } MEMORY_DESCRIPTOR, *PMEMORY_DESCRIPTOR;

typedef struct _TIME_FIELDS { SHORT Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday; } TIME_FIELDS, *PTIME_FIELDS;
typedef enum { ArcOpenReadOnly, ArcOpenWriteOnly, ArcOpenReadWrite, ArcCreateWriteOnly, ArcCreateReadWrite,
               ArcSupersedeWriteOnly, ArcSupersedeReadWrite, ArcOpenDirectory, ArcCreateDirectory } OPEN_MODE;
typedef enum { SeekAbsolute, SeekRelative } SEEK_MODE;
typedef enum { MountLoadMedia, MountUnloadMedia } MOUNT_OPERATION;
typedef struct _FILE_INFORMATION {
    LARGE_INTEGER StartingAddress, EndingAddress, CurrentPosition;
    ULONG Type, FileNameLength;
    UCHAR Attributes;
    CHAR FileName[32];
} FILE_INFORMATION, *PFILE_INFORMATION;
#define ArcReadOnlyFile 0x01
#define ArcHiddenFile   0x02
#define ArcSystemFile   0x04
#define ArcArchiveFile  0x08
#define ArcDirectoryFile 0x10
#define ArcDeleteFile   0x20
typedef struct _DIRECTORY_ENTRY { ULONG FileNameLength; UCHAR FileAttribute; CHAR FileName[32]; } DIRECTORY_ENTRY, *PDIRECTORY_ENTRY;
typedef struct _ARC_DISPLAY_STATUS {
    USHORT CursorXPosition, CursorYPosition, CursorMaxXPosition, CursorMaxYPosition;
    UCHAR ForegroundColor, BackgroundColor, HighIntensity, Underscored, ReverseVideo;
} ARC_DISPLAY_STATUS, *PARC_DISPLAY_STATUS;
typedef struct _SYSTEM_ID { CHAR VendorId[8], ProductId[8]; } SYSTEM_ID, *PSYSTEM_ID;

/* The CM_* resource list a component's ConfigurationData is: NT's own layout (4-byte packed,
 * 16-byte descriptors), read by the kernel and by miniports such as symc810.sys.  The HAL's
 * include/nt.h declares the same thing for the kernel side. */
typedef struct __attribute__((packed, aligned(4))) _CM_PARTIAL_RESOURCE_DESCRIPTOR {
    UCHAR Type, ShareDisposition;
    USHORT Flags;
    union {
        struct { ULONG StartLo, StartHi, Length; } Port;
        struct { ULONG Level, Vector, Affinity; } Interrupt;
        struct { ULONG StartLo, StartHi, Length; } Memory;
        struct { ULONG Channel, Port, Reserved1; } Dma;
        struct { ULONG DataSize, Reserved1, Reserved2; } DeviceSpecificData;
    } u;
} CM_PARTIAL_RESOURCE_DESCRIPTOR, *PCM_PARTIAL_RESOURCE_DESCRIPTOR;
enum { CmResourceTypeNull, CmResourceTypePort, CmResourceTypeInterrupt, CmResourceTypeMemory, CmResourceTypeDma,
       CmResourceTypeDeviceSpecific };
enum { CmResourceShareUndetermined, CmResourceShareDeviceExclusive, CmResourceShareDriverExclusive, CmResourceShareShared };
#define CM_RESOURCE_PORT_MEMORY 0
#define CM_RESOURCE_PORT_IO     1
#define CM_RESOURCE_INTERRUPT_LEVEL_SENSITIVE 0
#define CM_RESOURCE_INTERRUPT_LATCHED 1
typedef struct __attribute__((packed, aligned(4))) _CM_PARTIAL_RESOURCE_LIST {
    USHORT Version, Revision;
    ULONG Count;
    CM_PARTIAL_RESOURCE_DESCRIPTOR PartialDescriptors[1];
} CM_PARTIAL_RESOURCE_LIST, *PCM_PARTIAL_RESOURCE_LIST;

/* the ARC firmware vector: 63 slots, in the order the specification gives them */
typedef struct _FIRMWARE_VECTOR {
    PVOID Load, Invoke, Execute, Halt, PowerDown, Restart, Reboot, EnterInteractiveMode, Reserved1,
          GetPeer, GetChild, GetParent, GetConfigurationData, AddChild, DeleteComponent, GetComponent,
          SaveConfiguration, GetSystemId, GetMemoryDescriptor, Reserved2, GetTime, GetRelativeTime,
          GetDirectoryEntry, Open, Close, Read, GetReadStatus, Write, Seek, Mount, GetEnvironmentVariable,
          SetEnvironmentVariable, GetFileInformation, SetFileInformation, FlushAllCaches, TestUnicodeCharacter,
          GetDisplayStatus;
} FIRMWARE_VECTOR;
#define FIRMWARE_VECTOR_ENTRIES 37

typedef struct _RESTART_BLOCK {
    ULONG Signature, Length;
    USHORT Version, Revision;
    struct _RESTART_BLOCK *NextRestartBlock;
    PVOID RestartAddress;
    ULONG BootMasterId, ProcessorId;
    ULONG BootStatus, CheckSum, SaveAreaLength;
    ULONG SaveArea[1];
} RESTART_BLOCK, *PRESTART_BLOCK;

typedef struct _SYSTEM_PARAMETER_BLOCK {
    ULONG Signature, Length;
    USHORT Version, Revision;
    PRESTART_BLOCK RestartBlock;
    PVOID DebugBlock, GenerateExceptionVector, TlbMissExceptionVector;
    ULONG FirmwareVectorLength;
    PVOID FirmwareVector;
    ULONG VendorVectorLength;
    PVOID VendorVector;
    ULONG AdapterCount, Adapter0Type, Adapter0Length;
    PVOID Adapter0Vector;
} SYSTEM_PARAMETER_BLOCK, *PSYSTEM_PARAMETER_BLOCK;
#define ARC_SYSTEM_BLOCK_SIGNATURE  0x53435241u   /* 'ARCS' */
#define ARC_RESTART_BLOCK_SIGNATURE 0x42545352u   /* 'RSTB' */
#define ARC_SPB_PHYS   0x4000u
#define ARC_KSEG0      0x80000000u
#define ARC_KSEG0_SIZE 0x00800000u                /* the veneer maps 8 MB; the loader lives there */
#define ARC_KSEG0_PAGES (ARC_KSEG0_SIZE >> 12)
#define KSEG0(p)   ((PVOID)((ULONG)(p) | ARC_KSEG0))
#define PHYS(v)    ((ULONG)(v) & ~ARC_KSEG0)

/* ---- the shim's own ------------------------------------------------------------------------- */
VOID arcfw_main(ULONG snapshot);
VOID arcfw_enter(PVOID entry, ULONG argc, PCHAR *argv, PCHAR *envp) __attribute__((noreturn));

/* console.c */
VOID con_putc(CHAR c);
VOID con_puts(const CHAR *s);
VOID con_printf(const CHAR *fmt, ...);
ULONG con_vsnprintf(CHAR *buf, ULONG n, const CHAR *fmt, va_list ap);
ULONG con_snprintf(CHAR *buf, ULONG n, const CHAR *fmt, ...);
VOID fatal(const CHAR *fmt, ...) __attribute__((noreturn));
extern ULONG arcfw_debug;      /* ARCFW_DEBUG bits, from the OF property arcfw-debug if present */
#define DBG(mask, ...) do { if (arcfw_debug & (mask)) con_printf(__VA_ARGS__); } while (0)
#define DBG_CONFIG 0x0001
#define DBG_MEM    0x0010
#define DBG_IO     0x0200
#define DBG_LOAD   0x0800
#define DBG_RW     0x1000
#define DBG_ENV    0x4000

/* string.c */
ULONG strlen(const CHAR *s);
LONG strcmp(const CHAR *a, const CHAR *b);
LONG strncmp(const CHAR *a, const CHAR *b, ULONG n);
LONG strcasecmp(const CHAR *a, const CHAR *b);
LONG strncasecmp(const CHAR *a, const CHAR *b, ULONG n);
const CHAR *strstr_ci(const CHAR *hay, const CHAR *needle);
CHAR *strcpy(CHAR *d, const CHAR *s);
CHAR *strcat(CHAR *d, const CHAR *s);
CHAR *strchr(const CHAR *s, CHAR c);
VOID *memcpy(VOID *d, const VOID *s, ULONG n);
VOID *memset(VOID *d, int c, ULONG n);
LONG memcmp(const VOID *a, const VOID *b, ULONG n);
ULONG atoul(const CHAR *s, const CHAR **end);
CHAR toupper(CHAR c);

/* heap.c: a bump allocator inside the image's own bss, firmware-permanent */
PVOID zalloc(ULONG n);
PVOID zalloc_aligned(ULONG n, ULONG align);
CHAR *strdup_(const CHAR *s);

/* arc.c */
VOID arc_init_spb(VOID);
extern SYSTEM_PARAMETER_BLOCK *arc_spb;
extern FIRMWARE_VECTOR arc_vector;

/* config.c: the configuration tree */
typedef struct _ARC_NODE {
    CONFIGURATION_COMPONENT Component;
    struct _ARC_NODE *Parent, *Child, *Peer;
    PVOID ConfigurationData;
    CHAR *OfPath;                   /* the Open Firmware path this component stands for, or NULL */
    ULONG OfPhandle;
    ULONG Unit;                     /* SCSI target for peripherals */
} ARC_NODE, *PARC_NODE;
VOID config_build(VOID);
PARC_NODE config_root(VOID);
PARC_NODE config_from_component(PCONFIGURATION_COMPONENT c);
PARC_NODE config_lookup(const CHAR *arcpath, const CHAR **rest);   /* longest component prefix */
VOID config_node_path(PARC_NODE n, CHAR *buf, ULONG len);           /* its ARC path */
VOID config_dump(PARC_NODE n, ULONG depth);

/* memory.c */
VOID memory_init(VOID);
VOID memory_note_claim(ULONG phys, ULONG size, MEMORY_TYPE type);
PMEMORY_DESCRIPTOR memory_next(PMEMORY_DESCRIPTOR prev);

/* env.c */
ARC_STATUS env_get(const CHAR *name, USHORT len, CHAR *buf);
ARC_STATUS env_set(const CHAR *name, const CHAR *value);
const CHAR *env_lookup(const CHAR *name);     /* NULL if absent; static buffer */

/* io.c */
VOID io_init(VOID);
ARC_STATUS io_open(const CHAR *path, OPEN_MODE mode, PULONG fd);
ARC_STATUS io_close(ULONG fd);
ARC_STATUS io_read(ULONG fd, PVOID buf, ULONG n, PULONG count);
ARC_STATUS io_write(ULONG fd, const VOID *buf, ULONG n, PULONG count);
ARC_STATUS io_seek(ULONG fd, LARGE_INTEGER *off, SEEK_MODE mode);
ARC_STATUS io_open_iso_file(const CHAR *arcpath, PULONG fd, PULONG offset, PULONG size);   /* a CD file, raw */

/* fat.c: a file on a FAT partition, read raw (the ROM's fat-files package stops one directory down) */
typedef struct {
    ULONG dev;                       /* the whole disk, an Open Firmware instance */
    ULONG part_lba, part_secs;       /* the partition's first sector on the disk, and its length */
    BOOLEAN raw;                     /* the partition itself, not a file on it */
    ULONG bps, spc;                  /* bytes per sector, sectors per cluster */
    ULONG fat_sec, root_sec, root_secs, data_sec, nclus, root_clus;
    UCHAR bits;                      /* 12, 16 or 32 */
    ULONG first, size;               /* the file */
    ULONG cur_idx, cur_clus;         /* where the last read left the chain walk */
    ULONG cache_sec; UCHAR cache[512] __attribute__((aligned(16)));
} FAT_FILE;
ARC_STATUS fat_open(const CHAR *devpath, ULONG part, const CHAR *file, FAT_FILE *ff);
ARC_STATUS fat_open_raw(const CHAR *devpath, ULONG part, FAT_FILE *ff);
ARC_STATUS fat_read(FAT_FILE *ff, ULONG pos, VOID *buf, ULONG n, PULONG got);
VOID fat_close(FAT_FILE *ff);
ARC_STATUS io_get_read_status(ULONG fd);
ARC_STATUS io_get_file_information(ULONG fd, PFILE_INFORMATION fi);

/* load.c */
ARC_STATUS load_image(const CHAR *arcpath, PVOID *entry_desc, PVOID *low, PVOID *high);
VOID load_run(PVOID entry_desc, ULONG argc, PCHAR *argv, PCHAR *envp) __attribute__((noreturn));
