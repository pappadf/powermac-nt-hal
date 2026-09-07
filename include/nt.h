/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * UPSTREAM: none of this file is copied, but two sources were read to write it.
 *   (1) The DDK-documented interface layouts, as reproduced in Wack0/entii-for-workcubes
 *   nt4/hal/{arc.h,hal.h,halppc.h,ppcdef.h} and halartx/source/nthal.h (commit c9b041da;
 *   those files are Microsoft-derived and are deliberately NOT vendored here).
 *   (2) Field offsets observed in the shipped NTKRNLMP.EXE's own code with
 *   tools/pe-dis.py.  Declarations that must match the kernel unavoidably share its
 *   names — that is what binary compatibility means — but every layout here was
 *   re-declared in our own words and pinned with _Static_assert.  See PROVENANCE.md.
 *
 * nt.h — the slice of the Windows NT 4.0 kernel/HAL contract this HAL uses, written from the
 * documented DDK interfaces (types, structure layouts, calling conventions).  Field offsets that
 * the kernel and HAL share are pinned with static asserts. */
#pragma once

typedef unsigned char UCHAR, BOOLEAN, *PUCHAR, *PBOOLEAN;
typedef char CHAR, CCHAR, *PCHAR;
typedef unsigned short USHORT, *PUSHORT, WCHAR;
typedef short SHORT, CSHORT;
typedef unsigned int ULONG, *PULONG, KAFFINITY, *PKAFFINITY;
typedef int LONG, *PLONG, NTSTATUS, ARC_STATUS;
typedef unsigned long long ULONGLONG;
typedef long long LONGLONG;
typedef void VOID, *PVOID;
typedef UCHAR KIRQL, *PKIRQL;
typedef ULONG SIZE_T;
#define TRUE 1
#define FALSE 0
#define NULL ((void *)0)
#define IN
#define OUT
#define OPTIONAL
#define UNREFERENCED_PARAMETER(p) (void)(p)
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001)
#define STATUS_NOT_IMPLEMENTED ((NTSTATUS)0xC0000002)
#define STATUS_NO_MEMORY ((NTSTATUS)0xC0000017)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xC000009A)
#define STATUS_NOT_FOUND ((NTSTATUS)0xC0000225)
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xC00000BB)
#define STATUS_NO_SUCH_DEVICE ((NTSTATUS)0xC000000E)
#define STATUS_PENDING ((NTSTATUS)0x00000103)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000D)

/* NT *passes* LARGE_INTEGER/PHYSICAL_ADDRESS by value in a register pair (low word in the lower
 * register on this little-endian machine), which is how the SVR4 ABI treats a scalar 64-bit
 * integer but not a union — so for the ABI they are scalars here.
 *
 * *Returning* one is different: they are unions, and Microsoft's PowerPC compiler returns every
 * struct, eight bytes included, through a hidden first argument in r3 — the real arguments then
 * start at r4.  MmGetPhysicalAddress reads its VirtualAddress from r4, and SCSIPORT.SYS calls
 * IoMapTransfer with r3 pointing at a stack slot it reads the result out of afterwards.  So any
 * routine on this boundary that "returns" a LARGE_INTEGER is written here as a void routine
 * taking that out-pointer first. */
typedef LONGLONG LARGE_INTEGER;
typedef ULONGLONG PHYSICAL_ADDRESS;
typedef LARGE_INTEGER *PLARGE_INTEGER;
typedef PHYSICAL_ADDRESS *PPHYSICAL_ADDRESS;
typedef union _ULARGE_INTEGER { struct { ULONG LowPart; ULONG HighPart; }; ULONGLONG QuadPart; } ULARGE_INTEGER, *PULARGE_INTEGER;
#define LOW32(v)  ((ULONG)(v))
#define HIGH32(v) ((ULONG)((ULONGLONG)(v) >> 32))
typedef struct _LIST_ENTRY { struct _LIST_ENTRY *Flink, *Blink; } LIST_ENTRY, *PLIST_ENTRY;
typedef struct _UNICODE_STRING { USHORT Length, MaximumLength; WCHAR *Buffer; } UNICODE_STRING, *PUNICODE_STRING;
typedef struct _STRING { USHORT Length, MaximumLength; PCHAR Buffer; } STRING, *PSTRING;
typedef ULONG KSPIN_LOCK, *PKSPIN_LOCK;

