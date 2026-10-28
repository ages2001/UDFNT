/*
 * UDFNT - UDF (Universal Disk Format) file system driver for Windows NT
 * 3.1, 3.5, 3.51, 4.0, 2000, XP and XP x64 / Server 2003 x64.
 *
 *   UDN_CROSS  GCC cross build: one i386 binary for NT 3.1 - XP
 *
 * The on-disk work is done by the shared engine (udfsup.c, udfbuf.c,
 * udfvol.c, udfino.c, udfdir.c), reached through ufx.c. File data is cached
 * by the Cache Manager in each file's own section and goes to the disk
 * non-cached, mapped through the file's extents (ufx.h explains how UDF
 * files are made fit for that).
 */

#ifndef _UDN_H_
#define _UDN_H_

#ifndef UDF_KERNEL
#define UDF_KERNEL
#endif

#ifdef UDN_CROSS
#ifndef UDN_NT31
#define UDN_NT31
#endif
#ifndef UDN_NT4
#define UDN_NT4
#endif
#endif /* UDN_CROSS */

/*
 * GCC has no __try/__finally: the handlers' cleanup blocks become plain code
 * reached by goto (__leave). Exceptions are caught with UdnTryCall.
 */
#ifdef __GNUC__
#define __try
#define __finally               __exn_leave: __attribute__((unused));
#define __except(x)             if (0)
#define __leave                 goto __exn_leave
#ifndef AbnormalTermination
#define AbnormalTermination()   0
#endif
#ifndef GetExceptionInformation
#define GetExceptionInformation() ((PEXCEPTION_POINTERS)0)
#endif
#ifndef GetExceptionCode
#define GetExceptionCode()      0
#endif
#ifndef EXCEPTION_EXECUTE_HANDLER
#define EXCEPTION_EXECUTE_HANDLER 1
#endif
#ifndef EXCEPTION_CONTINUE_SEARCH
#define EXCEPTION_CONTINUE_SEARCH 0
#endif
#endif /* __GNUC__ */

#include <ntifs.h>
#include <ntdddisk.h>

#include "ufx.h"
#include "udfshim.h"
#include "udnctl.h"

/* ------------------------------------------------------------------ */
/* NT 3.1 import profile                                               */
/* ------------------------------------------------------------------ */

/*
 * Windows NT 3.1 does not export the Lite executive-resource functions,
 * MmCanFileBeTruncated, or the FsRtlNotifyFull / NotifySync change-
 * notification family (all introduced in NT 3.5/4.0). Mapping the Lite
 * calls to their originals is exact; truncation always allowed is the safe
 * choice where the memory manager cannot answer; change notification is an
 * optional feature and is left out on NT 3.1. One binary then loads on NT
 * 3.1 through 4.0. (NT 3.51 and later export the Lite set, so a build
 * without UDN_NT31 uses it directly.)
 */
#ifdef UDN_NT31

/*
 * The mingw DDK aliases the legacy resource names to their Lite forms (the
 * only ones modern ntoskrnl exports). Undo that and import the executive-
 * resource functions that are common to EVERY NT the one binary must load
 * on - 3.1, 3.51 and 4.0 - resolving the Lite names to them.
 *
 * The catch is the shared acquire: NT 3.1 exports ExAcquireResourceShared
 * but not the Lite form, while NT 3.51+ export ExAcquireResourceSharedLite
 * but DROPPED ExAcquireResourceShared. There is no shared-acquire name
 * common to all three, and an import the running kernel cannot resolve makes
 * the OS loader reject the driver as "missing or corrupt" (this is exactly
 * why the single binary failed to load on NT 3.51). The exclusive acquire,
 * by contrast, is exported under the same name (ExAcquireResourceExclusive)
 * by every one of them. So the one binary acquires exclusively wherever the
 * code asks for shared: exclusive is always a safe substitute for shared
 * (it is strictly more restrictive, the lock hierarchy stays acyclic, so no
 * new deadlock is possible), at the only cost of serialising concurrent
 * readers - immaterial on the uniprocessor retro systems this targets.
 * (A Visual C++ build pinned to a single NT version can instead import that
 * version's real shared acquire and keep true reader concurrency.)
 */
#undef ExAcquireResourceExclusive
#undef ExAcquireResourceShared
#undef ExReleaseResourceForThread
#undef ExInitializeResource
#undef ExDeleteResource
#undef ExAcquireResourceExclusiveLite
#undef ExAcquireResourceSharedLite
#undef ExReleaseResourceForThreadLite
#undef ExInitializeResourceLite
#undef ExDeleteResourceLite

/*
 * Resolved at DriverEntry from the running ntoskrnl (see exnres.c): the Lite
 * form on NT 3.51+, the legacy form on NT 3.1. The code calls the Lite names;
 * they expand to calls through these pointers. (Init/Delete return NTSTATUS in
 * the Lite ABI and VOID in the legacy one; the pointer type is NTSTATUS and
 * the legacy return is simply ignored - harmless on stdcall x86.)
 */
typedef NTSTATUS (NTAPI *UDN_PINITRES)(PERESOURCE);
typedef BOOLEAN  (NTAPI *UDN_PACQRES)(PERESOURCE, BOOLEAN);
typedef VOID     (NTAPI *UDN_PRELRES)(PERESOURCE, ERESOURCE_THREAD);
typedef NTSTATUS (NTAPI *UDN_PDELRES)(PERESOURCE);

extern UDN_PINITRES ExnpInitRes;
extern UDN_PACQRES  ExnpAcqExcl;
extern UDN_PACQRES  ExnpAcqShared;
extern UDN_PDELRES  ExnpDelRes;
extern UDN_PRELRES  ExnpRelForThread;

BOOLEAN UdnResolveResourceApi(VOID);

