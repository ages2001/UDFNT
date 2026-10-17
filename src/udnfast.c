/*
 * UDFNT - fast I/O and cache manager callbacks
 */

#include "udn.h"

static BOOLEAN
NTAPI
UdnFastIoNoWrite (
    PFILE_OBJECT FileObject,
    PLARGE_INTEGER FileOffset,
    ULONG Length,
    BOOLEAN Wait,
    ULONG LockKey,
    PVOID Buffer,
    PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject
    )
{
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(FileOffset);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(Wait);
    UNREFERENCED_PARAMETER(LockKey);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(IoStatus);
    UNREFERENCED_PARAMETER(DeviceObject);
    return FALSE;
}

UCHAR
UdnIsFastIoPossible (
    PUDN_FCB Fcb
    )
{
    if (Fcb->Vcb->VcbState & VCB_STATE_DISMOUNTED) {
        return FastIoIsNotPossible;
    }

    if (FsRtlAreThereCurrentFileLocks(&Fcb->FileLock)) {
        return FastIoIsQuestionable;
    }

    return FastIoIsPossible;
}

static BOOLEAN
UdnFastIoUsable (
    PUDN_FCB Fcb
    )
{
    if (Fcb == NULL || (!UdnIsFcb(Fcb) && !UdnIsDcb(Fcb))) {
        return FALSE;
    }

    if ((Fcb->Vcb->VcbState & VCB_STATE_DISMOUNTED) ||
        (Fcb->Vcb->Vpb->RealDevice->Flags & DO_VERIFY_VOLUME)) {

        return FALSE;
    }

    return TRUE;
}

static BOOLEAN
NTAPI
UdnFastIoCheckIfPossible (
    PFILE_OBJECT FileObject,
    PLARGE_INTEGER FileOffset,
    ULONG Length,
    BOOLEAN Wait,
    ULONG LockKey,
    BOOLEAN CheckForReadOperation,
    PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject
    )
{
    PUDN_FCB Fcb = (PUDN_FCB)FileObject->FsContext;
    LARGE_INTEGER LargeLength;

    UNREFERENCED_PARAMETER(Wait);
    UNREFERENCED_PARAMETER(IoStatus);
    UNREFERENCED_PARAMETER(DeviceObject);

    if (!UdnFastIoUsable(Fcb) || !UdnIsFcb(Fcb)) {
        return FALSE;
    }

    LargeLength.QuadPart = Length;

    if (CheckForReadOperation) {
        return FsRtlFastCheckLockForRead(&Fcb->FileLock, FileOffset, &LargeLength, LockKey,
                                         FileObject, IoGetCurrentProcess());
    }

    if (Fcb->Vcb->VcbState & VCB_STATE_READ_ONLY) {
        return FALSE;
    }

    return FsRtlFastCheckLockForWrite(&Fcb->FileLock, FileOffset, &LargeLength, LockKey,
                                      FileObject, IoGetCurrentProcess());
}

static BOOLEAN
NTAPI
UdnFastQueryBasicInfo (
    PFILE_OBJECT FileObject,
    BOOLEAN Wait,
    PFILE_BASIC_INFORMATION Buffer,
    PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject
    )
{
    PUDN_FCB Fcb = (PUDN_FCB)FileObject->FsContext;
    FILE_BASIC_INFORMATION Info;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!UdnFastIoUsable(Fcb)) {
        return FALSE;
    }

    FsRtlEnterFileSystem();

    if (!ExAcquireResourceSharedLite(&Fcb->Resource, Wait)) {
        FsRtlExitFileSystem();
        return FALSE;
    }

    UdnFillBasicInfo(Fcb, &Info);

    UdnRelease(&Fcb->Resource);
    FsRtlExitFileSystem();

    *Buffer = Info;
    IoStatus->Status = STATUS_SUCCESS;
    IoStatus->Information = sizeof(FILE_BASIC_INFORMATION);

    return TRUE;
}