/* A function pointer in the NT PowerPC ABI is the address of a two-word descriptor. */
typedef struct _FUNC_DESC { PVOID Entry; ULONG Toc; } FUNC_DESC, *PFUNC_DESC;
#define DEFINE_DESC(f) const FUNC_DESC f##_desc = { (PVOID)(f), 0 }
#define DESC(f) ((PVOID)&f##_desc)

/* ---- IRQL, vectors (ppcdef.h contract) ------------------------------------------------ */
#define PASSIVE_LEVEL 0
#define APC_LEVEL 1
#define DISPATCH_LEVEL 2
#define PROFILE_LEVEL 27
#define CLOCK2_LEVEL 28
#define IPI_LEVEL 29
#define POWER_LEVEL 30
#define HIGH_LEVEL 31
#define MAXIMUM_DEVICE_LEVEL 27
#define MACHINE_CHECK_VECTOR 4
#define EXTERNAL_INTERRUPT_VECTOR 5
#define DECREMENT_VECTOR 7
#define DEVICE_VECTORS 32
#define MAXIMUM_VECTOR 256
#define MAXIMUM_INCREMENT 100000  /* 10 ms in 100 ns units */
#define MINIMUM_INCREMENT 10000   /* 1 ms */

/* ---- bugcheck codes ------------------------------------------------------------------- */
#define IRQL_NOT_GREATER_OR_EQUAL 0x09
#define IRQL_NOT_LESS_OR_EQUAL 0x0A
#define HAL_INITIALIZATION_FAILED 0x5C
#define UNSUPPORTED_PROCESSOR 0x5D
#define MISMATCHED_HAL 0x79
#define NMI_HARDWARE_FAILURE 0x80

/* ---- ARC status codes ----------------------------------------------------------------- */
enum { ESUCCESS = 0, E2BIG, EACCES, EAGAIN, EBADF, EBUSY, EFAULT, EINVAL, EIO, EISDIR, EMFILE, EMLINK,
       ENAMETOOLONG, ENODEV, ENOENT, ENOEXEC, ENOMEM, ENOSPC, ENOTDIR, ENOTTY, ENXIO, EROFS };

/* ---- Processor Control Region (PowerPC), reached through SPRG1 -------------------------- */
typedef struct _KPRCB {
    USHORT MinorVersion, MajorVersion;
    PVOID CurrentThread, NextThread, IdleThread;
    CCHAR Number, Reserved;
    USHORT BuildType;
    KAFFINITY SetMember;
    PVOID RestartBlock;
    ULONG PcrPage, PcrPage2;
    ULONG SystemReserved[15];
    ULONG HalReserved[16];
} KPRCB, *PKPRCB;
#define PRCB_MAJOR_VERSION 1