#define ExInitializeResourceLite(r)          ExnpInitRes(r)
#define ExAcquireResourceExclusiveLite(r,w)  ExnpAcqExcl((r),(w))
#define ExAcquireResourceSharedLite(r,w)     ExnpAcqShared((r),(w))
#define ExDeleteResourceLite(r)              ExnpDelRes(r)
#define ExReleaseResourceForThreadLite(r,t)  ExnpRelForThread((r),(t))

/*
 * NT 4.0 introduced the fast-call Iof/Obf forms; NT 3.1 has only the
 * stdcall originals. The DDK maps the plain names to the fast-call ones,
 * so undo that and declare the stdcall exports NT 3.1 actually has.
 */
#undef IoCallDriver
#undef IoCompleteRequest
#undef ObDereferenceObject
__declspec(dllimport) NTSTATUS NTAPI IoCallDriver(PDEVICE_OBJECT, PIRP);
__declspec(dllimport) VOID     NTAPI IoCompleteRequest(PIRP, CCHAR);
__declspec(dllimport) VOID     NTAPI ObDereferenceObject(PVOID);

#define MmCanFileBeTruncated(S, N)      TRUE
#define FsRtlNotifyInitializeSync(S)    ((void)0)
#define FsRtlNotifyUninitializeSync(S)  ((void)0)
#define FsRtlNotifyCleanup(a, b, c)     ((void)0)
#define FsRtlNotifyFullChangeDirectory(a,b,c,d,e,f,g,h,i,j)  ((void)0)
#define FsRtlNotifyFullReportChange(a,b,c,d,e,f,g,h,i)       ((void)0)
#endif /* UDN_NT31 */

#ifdef __GNUC__
/*
 * The mingw DDK IO_STACK_LOCATION already carries the file-system members,
 * so the UDN_NT4 extended-stack cast is a plain identity there.
 */
typedef IO_STACK_LOCATION EXTENDED_IO_STACK_LOCATION, *PEXTENDED_IO_STACK_LOCATION;

/* The DDK headers redefine these SEH intrinsics; re-assert the shim so no
   _abnormal_termination / exception-record references are emitted. */
#undef AbnormalTermination
#define AbnormalTermination()       0
#undef GetExceptionCode
#define GetExceptionCode()          0
#undef GetExceptionInformation
#define GetExceptionInformation()   ((PEXCEPTION_POINTERS)0)
#endif

#ifdef UDN_CROSS
/*
 * NT 3.1 does not export ExAllocatePoolWithTag, the Ke critical-region
 * calls, or the top-level-IRP / verify / requestor helpers (newer builds
 * access them inline through the thread). The universal binary drops the
 * pool tag, makes the critical region a no-op pair, and keeps the top-level
 * IRP in one process-wide slot. A Visual C++ NT 3.1 build would instead use
 * the real inline thread-field accessors.
 *
 * The requestor of an IRP is the process of the thread it carries: that is
 * the current process when the IRP runs in its own thread, as a cleanup does
 * (the closing thread sends it). It must be right there: the byte-range locks
 * a handle took go at its cleanup only when they are matched by process, or
 * they outlive the handle and the next lock of the file waits for ever
 * (WritePrivateProfileString, Explorer writing desktop.ini).
 */
#define ExAllocatePoolWithTag(Type, Size, Tag)  ExAllocatePool((Type), (Size))
#define KeEnterCriticalRegion()                 ((void)0)
#define KeLeaveCriticalRegion()                 ((void)0)
extern PIRP UdnXTopLevelIrp;
#define IoGetTopLevelIrp()          (UdnXTopLevelIrp)
#define IoSetTopLevelIrp(I)         (UdnXTopLevelIrp = (PIRP)(I))
#define IoGetRequestorProcess(Irp)  UdnRequestorProcess(Irp)
static __inline PEPROCESS UdnRequestorProcess(PIRP Irp)
{
    return Irp->Tail.Overlay.Thread == PsGetCurrentThread() ? IoGetCurrentProcess() : (PEPROCESS)0;
}
#define IoGetDeviceToVerify(T)      ((PDEVICE_OBJECT)0)
#define IoSetDeviceToVerify(T, D)   ((void)0)
#endif

/* ------------------------------------------------------------------ */
/* Target configuration                                                */
/* ------------------------------------------------------------------ */

/*
 * The NT4 DDK IO_STACK_LOCATION lacks the file system members
 * (QueryDirectory, NotifyDirectory, FileSystemControl, LockControl);
 * the free ntifs.h provides them through EXTENDED_IO_STACK_LOCATION.
 */
#ifdef UDN_NT4
#define UdnXSp(IrpSp)           ((PEXTENDED_IO_STACK_LOCATION)(IrpSp))
#define UdnMdlAddress(Mdl)      MmGetSystemAddressForMdl(Mdl)
#define UDN_STATUS_DISMOUNTED   STATUS_FILE_INVALID
#else
#define UdnXSp(IrpSp)           (IrpSp)
#define UdnMdlAddress(Mdl)      MmGetSystemAddressForMdlSafe((Mdl), NormalPagePriority)
#define UDN_STATUS_DISMOUNTED   STATUS_VOLUME_DISMOUNTED
#endif

/* Same as the W2K IoSkipCurrentIrpStackLocation, which NT4 lacks */
#define UdnSkipStack(Irp) {                         \
    (Irp)->CurrentLocation++;                       \
    (Irp)->Tail.Overlay.CurrentStackLocation++;     \
}

#define UdnCopyStackToNext(Irp) {                                           \
    PIO_STACK_LOCATION _Sp = IoGetCurrentIrpStackLocation(Irp);            \
    PIO_STACK_LOCATION _Next = IoGetNextIrpStackLocation(Irp);             \
    RtlCopyMemory(_Next, _Sp, FIELD_OFFSET(IO_STACK_LOCATION, CompletionRoutine)); \
    _Next->Control = 0;                                                     \
}