static BOOLEAN
NTAPI
UdnFastQueryStandardInfo (
    PFILE_OBJECT FileObject,
    BOOLEAN Wait,
    PFILE_STANDARD_INFORMATION Buffer,
    PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject
    )
{
    PUDN_FCB Fcb = (PUDN_FCB)FileObject->FsContext;
    FILE_STANDARD_INFORMATION Info;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!UdnFastIoUsable(Fcb)) {
        return FALSE;
    }

    FsRtlEnterFileSystem();

    if (!ExAcquireResourceSharedLite(&Fcb->Resource, Wait)) {
        FsRtlExitFileSystem();
        return FALSE;
    }

    UdnFillStandardInfo(Fcb, &Info);

    UdnRelease(&Fcb->Resource);
    FsRtlExitFileSystem();

    *Buffer = Info;
    IoStatus->Status = STATUS_SUCCESS;
    IoStatus->Information = sizeof(FILE_STANDARD_INFORMATION);

    return TRUE;
}

static BOOLEAN
NTAPI
UdnFastQueryNetworkOpenInfo (
    PFILE_OBJECT FileObject,
    BOOLEAN Wait,
    PFILE_NETWORK_OPEN_INFORMATION Buffer,
    PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject
    )
{
    PUDN_FCB Fcb = (PUDN_FCB)FileObject->FsContext;
    FILE_NETWORK_OPEN_INFORMATION Info;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!UdnFastIoUsable(Fcb)) {
        return FALSE;
    }

    FsRtlEnterFileSystem();

    if (!ExAcquireResourceSharedLite(&Fcb->Resource, Wait)) {
        FsRtlExitFileSystem();
        return FALSE;
    }

    UdnFillNetworkOpenInfo(Fcb, &Info);

    UdnRelease(&Fcb->Resource);
    FsRtlExitFileSystem();

    *Buffer = Info;
    IoStatus->Status = STATUS_SUCCESS;
    IoStatus->Information = sizeof(FILE_NETWORK_OPEN_INFORMATION);

    return TRUE;
}

#ifdef UDN_NT31
/*
 * NT 3.1 calls the fast I/O routines without the DeviceObject parameter that
 * NT 3.5 added: these routines pop what it pushes (stdcall), or the caller's
 * stack - and the registers it restores from it - would be off by four bytes.
 */
static BOOLEAN NTAPI
UdnFastIoCheckIfPossible31 (PFILE_OBJECT FileObject, PLARGE_INTEGER FileOffset, ULONG Length, BOOLEAN Wait,
                            ULONG LockKey, BOOLEAN CheckForReadOperation, PIO_STATUS_BLOCK IoStatus)
{
    return UdnFastIoCheckIfPossible(FileObject, FileOffset, Length, Wait, LockKey, CheckForReadOperation, IoStatus, NULL);
}

static BOOLEAN NTAPI
UdnFastIoNoWrite31 (PFILE_OBJECT FileObject, PLARGE_INTEGER FileOffset, ULONG Length, BOOLEAN Wait,
                    ULONG LockKey, PVOID Buffer, PIO_STATUS_BLOCK IoStatus)
{
    return UdnFastIoNoWrite(FileObject, FileOffset, Length, Wait, LockKey, Buffer, IoStatus, NULL);
}

static BOOLEAN NTAPI
UdnFastQueryBasicInfo31 (PFILE_OBJECT FileObject, BOOLEAN Wait, PFILE_BASIC_INFORMATION Buffer, PIO_STATUS_BLOCK IoStatus)
{
    return UdnFastQueryBasicInfo(FileObject, Wait, Buffer, IoStatus, NULL);
}

static BOOLEAN NTAPI
UdnFastQueryStandardInfo31 (PFILE_OBJECT FileObject, BOOLEAN Wait, PFILE_STANDARD_INFORMATION Buffer, PIO_STATUS_BLOCK IoStatus)
{
    return UdnFastQueryStandardInfo(FileObject, Wait, Buffer, IoStatus, NULL);
}
#endif