typedef struct _KPCR {
    USHORT MinorVersion, MajorVersion;
    PVOID InterruptRoutine[MAXIMUM_VECTOR];       /* 0x004: descriptors */
    ULONG PcrPage2;                               /* 0x404 */
    ULONG Kseg0Top;                               /* 0x408 */
    ULONG Spare7[30];
    ULONG FirstLevelDcacheSize, FirstLevelDcacheFillSize, FirstLevelIcacheSize, FirstLevelIcacheFillSize;
    ULONG SecondLevelDcacheSize, SecondLevelDcacheFillSize, SecondLevelIcacheSize, SecondLevelIcacheFillSize;
    PKPRCB Prcb;                                  /* 0x4a4 */
    PVOID Teb;
    ULONG DcacheAlignment, DcacheFillSize, IcacheAlignment, IcacheFillSize;
    ULONG ProcessorVersion, ProcessorRevision;    /* 0x4bc */
    ULONG ProfileInterval, ProfileCount;
    ULONG StallExecutionCount, StallScaleFactor;  /* 0x4cc */
    ULONG Spare;
    ULONG CachePolicy;
    UCHAR IrqlMask[32];                           /* 0x4dc */
    UCHAR IrqlTable[9];                           /* 0x4fc */
    UCHAR CurrentIrql;                            /* 0x505 */
    CCHAR Number;                                 /* 0x506 */
    UCHAR Pad0;
    KAFFINITY SetMember;                          /* 0x508 */
    ULONG ReservedVectors;                        /* 0x50c */
    PVOID CurrentThread;                          /* 0x510 */
    ULONG AlignedCachePolicy;
    union { ULONG SoftwareInterrupt; struct { UCHAR ApcInterrupt, DispatchInterrupt, Spare4, Spare5; }; }; /* 0x518 */
    KAFFINITY NotMember;
    ULONG SystemReserved[16];                     /* 0x520 */
    ULONG HalReserved[16];                        /* 0x560 */
} KPCR, *PKPCR;
_Static_assert(__builtin_offsetof(KPCR, Prcb) == 0x4a4, "KPCR.Prcb");
_Static_assert(__builtin_offsetof(KPCR, StallScaleFactor) == 0x4d0, "KPCR.StallScaleFactor");
_Static_assert(__builtin_offsetof(KPCR, CurrentIrql) == 0x505, "KPCR.CurrentIrql");
_Static_assert(__builtin_offsetof(KPCR, SoftwareInterrupt) == 0x518, "KPCR.SoftwareInterrupt");
_Static_assert(__builtin_offsetof(KPCR, HalReserved) == 0x560, "KPCR.HalReserved");
static inline PKPCR HalpGetPcr(void) { ULONG p; __asm__ volatile("mfsprg %0, 1" : "=r"(p)); return (PKPCR)p; }
#define PCR (HalpGetPcr())

/* ---- ARC configuration tree and loader block --------------------------------------------- */
typedef enum { SystemClass, ProcessorClass, CacheClass, AdapterClass, ControllerClass, PeripheralClass, MemoryClass } CONFIGURATION_CLASS;
typedef enum { ArcSystem, CentralProcessor, FloatingPointProcessor, PrimaryIcache, PrimaryDcache, SecondaryIcache, SecondaryDcache,
    SecondaryCache, EisaAdapter, TcAdapter, ScsiAdapter, DtiAdapter, MultiFunctionAdapter, DiskController, TapeController,
    CdromController, WormController, SerialController, NetworkController, DisplayController, ParallelController, PointerController,
    KeyboardController, AudioController, OtherController, DiskPeripheral, FloppyDiskPeripheral, TapePeripheral, ModemPeripheral,
    MonitorPeripheral, PrinterPeripheral, PointerPeripheral, KeyboardPeripheral, TerminalPeripheral, OtherPeripheral, LinePeripheral,
    NetworkPeripheral, SystemMemory } CONFIGURATION_TYPE;