/* ExReleaseResourceLite is not exported by every NT4 build */
#define UdnRelease(Resource) \
    ExReleaseResourceForThreadLite((Resource), ExGetCurrentResourceThread())

#ifndef VPB_LOCKED
#define VPB_LOCKED                  0x00000002
#endif

/* File system IRP details some DDK headers leave out */
#ifndef IRP_MN_QUERY_DIRECTORY
#define IRP_MN_QUERY_DIRECTORY          0x01
#endif
#ifndef IRP_MN_NOTIFY_CHANGE_DIRECTORY
#define IRP_MN_NOTIFY_CHANGE_DIRECTORY  0x02
#endif
#ifndef IRP_MN_USER_FS_REQUEST
#define IRP_MN_USER_FS_REQUEST          0x00
#endif
#ifndef IRP_MN_MOUNT_VOLUME
#define IRP_MN_MOUNT_VOLUME             0x01
#endif
#ifndef IRP_MN_VERIFY_VOLUME
#define IRP_MN_VERIFY_VOLUME            0x02
#endif
#ifndef SL_RESTART_SCAN
#define SL_RESTART_SCAN                 0x01
#endif
#ifndef SL_RETURN_SINGLE_ENTRY
#define SL_RETURN_SINGLE_ENTRY          0x02
#endif
#ifndef SL_INDEX_SPECIFIED
#define SL_INDEX_SPECIFIED              0x04
#endif
#ifndef SL_WATCH_TREE
#define SL_WATCH_TREE                   0x01
#endif
#ifndef SL_OPEN_PAGING_FILE
#define SL_OPEN_PAGING_FILE             0x02
#endif
#ifndef SL_OPEN_TARGET_DIRECTORY
#define SL_OPEN_TARGET_DIRECTORY        0x04
#endif
#ifndef SL_OVERRIDE_VERIFY_VOLUME
#define SL_OVERRIDE_VERIFY_VOLUME       0x02
#endif

#ifndef IO_REMOUNT
#define IO_REMOUNT                  0x00000001
#endif

#ifndef FILE_READ_ONLY_VOLUME
#define FILE_READ_ONLY_VOLUME       0x00080000
#endif

#ifndef FSCTL_IS_VOLUME_DIRTY
#define FSCTL_IS_VOLUME_DIRTY       CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 30, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#ifndef VOLUME_IS_DIRTY
#define VOLUME_IS_DIRTY             0x00000001
#endif

#ifndef FILE_WRITE_TO_END_OF_FILE
#define FILE_WRITE_TO_END_OF_FILE   0xffffffff
#endif

#ifndef IOCTL_DISK_IS_WRITABLE
#define IOCTL_DISK_IS_WRITABLE      CTL_CODE(IOCTL_DISK_BASE, 0x0009, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

/* Status codes newer than some DDK headers */
#ifndef STATUS_USER_MAPPED_FILE
#define STATUS_USER_MAPPED_FILE     ((NTSTATUS)0xC0000243L)
#endif
#ifndef STATUS_CANNOT_MAKE
#define STATUS_CANNOT_MAKE          ((NTSTATUS)0xC000016BL)
#endif

/* Information classes newer than NT4, answered on both targets */
#define UDN_CLASS_ATTRIBUTE_TAG             35
#define UDN_CLASS_FS_FULL_SIZE              7

typedef struct _UDN_FILE_ATTRIBUTE_TAG_INFORMATION {
    ULONG       FileAttributes;
    ULONG       ReparseTag;
} UDN_FILE_ATTRIBUTE_TAG_INFORMATION, *PUDN_FILE_ATTRIBUTE_TAG_INFORMATION;

typedef struct _UDN_FILE_FS_FULL_SIZE_INFORMATION {
    LARGE_INTEGER TotalAllocationUnits;
    LARGE_INTEGER CallerAvailableAllocationUnits;
    LARGE_INTEGER ActualAvailableAllocationUnits;
    ULONG       SectorsPerAllocationUnit;
    ULONG       BytesPerSector;
} UDN_FILE_FS_FULL_SIZE_INFORMATION, *PUDN_FILE_FS_FULL_SIZE_INFORMATION;

/* ------------------------------------------------------------------ */
/* Debug output                                                        */
/* ------------------------------------------------------------------ */

#define UDN_PFX                 "[UDFNT] "

#if defined(UDN_DBGPORT)
VOID     UdnDbgPrintf(const char *Format, ...);
#define UDN_DBG(args)           UdnDbgPrintf args
#elif DBG || defined(UDN_DEBUG)
#define UDN_DBG(args)           DbgPrint args
#else
#define UDN_DBG(args)
#endif

/*
 * Debug trace to the QEMU "debugcon" port (0xE9): readable bytes with no
 * kernel-debugger protocol. Boot QEMU with -debugcon file:/path. Compile
 * with -DEXN_DBGPORT to enable; a no-op otherwise.
 */
#ifdef UDN_DBGPORT
static __inline void UdnDbgCh(unsigned char c)
{ unsigned short port = 0x00E9;
  __asm__ __volatile__ ("outb %b0, %w1" : : "a"(c), "d"(port)); }
static __inline void UdnDbgStr(const char *s)
{ while (*s) UdnDbgCh((unsigned char)*s++); }
static __inline void UdnDbgHex(ULONG_PTR v)
{ int i, n = (int)(sizeof(v) * 2); char h[17]; const char *d="0123456789abcdef";
  for (i=n-1;i>=0;i--){ h[i]=d[v&0xF]; v>>=4; } h[n]=0; UdnDbgStr(h); }