VOID
UdnInitializeFastIo (
    PFAST_IO_DISPATCH FastIo
    )
{
    RtlZeroMemory(FastIo, sizeof(FAST_IO_DISPATCH));

    FastIo->SizeOfFastIoDispatch = sizeof(FAST_IO_DISPATCH);
    FastIo->FastIoCheckIfPossible = UdnFastIoCheckIfPossible;
    FastIo->FastIoRead = FsRtlCopyRead;
    /* Writes take the IRP path: blocks are found for the data before it
       enters the cache (holes, inline data), so a full disk is reported to
       the writer and not to the lazy writer */
    FastIo->FastIoWrite = UdnFastIoNoWrite;
    FastIo->FastIoQueryBasicInfo = UdnFastQueryBasicInfo;
    FastIo->FastIoQueryStandardInfo = UdnFastQueryStandardInfo;
    FastIo->FastIoQueryNetworkOpenInfo = UdnFastQueryNetworkOpenInfo;
#ifdef UDN_NT31
    if (UdnNt31) {
        /* FsRtlCopyRead is NT 3.1's own: it already takes what NT 3.1 passes */
        FastIo->FastIoCheckIfPossible = (PFAST_IO_CHECK_IF_POSSIBLE)UdnFastIoCheckIfPossible31;
        FastIo->FastIoWrite = (PFAST_IO_WRITE)UdnFastIoNoWrite31;
        FastIo->FastIoQueryBasicInfo = (PFAST_IO_QUERY_BASIC_INFO)UdnFastQueryBasicInfo31;
        FastIo->FastIoQueryStandardInfo = (PFAST_IO_QUERY_STANDARD_INFO)UdnFastQueryStandardInfo31;
        FastIo->FastIoQueryNetworkOpenInfo = NULL;
    }
#endif
}

/* ------------------------------------------------------------------ */
/* Cache manager callbacks                                             */
/* ------------------------------------------------------------------ */

static BOOLEAN
NTAPI
UdnAcquireForLazyWrite (
    PVOID Context,
    BOOLEAN Wait
    )
{
    PUDN_FCB Fcb = (PUDN_FCB)Context;

    FsRtlEnterFileSystem();

    if (!ExAcquireResourceSharedLite(&Fcb->PagingIoResource, Wait)) {
        FsRtlExitFileSystem();
        return FALSE;
    }

    /* Paging writes from this thread must not move VDL */
    Fcb->LazyWriteThread = KeGetCurrentThread();

    if (IoGetTopLevelIrp() == NULL) {
        IoSetTopLevelIrp((PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);
    }

    return TRUE;
}

static VOID
NTAPI
UdnReleaseFromLazyWrite (
    PVOID Context
    )
{
    PUDN_FCB Fcb = (PUDN_FCB)Context;

    if (IoGetTopLevelIrp() == (PIRP)FSRTL_CACHE_TOP_LEVEL_IRP) {
        IoSetTopLevelIrp(NULL);
    }

    Fcb->LazyWriteThread = NULL;

    UdnRelease(&Fcb->PagingIoResource);
    FsRtlExitFileSystem();
}

static BOOLEAN
NTAPI
UdnAcquireForReadAhead (
    PVOID Context,
    BOOLEAN Wait
    )
{
    PUDN_FCB Fcb = (PUDN_FCB)Context;

    FsRtlEnterFileSystem();

    if (!ExAcquireResourceSharedLite(&Fcb->Resource, Wait)) {
        FsRtlExitFileSystem();
        return FALSE;
    }

    if (IoGetTopLevelIrp() == NULL) {
        IoSetTopLevelIrp((PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);
    }

    return TRUE;
}

static VOID
NTAPI
UdnReleaseFromReadAhead (
    PVOID Context
    )
{
    PUDN_FCB Fcb = (PUDN_FCB)Context;

    if (IoGetTopLevelIrp() == (PIRP)FSRTL_CACHE_TOP_LEVEL_IRP) {
        IoSetTopLevelIrp(NULL);
    }

    UdnRelease(&Fcb->Resource);
    FsRtlExitFileSystem();
}

VOID
UdnInitializeCacheCallbacks (
    VOID
    )
{
    UdnData.CacheManagerCallbacks.AcquireForLazyWrite = UdnAcquireForLazyWrite;
    UdnData.CacheManagerCallbacks.ReleaseFromLazyWrite = UdnReleaseFromLazyWrite;
    UdnData.CacheManagerCallbacks.AcquireForReadAhead = UdnAcquireForReadAhead;
    UdnData.CacheManagerCallbacks.ReleaseFromReadAhead = UdnReleaseFromReadAhead;

}