typedef struct _CONFIGURATION_COMPONENT {
    CONFIGURATION_CLASS Class; CONFIGURATION_TYPE Type; ULONG Flags; USHORT Version, Revision; ULONG Key, AffinityMask;
    ULONG ConfigurationDataLength, IdentifierLength; PCHAR Identifier;
} CONFIGURATION_COMPONENT, *PCONFIGURATION_COMPONENT;
typedef struct _CONFIGURATION_COMPONENT_DATA {
    struct _CONFIGURATION_COMPONENT_DATA *Parent, *Child, *Sibling; CONFIGURATION_COMPONENT ComponentEntry; PVOID ConfigurationData;
} CONFIGURATION_COMPONENT_DATA, *PCONFIGURATION_COMPONENT_DATA;
typedef struct _MEMORY_ALLOCATION_DESCRIPTOR { LIST_ENTRY ListEntry; ULONG MemoryType, BasePage, PageCount; } MEMORY_ALLOCATION_DESCRIPTOR, *PMEMORY_ALLOCATION_DESCRIPTOR;
typedef struct _NLS_DATA_BLOCK { PVOID AnsiCodePageData, OemCodePageData, UnicodeCaseTableData; } NLS_DATA_BLOCK, *PNLS_DATA_BLOCK;
typedef struct _PPC_LOADER_BLOCK {
    ULONG InterruptStack, FirstLevelDcacheSize, FirstLevelDcacheFillSize, FirstLevelIcacheSize, FirstLevelIcacheFillSize;
    ULONG HashedPageTable, PanicStack, PcrPage, PdrPage;
    ULONG SecondLevelDcacheSize, SecondLevelDcacheFillSize, SecondLevelIcacheSize, SecondLevelIcacheFillSize, PcrPage2;
    UCHAR IcacheMode, DcacheMode; USHORT NumberCongruenceClasses; ULONG Kseg0Top; UCHAR MajorVersion, MinorVersion; USHORT Reserved;
    ULONG HashedPageTableSize; PVOID PcrPagesDescriptor, KernelKseg0PagesDescriptor; ULONG MinimumBlockLength, MaximumBlockLength;
} PPC_LOADER_BLOCK;
typedef struct _LOADER_PARAMETER_BLOCK {
    LIST_ENTRY LoadOrderListHead, MemoryDescriptorListHead, BootDriverListHead;
    ULONG KernelStack, Prcb, Process, Thread, RegistryLength; PVOID RegistryBase;
    PCONFIGURATION_COMPONENT_DATA ConfigurationRoot;
    PCHAR ArcBootDeviceName, ArcHalDeviceName, NtBootPathName, NtHalPathName, LoadOptions;
    PNLS_DATA_BLOCK NlsData; PVOID ArcDiskInformation, OemFontFile, SetupLoaderBlock; ULONG Spare1;
    union { PPC_LOADER_BLOCK Ppc; } u;
} LOADER_PARAMETER_BLOCK, *PLOADER_PARAMETER_BLOCK;
/* ARC_DISK_INFORMATION is just the list head; each entry is one disk the firmware could name. */
typedef struct _ARC_DISK_SIGNATURE { LIST_ENTRY ListEntry; ULONG Signature; PCHAR ArcName; ULONG CheckSum; BOOLEAN ValidPartitionTable; }
    ARC_DISK_SIGNATURE, *PARC_DISK_SIGNATURE;
typedef struct _LDR_DATA_TABLE_ENTRY { LIST_ENTRY InLoadOrderLinks, InMemoryOrderLinks, InInitializationOrderLinks; PVOID DllBase, EntryPoint; ULONG SizeOfImage; UNICODE_STRING FullDllName, BaseDllName; } LDR_DATA_TABLE_ENTRY, *PLDR_DATA_TABLE_ENTRY;

/* CM_PARTIAL_RESOURCE_DESCRIPTOR as the ARC firmware stores it in ConfigurationData */
#define CmResourceTypePort 1
#define CmResourceTypeInterrupt 2
#define CmResourceTypeMemory 3
#define CmResourceTypeDma 4
#define CmResourceTypeDeviceSpecific 5
#pragma pack(push,4)
typedef struct _CM_PARTIAL_RESOURCE_DESCRIPTOR {
    UCHAR Type, ShareDisposition; USHORT Flags;
    union {
        struct { PHYSICAL_ADDRESS Start; ULONG Length; } Port;
        struct { ULONG Level, Vector, Affinity; } Interrupt;
        struct { PHYSICAL_ADDRESS Start; ULONG Length; } Memory;
        struct { ULONG Channel, Port, Reserved1; } Dma;
        struct { ULONG DataSize, Reserved1, Reserved2; } DeviceSpecificData;
    } u;
} CM_PARTIAL_RESOURCE_DESCRIPTOR, *PCM_PARTIAL_RESOURCE_DESCRIPTOR;
typedef struct _CM_PARTIAL_RESOURCE_LIST { USHORT Version, Revision; ULONG Count; CM_PARTIAL_RESOURCE_DESCRIPTOR PartialDescriptors[1]; } CM_PARTIAL_RESOURCE_LIST, *PCM_PARTIAL_RESOURCE_LIST;
_Static_assert(sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR)==16, "CM_PARTIAL_RESOURCE_DESCRIPTOR must be 16 bytes (NT pshpack4)");
#pragma pack(pop)

/* ---- HAL interface enums -------------------------------------------------------------------- */
typedef enum { Internal, Isa, Eisa, MicroChannel, TurboChannel, PCIBus, VMEBus, NuBus, PCMCIABus, CBus, MPIBus, MPSABus,
               ProcessorInternal, InternalPowerBus, PNPISABus, MaximumInterfaceType } INTERFACE_TYPE;