static __inline void UdnDbgUni(const void *us)
{ const unsigned short *p; unsigned short len, i;
  len = ((const UNICODE_STRING *)us)->Length;
  p   = (const unsigned short *)((const UNICODE_STRING *)us)->Buffer;
  if (!p) { UdnDbgStr("(null)"); return; }
  for (i=0;i<len/2 && i<120;i++) UdnDbgCh((unsigned char)(p[i]<128?p[i]:'?')); }
#define DBGP(s)     UdnDbgStr(s)
#define DBGX(v)     UdnDbgHex((ULONG_PTR)(v))
#define DBGU(u)     UdnDbgUni(&(u))
#else
#define DBGP(s)     ((void)0)
#define DBGX(v)     ((void)0)
#define DBGU(u)     ((void)0)
#endif


/* ------------------------------------------------------------------ */
/* Structured exception handling                                       */
/* ------------------------------------------------------------------ */

/*
 * The Cache Manager, the Memory Manager and the probe routines report I/O
 * errors and bad user buffers by raising. Code that calls them runs that
 * part through UdnTryCall (exnseh.c), which returns the exception code
 * (STATUS_SUCCESS when nothing was raised). With ExpectedOnly only the
 * codes FsRtlIsNtstatusExpected accepts (and in-page errors) are caught;
 * anything else is a bug and stops the system where it happened.
 */
typedef VOID (*UDN_TRY_BODY)(PVOID Context);

NTSTATUS UdnTryCall(UDN_TRY_BODY Body, PVOID Context, BOOLEAN ExpectedOnly);

/* RtlCopyMemory to or from a caller's buffer, faults caught (mapped status) */
NTSTATUS UdnSafeCopy(PVOID Destination, const VOID *Source, SIZE_T Length);

/* Maps an exception code to the status a request returns */
NTSTATUS UdnExceptionStatus(NTSTATUS Code);

/* ------------------------------------------------------------------ */
/* Limits and tags                                                     */
/* ------------------------------------------------------------------ */

#define UDN_FS_DEVICE_NAME      L"\\Udfnt"
#define UDN_CDFS_DEVICE_NAME    L"\\UdfntCdrom"
#define UDN_TAG                 'ntdU'
#define UDN_TAG_FCB             'Fndu'
#define UDN_TAG_CCB             'Cndu'
#define UDN_TAG_RUNS            'Rndu'
#define UDN_TAG_BUFFER          'Bndu'
#define UDN_TAG_NAME            'Nndu'
#define UDN_TAG_WORK            'Wndu'
#define UDN_TAG_META            'Mndu'
#define UDN_TAG_EA              'Endu'
#define UDN_TAG_ENGINE          'endu'
#define UDN_TAG_INDEX           'Indu'
#define UDN_TAG_REGISTRY        'Gndu'

#define UDN_PAGE_SIZE           0x1000
#define UDN_ZERO_CHUNK          0x10000
#define UDN_MAX_LABEL           32          /* NT's longest label (UDF keeps up to 126 characters) */
#define UDN_MAX_NAME            UFX_MAX_NAME   /* bytes of UTF-8 */
#define UDN_SECTOR_SIZE         (Vcb->SectorSize)       /* the device's sector (512 - 4096) */
#define UDN_SECTOR_SHIFT        (Vcb->SectorShift)
#define UDN_MAX_LINK_DEPTH      8           /* symbolic links followed per open */

/* NT attribute bits we keep (the rest are computed) */
#define UDN_ATTR_SETTABLE       (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | \
                                 FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE)

/* ------------------------------------------------------------------ */
/* Node types                                                          */
/* ------------------------------------------------------------------ */

#define UDN_NTC_VCB             ((CSHORT)0x0E21)
#define UDN_NTC_FCB             ((CSHORT)0x0E22)    /* file (or symbolic link opened as itself) */
#define UDN_NTC_DCB             ((CSHORT)0x0E23)    /* directory */
#define UDN_NTC_VFCB            ((CSHORT)0x0E24)    /* whole volume (DASD) */
#define UDN_NTC_CCB             ((CSHORT)0x0E25)

#define UdnNodeType(Ptr)        (*((CSHORT *)(Ptr)))

/* ------------------------------------------------------------------ */
/* Structures                                                          */
/* ------------------------------------------------------------------ */

typedef struct _UDN_VCB UDN_VCB, *PUDN_VCB;

/*
 * NT 3.1's ERESOURCE is larger than later headers describe:
 * every embedded ERESOURCE is followed by padding.
 */
#define UDN_ERESOURCE_PAD       0x40

/* A run of a paging file (kept nonpaged: paging I/O never enters the engine) */
typedef struct _UDN_RUN {
    ULONGLONG   FileBlock;
    ULONGLONG   DiskBlock;
    ULONG       Length;
} UDN_RUN, *PUDN_RUN;

/* Case-insensitive name index of a big directory (see extdctl.c) */
typedef struct _UDN_NAME_INDEX UDN_NAME_INDEX, *PUDN_NAME_INDEX;