#pragma pack(push,4)
typedef struct _CM_FULL_RESOURCE_DESCRIPTOR { INTERFACE_TYPE InterfaceType; ULONG BusNumber; CM_PARTIAL_RESOURCE_LIST PartialResourceList; } CM_FULL_RESOURCE_DESCRIPTOR, *PCM_FULL_RESOURCE_DESCRIPTOR;
typedef struct _CM_RESOURCE_LIST_REAL { ULONG Count; CM_FULL_RESOURCE_DESCRIPTOR List[1]; } CM_RESOURCE_LIST_REAL, *PCM_RESOURCE_LIST_REAL;
#pragma pack(pop)
#define CmResourceShareDeviceExclusive 1
#define CM_RESOURCE_PORT_IO 1
#define CM_RESOURCE_MEMORY_READ_WRITE 0
#define CM_RESOURCE_INTERRUPT_LEVEL_SENSITIVE 0
typedef enum { ConfigurationSpaceUndefined = -1, Cmos, EisaConfiguration, Pos, CbusConfiguration, PCIConfiguration, VMEConfiguration,
               NuBusConfiguration, PCMCIAConfiguration, MPIConfiguration, MPSAConfiguration, PNPISAConfiguration, MaximumBusDataType } BUS_DATA_TYPE;
typedef enum { HalHaltRoutine, HalPowerDownRoutine, HalRestartRoutine, HalRebootRoutine, HalInteractiveModeRoutine, HalMaximumRoutine } FIRMWARE_REENTRY;
typedef enum { LevelSensitive, Latched } KINTERRUPT_MODE;
typedef enum { ProfileTime, ProfileAlignmentFixup, ProfileTotalIssues, ProfileMaximum } KPROFILE_SOURCE;
typedef enum { Width8Bits, Width16Bits, Width32Bits, MaximumDmaWidth } DMA_WIDTH;
typedef enum { Compatible, TypeA, TypeB, TypeC, MaximumDmaSpeed } DMA_SPEED;
typedef struct _DEVICE_DESCRIPTION {
    ULONG Version; BOOLEAN Master, ScatterGather, DemandMode, AutoInitialize, Dma32BitAddresses, IgnoreCount, Reserved1, Reserved2;
    ULONG BusNumber, DmaChannel; INTERFACE_TYPE InterfaceType; DMA_WIDTH DmaWidth; DMA_SPEED DmaSpeed; ULONG MaximumLength, DmaPort;
} DEVICE_DESCRIPTION, *PDEVICE_DESCRIPTION;
#define DEVICE_DESCRIPTION_VERSION1 1
typedef struct _TIME_FIELDS { CSHORT Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday; } TIME_FIELDS, *PTIME_FIELDS;
typedef struct _MDL { struct _MDL *Next; CSHORT Size, MdlFlags; PVOID Process, MappedSystemVa, StartVa; ULONG ByteCount, ByteOffset; } MDL, *PMDL;
#define MDL_IO_PAGE_READ 0x0040
typedef struct _KINTERRUPT KINTERRUPT, *PKINTERRUPT;
typedef struct _ADAPTER_OBJECT *PADAPTER_OBJECT;
typedef struct _DEVICE_OBJECT *PDEVICE_OBJECT;
typedef struct _DRIVER_OBJECT *PDRIVER_OBJECT;
/* The kernel's IoAllocateAdapterChannel fills this out of DEVICE_OBJECT.Queue.Wcb and hands it to
 * HalAllocateAdapterChannel; the first 16 bytes are the KDEVICE_QUEUE_ENTRY the HAL would use to
 * queue the request if the adapter were busy. */
typedef struct _WAIT_CONTEXT_BLOCK {
    UCHAR WaitQueueEntry[16];
    PVOID DeviceRoutine;                /* PDRIVER_CONTROL: pointer to a { entry, toc } descriptor */
    PVOID DeviceContext;
    ULONG NumberOfMapRegisters;
    PVOID DeviceObject;
    PVOID CurrentIrp;
    PVOID BufferChainingDpc;
} WAIT_CONTEXT_BLOCK, *PWAIT_CONTEXT_BLOCK;
_Static_assert(sizeof(WAIT_CONTEXT_BLOCK) == 40, "WAIT_CONTEXT_BLOCK must be 40 bytes");
typedef enum { KeepObject = 1, DeallocateObject, DeallocateObjectKeepRegisters } IO_ALLOCATION_ACTION;
/* Partition tables.  ntdddisk.h is not packed: LARGE_INTEGER forces 8-byte alignment, so
 * PARTITION_INFORMATION is 32 bytes and DRIVE_LAYOUT_INFORMATION's array starts at +8. */