typedef struct _UDN_FCB {
    FSRTL_COMMON_FCB_HEADER Header;
    SECTION_OBJECT_POINTERS SectionObjectPointers;
    ERESOURCE   Resource;
    UCHAR       ResourcePad[UDN_ERESOURCE_PAD];
    ERESOURCE   PagingIoResource;
    UCHAR       PagingIoResourcePad[UDN_ERESOURCE_PAD];
    PUDN_VCB    Vcb;
    struct _UDN_FCB *ParentDcb;     /* where it was first opened (directories: the parent) */
    LIST_ENTRY  FcbLinks;
    ULONG       RefCount;           /* file objects + child FCBs */
    ULONG       UncleanCount;
    ULONG       NonCachedUncleanCount;
    ULONG       FcbState;
    SHARE_ACCESS ShareAccess;
    FILE_LOCK   FileLock;
    ULONG       FileLockSpare[16];  /* NT 3.1's FILE_LOCK is larger than the headers' */
    PKTHREAD    LazyWriteThread;

    ULONG       Ino;                /* the file's identity */
    UFX_INODE   Inode;              /* in-memory copy (under the engine) */
    ULONG       Attributes;         /* NT attributes */
    ULONG       DosAttributes;      /* kept in user.DOSATTRIB: hidden/system/archive */
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER ChangeTime;
    UNICODE_STRING Name;            /* last component as NT sees it */
    UNICODE_STRING FullName;        /* built on demand */
    ULONG       UfxNameLength;      /* the name as stored (UTF-8 bytes) */
    UCHAR       UfxName[UDN_MAX_NAME + 1];

    /* last mapping, so sequential paging I/O does not walk the tree each time
       (under the paging resource; cleared whenever the map changes) */
    ULONGLONG   MapFileBlock;
    ULONGLONG   MapDiskBlock;
    ULONG       MapRun;             /* 0 = nothing cached */
    BOOLEAN     MapHole;

    /* a paging file's whole map */
    PUDN_RUN    Runs;
    ULONG       RunCount;

    PUDN_NAME_INDEX NameIndex;      /* directories */

    /* the name a pending delete removes (the handle that asked for it:
       with hard links that is not necessarily the FCB's own name) */
    ULONG       DeleteParentIno;
    ULONG       DeleteNameLength;
    UCHAR       DeleteName[UDN_MAX_NAME + 1];
} UDN_FCB, *PUDN_FCB;

#define FCB_STATE_ROOT              0x0001
#define FCB_STATE_DELETE_PENDING    0x0004
#define FCB_STATE_DELETED           0x0008  /* off the disk, kept for Cc and Mm */
#define FCB_STATE_INODE_DIRTY       0x0010  /* the inode lags behind the FCB */
#define FCB_STATE_TRUNCATE_ON_CLOSE 0x0020
#define FCB_STATE_SYMLINK           0x0040  /* a symbolic link opened as itself */
#define FCB_STATE_PAGING_FILE       0x0200
#define FCB_STATE_VISITED           0x0400  /* volume flush walk */

typedef struct _UDN_CCB {
    CSHORT      NodeTypeCode;
    CSHORT      NodeByteSize;
    ULONG       Flags;
    ULONG       QueryState;         /* 0: ".", 1: "..", 2: entries */
    ULONGLONG   Cookie;             /* directory position of the next entry */
    ULONG       EaIndex;            /* next EA for a query-EA continuation */
    UNICODE_STRING Pattern;         /* upcased */
    /* the name this handle was opened under (hard links: delete/rename act on it) */
    ULONG       ParentIno;
    ULONG       NameLength;
    UCHAR       Name[UDN_MAX_NAME + 1];
} UDN_CCB, *PUDN_CCB;

#define CCB_FLAG_PATTERN_SET        0x0001
#define CCB_FLAG_WILDCARD           0x0002
#define CCB_FLAG_MATCH_ALL          0x0004
#define CCB_FLAG_VOLUME_OPEN        0x0008
#define CCB_FLAG_DISMOUNTED_VOLUME  0x0010
#define CCB_FLAG_DELETE_ON_CLOSE    0x0020
#define CCB_FLAG_USER_SET_WRITE     0x0040
#define CCB_FLAG_USER_SET_ACCESS    0x0080
#define CCB_FLAG_WRITE_HANDLE       0x0100
#define CCB_FLAG_HAVE_NAME          0x0200

#define UDN_COMMIT_SECONDS      5                   /* transaction commit interval */

struct _UDN_VCB {
    CSHORT      NodeTypeCode;
    CSHORT      NodeByteSize;
    ERESOURCE   Resource;
    UCHAR       ResourcePad[UDN_ERESOURCE_PAD];
    ERESOURCE   EngineResource;     /* one thread in the engine at a time */
    UCHAR       EngineResourcePad[UDN_ERESOURCE_PAD];
    LIST_ENTRY  VcbLinks;
    PVPB        Vpb;
    PDEVICE_OBJECT TargetDeviceObject;
    PDEVICE_OBJECT VolumeDeviceObject;
    ULONG       VcbState;
    ULONG       OpenCount;
    ULONG       UncleanCount;
    ULONG       WriteCount;
    PFILE_OBJECT LockFileObject;
    PKTHREAD    VerifyThread;
    PUDN_FCB    VolumeFcb;
    PUDN_FCB    RootDcb;
    LIST_ENTRY  FcbList;

    ULONG       SectorSize;         /* the device's */
    ULONG       SectorShift;
    BOOLEAN     Optical;            /* a CD/DVD/BD drive */
    UFX_VOL     Vol;                /* engine state */
    LONGLONG    VolumeBytes;        /* file system size */
    LONGLONG    PartitionBytes;
    ULONG       SerialNumber;
    BOOLEAN     Writing;            /* UfxStartWriting done: the volume is in use */
    BOOLEAN     BootView;           /* udfboot made it bootable: keep its FAT view current */
    ULONG       LabelLength;        /* characters */
    WCHAR       Label[UDN_MAX_LABEL];

    ULONG       Commits;            /* transactions written */
    PUCHAR      Bounce;             /* the engine's staging buffer (nonpaged; under EngineResource) */

    /* Periodic journal commit */
    KTIMER      CommitTimer;
    KDPC        CommitDpc;
    WORK_QUEUE_ITEM CommitItem;
    LONG        CommitQueued;
    BOOLEAN     TimerSet;
    KEVENT      CommitIdle;         /* signalled when no commit item is pending */

    PNOTIFY_SYNC NotifySync;
    LIST_ENTRY  DirNotifyList;
};

#define VCB_STATE_MOUNTED           0x0001
#define VCB_STATE_LOCKED            0x0002
#define VCB_STATE_DISMOUNTED        0x0004
#define VCB_STATE_IN_DISMOUNT       0x0008
#define VCB_STATE_DELETE_PENDING    0x0010
#define VCB_STATE_DASD_WRITTEN      0x0020
#define VCB_STATE_REMOVABLE         0x0040
#define VCB_STATE_PNP_LOCKED        0x0080
#define VCB_STATE_FREE_VPB          0x0100
#define VCB_STATE_REMOVED           0x0200
#define VCB_STATE_READ_ONLY         0x0400
#define VCB_STATE_FLUSH_ON_CLOSE    0x1000
#define VCB_STATE_SHUTDOWN          0x2000

typedef struct _UDN_DATA {
    PDRIVER_OBJECT  DriverObject;
    PDEVICE_OBJECT  FileSystemDeviceObject;
    PDEVICE_OBJECT  CdromDeviceObject;      /* the same, for CD / DVD / BD drives */
    ERESOURCE       Resource;
    UCHAR           ResourcePad[UDN_ERESOURCE_PAD];
    LIST_ENTRY      VcbList;
    FAST_IO_DISPATCH FastIoDispatch;
    CACHE_MANAGER_CALLBACKS CacheManagerCallbacks;
    UNICODE_STRING  RegistryPath;
    BOOLEAN         HideDotFiles;
    BOOLEAN         MountOptical;   /* registry: mount UDF discs in CD / DVD / BD drives */
    BOOLEAN         FollowLinks;    /* registry: follow symbolic links on open */
    BOOLEAN         CaseSensitive;  /* registry: exact-case lookups only */
    BOOLEAN         Writable;       /* registry: allow writing at all */
    ULONG           DefaultUid, DefaultGid;
    ULONG           FileMode, DirMode;  /* permission bits of new files / dirs */
} UDN_DATA, *PUDN_DATA;

extern UDN_DATA UdnData;

typedef struct _UDN_IRP_CONTEXT {
    PIRP            Irp;
    PIO_STACK_LOCATION IrpSp;
    PDEVICE_OBJECT  DeviceObject;
    PUDN_VCB        Vcb;
    ULONG           Flags;
    NTSTATUS        ExceptionStatus;
    NTSTATUS        DispatchStatus;
} UDN_IRP_CONTEXT, *PUDN_IRP_CONTEXT;

#define UDN_CTX_TOP_LEVEL           0x0001
#define UDN_CTX_NO_COMPLETE         0x0002

typedef struct _UDN_CLOSE_ITEM {
    WORK_QUEUE_ITEM Item;
    PUDN_VCB    Vcb;
    PUDN_FCB    Fcb;
    PUDN_CCB    Ccb;
} UDN_CLOSE_ITEM, *PUDN_CLOSE_ITEM;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

#define UdnIsFcb(Fcb)       ((Fcb)->Header.NodeTypeCode == UDN_NTC_FCB)
#define UdnIsDcb(Fcb)       ((Fcb)->Header.NodeTypeCode == UDN_NTC_DCB)
#define UdnIsVfcb(Fcb)      ((Fcb)->Header.NodeTypeCode == UDN_NTC_VFCB)

#define UdnRoundUp(Value, Size) \
    (((Value) + ((Size) - 1)) & ~((LONGLONG)(Size) - 1))

#define UDN_WRITE_ACCESS    (FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | \
                             FILE_WRITE_ATTRIBUTES | DELETE | FILE_DELETE_CHILD)

/* the largest file the volume's mapping can describe */
#define UdnMaxFileSize(Vcb) (0x7FFFFFFFFFFFF000ULL)    /* UDF: 2^64 bytes; NT: 2^63 */

/* engine status -> NT status (most engine codes are NT codes already) */
NTSTATUS UdnStatus(UFX_STATUS S);

/* ------------------------------------------------------------------ */
/* Prototypes                                                          */
/* ------------------------------------------------------------------ */

/* extinit.c */
NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath);
BOOLEAN UdnWriteSupportEnabled(VOID);

/* extdisp.c */
NTSTATUS NTAPI UdnFsdDispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS UdnVerifyVcb(PUDN_IRP_CONTEXT Ctx, PUDN_VCB Vcb);
NTSTATUS UdnVerifyWritable(PUDN_IRP_CONTEXT Ctx, PUDN_VCB Vcb);

/* extcrea.c */
NTSTATUS UdnCommonCreate(PUDN_IRP_CONTEXT Ctx);

/* extclos.c */
NTSTATUS UdnCommonCleanup(PUDN_IRP_CONTEXT Ctx);
NTSTATUS UdnCommonClose(PUDN_IRP_CONTEXT Ctx);
BOOLEAN  UdnTryTeardown(PUDN_VCB Vcb, ULONG InFlightReferences);
VOID     UdnDeleteVcb(PUDN_VCB Vcb);
BOOLEAN  UdnDismountVcb(PUDN_VCB Vcb);
VOID     UdnUnlockVcb(PUDN_VCB Vcb);
VOID     UdnPurgeCachedFiles(PUDN_VCB Vcb);
NTSTATUS UdnUnlinkName(PUDN_VCB Vcb, PUDN_FCB Fcb);
VOID     UdnSetDeleteName(PUDN_FCB Fcb, PUDN_CCB Ccb);
PUFX_INODE UdnDirInode(PUDN_VCB Vcb, ULONG Ino, PUFX_INODE Temp);