typedef struct _PARTITION_INFORMATION {
    LARGE_INTEGER StartingOffset;
    LARGE_INTEGER PartitionLength;
    ULONG HiddenSectors;
    ULONG PartitionNumber;
    UCHAR PartitionType;
    BOOLEAN BootIndicator;
    BOOLEAN RecognizedPartition;
    BOOLEAN RewritePartition;
} PARTITION_INFORMATION, *PPARTITION_INFORMATION;
_Static_assert(sizeof(PARTITION_INFORMATION) == 32, "PARTITION_INFORMATION must be 32 bytes");
typedef struct _DRIVE_LAYOUT_INFORMATION {
    ULONG PartitionCount;
    ULONG Signature;
    PARTITION_INFORMATION PartitionEntry[1];
} DRIVE_LAYOUT_INFORMATION, *PDRIVE_LAYOUT_INFORMATION;
_Static_assert(sizeof(DRIVE_LAYOUT_INFORMATION) == 40, "DRIVE_LAYOUT_INFORMATION must be 40 bytes");
#define PARTITION_ENTRY_UNUSED  0x00
#define PARTITION_FAT_12        0x01
#define PARTITION_XENIX_1       0x02
#define PARTITION_XENIX_2       0x03
#define PARTITION_FAT_16        0x04
#define PARTITION_EXTENDED      0x05
#define PARTITION_HUGE          0x06
#define PARTITION_IFS           0x07
#define PARTITION_FAT32         0x0b
#define PARTITION_FAT32_XINT13  0x0c
#define PARTITION_XINT13        0x0e
#define PARTITION_XINT13_EXTENDED 0x0f
#define PARTITION_NTFT          0x80
#define VALID_NTFT              0xc0

typedef struct _IO_STATUS_BLOCK { NTSTATUS Status; ULONG Information; } IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;
typedef struct _KEVENT { ULONG Opaque[4]; } KEVENT, *PKEVENT;   /* DISPATCHER_HEADER only: 16 bytes */
typedef struct _IRP *PIRP;
#define IRP_MJ_READ  3
#define IRP_MJ_WRITE 4
#define NotificationEvent 0
#define KernelMode 0
#define Executive 0
typedef struct _IO_RESOURCE_REQUIREMENTS_LIST *PIO_RESOURCE_REQUIREMENTS_LIST;
typedef struct _CM_RESOURCE_LIST *PCM_RESOURCE_LIST;
typedef struct _DEBUG_PARAMETERS *PDEBUG_PARAMETERS;
typedef struct _KPROCESSOR_STATE *PKPROCESSOR_STATE;
typedef PVOID PDRIVER_CONTROL;
typedef struct _CONFIGURATION_INFORMATION { ULONG DiskCount, FloppyCount, CdRomCount, TapeCount, ScsiPortCount, SerialCount, ParallelCount;
    BOOLEAN AtDiskPrimaryAddressClaimed, AtDiskSecondaryAddressClaimed; } CONFIGURATION_INFORMATION, *PCONFIGURATION_INFORMATION;