/* extread.c / extwrit.c */
NTSTATUS UdnCommonRead(PUDN_IRP_CONTEXT Ctx);
NTSTATUS UdnCommonWrite(PUDN_IRP_CONTEXT Ctx);
NTSTATUS UdnZeroFileRange(PUDN_FCB Fcb, PFILE_OBJECT FileObject, LONGLONG Start, LONGLONG End);

/* extdctl.c */
NTSTATUS UdnCommonDirectoryControl(PUDN_IRP_CONTEXT Ctx);
VOID     UdnNotifyCleanupList(PUDN_VCB Vcb, PUDN_CCB Ccb);
VOID     UdnNotifyChange(PUDN_VCB Vcb, PUDN_FCB Fcb, ULONG Filter, ULONG Action);
NTSTATUS UdnLookupName(PUDN_VCB Vcb, PUDN_FCB Dir, PCUNICODE_STRING Name, BOOLEAN CaseSensitive,
                       PUFX_ENTRY Entry);
VOID     UdnIndexAdd(PUDN_FCB Dir, const UCHAR *Name, ULONG Length, ULONG Ino);
VOID     UdnIndexRemove(PUDN_FCB Dir, const UCHAR *Name, ULONG Length);
VOID     UdnIndexFree(PUDN_FCB Dir);

/* extinfo.c */
NTSTATUS UdnCommonQueryInformation(PUDN_IRP_CONTEXT Ctx);
ULONG    UdnNtAttributes(ULONG Attributes);
VOID     UdnFillBasicInfo(PUDN_FCB Fcb, PFILE_BASIC_INFORMATION Info);
VOID     UdnFillStandardInfo(PUDN_FCB Fcb, PFILE_STANDARD_INFORMATION Info);
VOID     UdnFillNetworkOpenInfo(PUDN_FCB Fcb, PFILE_NETWORK_OPEN_INFORMATION Info);

/* extseti.c */
NTSTATUS UdnCommonSetInformation(PUDN_IRP_CONTEXT Ctx);
NTSTATUS UdnSetFileSize(PUDN_IRP_CONTEXT Ctx, PUDN_FCB Fcb, PFILE_OBJECT FileObject, LONGLONG NewSize);

/* exteas.c */
NTSTATUS UdnCommonQueryEa(PUDN_IRP_CONTEXT Ctx);
NTSTATUS UdnCommonSetEa(PUDN_IRP_CONTEXT Ctx);
NTSTATUS UdnSetEaBuffer(PUDN_VCB Vcb, PUDN_FCB Fcb, PFILE_FULL_EA_INFORMATION Buffer, ULONG Length,
                        BOOLEAN Replace);
ULONG    UdnNtEaSize(PUDN_VCB Vcb, PUDN_FCB Fcb);
NTSTATUS UdnCheckEaBuffer(PFILE_FULL_EA_INFORMATION Buffer, ULONG Length, PULONG ErrorOffset);

/* extvol.c */
NTSTATUS UdnCommonQueryVolumeInformation(PUDN_IRP_CONTEXT Ctx);
NTSTATUS UdnCommonSetVolumeInformation(PUDN_IRP_CONTEXT Ctx);

/* extfsct.c */
NTSTATUS UdnCommonFileSystemControl(PUDN_IRP_CONTEXT Ctx);
NTSTATUS UdnCommonPnp(PUDN_IRP_CONTEXT Ctx);
NTSTATUS UdnUserFsRequest(PUDN_IRP_CONTEXT Ctx);
VOID     UdnLoadLabel(PUDN_VCB Vcb);
NTSTATUS UdnLoadVolume(PUDN_VCB Vcb);
BOOLEAN  UdnSameVolume(PUDN_VCB Vcb);

/* extletr.c */
VOID     UdnStartDriveLetters(VOID);
VOID     UdnClaimRegistryLetter(PUDN_VCB Vcb, PDEVICE_OBJECT TargetDevice, LONGLONG StartingOffset);

/* extmisc.c */
NTSTATUS UdnCommonDeviceControl(PUDN_IRP_CONTEXT Ctx);
NTSTATUS UdnCommonLockControl(PUDN_IRP_CONTEXT Ctx);

/* extflsh.c */
NTSTATUS UdnCommonFlushBuffers(PUDN_IRP_CONTEXT Ctx);
NTSTATUS UdnCommonShutdown(PUDN_IRP_CONTEXT Ctx);
NTSTATUS UdnFlushFile(PUDN_VCB Vcb, PUDN_FCB Fcb);
NTSTATUS UdnFlushVolume(PUDN_VCB Vcb, BOOLEAN Final);
NTSTATUS UdnStartWriting(PUDN_VCB Vcb);
NTSTATUS UdnStopWriting(PUDN_VCB Vcb);
VOID     UdnResumeWriting(PUDN_VCB Vcb);
VOID     UdnUpdateBootView(PUDN_VCB Vcb);

/* extfast.c */
VOID     UdnInitializeFastIo(PFAST_IO_DISPATCH FastIo);
VOID     UdnInitializeCacheCallbacks(VOID);
UCHAR    UdnIsFastIoPossible(PUDN_FCB Fcb);

/* extmeta.c: the engine's host and the metadata cache */
VOID     UdnEngineEnter(PUDN_VCB Vcb);
NTSTATUS UdnEngineLeave(PUDN_VCB Vcb, NTSTATUS Status);
#ifdef UDN_NT31
extern BOOLEAN UdnNt31;                       /* running on NT 3.1 (extres.c) */
#endif       /* extshim view (engine not held) */
NTSTATUS UdnCommit(PUDN_VCB Vcb);              /* journal commit (engine held) */
NTSTATUS UdnCommitNow(PUDN_VCB Vcb);           /* takes the engine itself */
VOID     UdnBounceFree(PUDN_VCB Vcb);           /* the staging buffer goes (volume deleted) */
VOID     UdnMetaPurge(PUDN_VCB Vcb);
VOID     UdnMetaInitialize(PUDN_VCB Vcb);
VOID     UdnCommitTimerStart(PUDN_VCB Vcb);
VOID     UdnCommitTimerStop(PUDN_VCB Vcb);