/* ---- kernel routines the HAL calls (through the descriptor thunk, see thunk.S) --------------- */
VOID KeBugCheck(ULONG Code) __attribute__((noreturn));
VOID KeBugCheckEx(ULONG Code, ULONG P1, ULONG P2, ULONG P3, ULONG P4) __attribute__((noreturn));
VOID KeSetTimeIncrement(ULONG MaximumIncrement, ULONG MinimumIncrement);
typedef struct _KDPC { ULONG Opaque[16] __attribute__((aligned(8))); } KDPC, *PKDPC;  /* 32 bytes on this build; room and alignment to spare */
VOID KeInitializeDpc(PKDPC Dpc, PVOID DeferredRoutine, PVOID DeferredContext);
BOOLEAN KeInsertQueueDpc(PKDPC Dpc, PVOID SystemArgument1, PVOID SystemArgument2);
VOID KeUpdateSystemTime(PVOID TrapFrame, ULONG Increment);
VOID KiDispatchSoftwareInterrupt(VOID);
PVOID KePhase0MapIo(ULONG PhysicalBase, ULONG Size);
PVOID KePhase0DeleteIoMap(ULONG PhysicalBase, ULONG Size);
PVOID MmMapIoSpace(PHYSICAL_ADDRESS Physical, ULONG Size, BOOLEAN CacheEnable);   /* r3:r4 = address */
PVOID MmAllocateContiguousMemory(ULONG Size, PHYSICAL_ADDRESS Highest);
VOID MmFreeContiguousMemory(PVOID Va);
VOID MmGetPhysicalAddress(PPHYSICAL_ADDRESS Result, PVOID Va);   /* r3 = hidden return slot */
VOID RtlTimeToTimeFields(PLARGE_INTEGER Time, PTIME_FIELDS TimeFields);
VOID RtlInitUnicodeString(PUNICODE_STRING Destination, const WCHAR *Source);
NTSTATUS IoCreateSymbolicLink(PUNICODE_STRING SymbolicLinkName, PUNICODE_STRING DeviceName);
BOOLEAN RtlTimeFieldsToTime(PTIME_FIELDS TimeFields, PLARGE_INTEGER Time);
PVOID ExAllocatePool(ULONG PoolType, ULONG NumberOfBytes);
VOID ExFreePool(PVOID P);
PCONFIGURATION_INFORMATION IoGetConfigurationInformation(VOID);
PIRP IoBuildSynchronousFsdRequest(ULONG MajorFunction, PDEVICE_OBJECT DeviceObject, PVOID Buffer,
                                  ULONG Length, PLARGE_INTEGER StartingOffset, PKEVENT Event,
                                  PIO_STATUS_BLOCK IoStatusBlock);
NTSTATUS IoCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp);
VOID KeInitializeEvent(PKEVENT Event, ULONG Type, BOOLEAN State);
NTSTATUS KeWaitForSingleObject(PVOID Object, ULONG WaitReason, ULONG WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Timeout);
VOID KeInitializeSpinLock(PKSPIN_LOCK Lock);
extern PVOID __imp_KdDebuggerEnabled;
#define KdDebuggerEnabled (*(BOOLEAN *)__imp_KdDebuggerEnabled)
#define NonPagedPool 0

/* memset/memcpy for the compiler */
void *memset(void *s, int c, SIZE_T n);
void *memcpy(void *d, const void *s, SIZE_T n);

/* PowerPC KTRAP_FRAME, the part a machine-check handler touches: volatile GPRs 0..12 and the
 * resume address.  Non-volatile registers are not in the frame (the kernel keeps them live). */
#define TR_GPR0 0x5c
#define TR_MSR  0x110
#define TR_IAR  0x114
#define TR_LR   0x118
#define TR_GPR(tf, n) (*(PULONG)((PUCHAR)(tf) + TR_GPR0 + 4 * (n)))
#define TR_FIELD(tf, off) (*(PULONG)((PUCHAR)(tf) + (off)))

/* OEM console font, as SETUPLDR hands it in LOADER_PARAMETER_BLOCK.OemFontFile (the NT bitmap
 * font: a header then column-major glyph bytes, Map[ch-First].Offset from the header start). */
#pragma pack(push,1)
typedef struct _OEM_FONT_FILE_HEADER {
    USHORT Version; ULONG FileSize; UCHAR Copyright[60]; USHORT Type, Points, VerticleResolution,
    HorizontalResolution, Ascent, InternalLeading, ExternalLeading; UCHAR Italic, Underline, StrikeOut;
    USHORT Weight; UCHAR CharacterSet; USHORT PixelWidth, PixelHeight; UCHAR Family; USHORT AverageWidth,
    MaximumWidth; UCHAR FirstCharacter, LastCharacter, DefaultCharacter, BreakCharacter; USHORT WidthInBytes;
    ULONG Device, Face, BitsPointer, BitsOffset; UCHAR Filler;
    struct { USHORT Width, Offset; } Map[1];
} OEM_FONT_FILE_HEADER, *POEM_FONT_FILE_HEADER;
#pragma pack(pop)