/* extio.c */
NTSTATUS UdnSyncIo(PDEVICE_OBJECT Device, UCHAR MajorFunction, LONGLONG Offset,
                   ULONG Length, PVOID Buffer, BOOLEAN OverrideVerify);
NTSTATUS UdnDeviceIoctl(PDEVICE_OBJECT Device, ULONG IoControlCode, PVOID InputBuffer,
                        ULONG InputLength, PVOID OutputBuffer, ULONG OutputLength,
                        BOOLEAN OverrideVerify, PULONG Information);
NTSTATUS UdnFlushDevice(PUDN_VCB Vcb);
NTSTATUS UdnLockUserBuffer(PIRP Irp, LOCK_OPERATION Operation, ULONG Length);
PVOID    UdnMapUserBuffer(PIRP Irp);
NTSTATUS UdnNonCachedIo(PUDN_IRP_CONTEXT Ctx, PUDN_FCB Fcb, UCHAR MajorFunction,
                        LONGLONG StartingVbo, ULONG ByteCount, LONGLONG ValidData);
NTSTATUS UdnZeroDisk(PUDN_VCB Vcb, PUDN_FCB Fcb, LONGLONG Start, LONGLONG End);
NTSTATUS UdnVboToLbo(PUDN_VCB Vcb, PUDN_FCB Fcb, LONGLONG Vbo, BOOLEAN Allocate,
                     PLONGLONG Lbo, PULONG Contiguous);
NTSTATUS UdnReadDisk(PUDN_VCB Vcb, ULONGLONG Sector, ULONG Count, PVOID Buffer);
NTSTATUS UdnWriteDisk(PUDN_VCB Vcb, ULONGLONG Sector, ULONG Count, const VOID *Buffer);
NTSTATUS UdnAllocateRange(PUDN_VCB Vcb, PUDN_FCB Fcb, LONGLONG Start, LONGLONG End);
NTSTATUS UdnPrepareForWrite(PUDN_VCB Vcb, PUDN_FCB Fcb);
NTSTATUS UdnWriteData(PUDN_VCB Vcb, ULONGLONG Sector, ULONG Count, const VOID *Buffer);

#define UdnIsFsDevice(D)    ((D) == UdnData.FileSystemDeviceObject || (D) == UdnData.CdromDeviceObject)
#define UdnReadSectors(Device, Offset, Length, Buffer, Override) \
    UdnSyncIo((Device), IRP_MJ_READ, (Offset), (Length), (Buffer), (Override))
#define UdnWriteSectors(Device, Offset, Length, Buffer, Override) \
    UdnSyncIo((Device), IRP_MJ_WRITE, (Offset), (Length), (Buffer), (Override))

/* extstrc.c */
PUDN_FCB UdnCreateVolumeFcb(PUDN_VCB Vcb);
PUDN_FCB UdnCreateRootDcb(PUDN_VCB Vcb);
NTSTATUS UdnCreateFcb(PUDN_VCB Vcb, PUDN_FCB ParentDcb, PUFX_ENTRY Entry, BOOLEAN AsLink, PUDN_FCB *Fcb);
VOID     UdnDeleteFcb(PUDN_FCB Fcb);
VOID     UdnDereferenceFcb(PUDN_VCB Vcb, PUDN_FCB Fcb);
PUDN_FCB UdnFindFcbByIno(PUDN_VCB Vcb, ULONG Ino, BOOLEAN AsLink);
PUDN_CCB UdnCreateCcb(VOID);
VOID     UdnDeleteCcb(PUDN_CCB Ccb);
NTSTATUS UdnBuildFullName(PUDN_FCB Fcb);
VOID     UdnForgetNames(PUDN_VCB Vcb, PUDN_FCB Fcb);
BOOLEAN  UdnIsAncestor(PUDN_FCB Ancestor, PUDN_FCB Fcb);
LARGE_INTEGER UdnToNtTime(ULONG Seconds, ULONG Extra);
VOID     UdnFromNtTime(LARGE_INTEGER Time, PULONG Seconds, PULONG Extra);
NTSTATUS UdnLoadInode(PUDN_VCB Vcb, PUDN_FCB Fcb);
VOID     UdnInodeToFcb(PUDN_VCB Vcb, PUDN_FCB Fcb);
NTSTATUS UdnUpdateInode(PUDN_VCB Vcb, PUDN_FCB Fcb);
NTSTATUS UdnSetAllocation(PUDN_VCB Vcb, PUDN_FCB Fcb, ULONGLONG Bytes);
NTSTATUS UdnLoadRuns(PUDN_VCB Vcb, PUDN_FCB Fcb);
VOID     UdnForgetMapping(PUDN_FCB Fcb);
NTSTATUS UdnUnicodeToExt(PCUNICODE_STRING Name, PUCHAR Out, PULONG OutLength);
NTSTATUS UdnExtToUnicode(const UCHAR *In, ULONG InLength, PUNICODE_STRING Name);
BOOLEAN  UdnIsValidUnicodeName(PCUNICODE_STRING Name);
ULONG    UdnComputeAttributes(PUDN_VCB Vcb, PUFX_INODE Inode, const UCHAR *Name, ULONG NameLength,
                              ULONG DosAttributes);
ULONG    UdnLoadDosAttributes(PUDN_VCB Vcb, ULONG Ino, PUFX_INODE Inode);
NTSTATUS UdnStoreDosAttributes(PUDN_VCB Vcb, PUDN_FCB Fcb, ULONG Attributes);

#endif /* _UDN_H_ */
