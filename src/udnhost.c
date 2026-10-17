/*
 * UDFNT - the engine's host in the driver: device reads and writes for
 * the engine (staged through nonpaged memory, any alignment), memory, the
 * clock, the engine lock and the commits.
 *
 * The engine keeps a write-back cache of metadata blocks and file entries; a
 * commit writes them, flushes the disk and closes the integrity descriptor
 * (the first change after it opens it again). Commits happen every
 * UDN_COMMIT_SECONDS, when more than UDN_COMMIT_DIRTY bytes are dirty, on a
 * flush request, and when the volume goes away.
 *
 * Everything here runs with Vcb->EngineResource held exclusive, except the
 * timer DPC (which only queues the commit work item).
 */

#include "udn.h"

#define VCB_OF(Vol)         ((PUDN_VCB)(Vol)->HostContext)
#define ENGINE_UP(Vcb)      ((Vcb)->Vol.Udf.NMaps != 0)
#define UDN_COMMIT_DIRTY    (8u << 20)
#define UDN_STAGE           (256u * 1024)

/* ------------------------------------------------------------------ */
/* Device I/O for the engine                                           */
/* ------------------------------------------------------------------ */

/*
 * The staging buffer: one per volume, allocated at the first I/O (the mount)
 * and kept, so that a commit under memory pressure does not fail for want
 * of nonpaged pool; smaller temporary ones if even that could not be had.
 */
static PUCHAR
UdnBounceGet (
    PUDN_VCB Vcb,
    PULONG Stage
    )
{
    ULONG Size;
    PUCHAR P;
    if (Vcb->Bounce == NULL) {
        Vcb->Bounce = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, UDN_STAGE + UDN_SECTOR_SIZE, UDN_TAG_BUFFER);
    }
    if (Vcb->Bounce != NULL) {
        *Stage = UDN_STAGE;
        return Vcb->Bounce;
    }
    for (Size = 64 * 1024; Size >= 4096; Size /= 4) {
        P = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Size + UDN_SECTOR_SIZE, UDN_TAG_BUFFER);
        if (P != NULL) {
            *Stage = Size;
            return P;
        }
    }
    UDN_DBG((UDN_PFX "no staging buffer\n"));
    return NULL;
}

static VOID
UdnBounceDone (
    PUDN_VCB Vcb,
    PUCHAR P
    )
{
    if (P != Vcb->Bounce) {
        ExFreePool(P);
    }
}

VOID
UdnBounceFree (
    PUDN_VCB Vcb
    )
{
    if (Vcb->Bounce != NULL) {
        ExFreePool(Vcb->Bounce);
        Vcb->Bounce = NULL;
    }
}

/* Length bytes at byte Offset of the volume, any alignment */
UDF_STATUS
UdfHostRead (
    PUDF_VOL Vol,
    ULONGLONG Offset,
    ULONG Length,
    PVOID Buffer
    )
{
    PUDN_VCB Vcb = VCB_OF(Vol);
    PUCHAR Out = (PUCHAR)Buffer;
    PUCHAR Bounce;
    ULONG Stage;
    NTSTATUS Status = STATUS_SUCCESS;

    Bounce = UdnBounceGet(Vcb, &Stage);
    if (Bounce == NULL) {
        return UDF_NO_MEMORY;
    }
    while (Length != 0) {
        ULONGLONG First = Offset & ~(ULONGLONG)(UDN_SECTOR_SIZE - 1);
        ULONG Lead = (ULONG)(Offset - First);
        ULONG Take = Length;
        ULONG Span;
        if (Take > Stage - Lead) {
            Take = Stage - Lead;
        }
        Span = (Lead + Take + UDN_SECTOR_SIZE - 1) & ~(UDN_SECTOR_SIZE - 1);
        Status = UdnReadDisk(Vcb, First >> UDN_SECTOR_SHIFT, Span >> UDN_SECTOR_SHIFT, Bounce);
        if (!NT_SUCCESS(Status)) {
            break;
        }
        RtlCopyMemory(Out, Bounce + Lead, Take);
        Out += Take;
        Offset += Take;
        Length -= Take;
    }
    UdnBounceDone(Vcb, Bounce);
    return Status;
}

UDF_STATUS
UdfHostWrite (
    PUDF_VOL Vol,
    ULONGLONG Offset,
    ULONG Length,
    const VOID *Buffer
    )
{
    PUDN_VCB Vcb = VCB_OF(Vol);
    const UCHAR *In = (const UCHAR *)Buffer;
    PUCHAR Bounce;
    ULONG Stage;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Vcb->VcbState & VCB_STATE_READ_ONLY) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }
    Bounce = UdnBounceGet(Vcb, &Stage);
    if (Bounce == NULL) {
        return UDF_NO_MEMORY;
    }
    while (Length != 0) {
        ULONGLONG First = Offset & ~(ULONGLONG)(UDN_SECTOR_SIZE - 1);
        ULONG Lead = (ULONG)(Offset - First);
        ULONG Take = Length;
        ULONG Span;
        if (Take > Stage - Lead) {
            Take = Stage - Lead;
        }
        Span = (Lead + Take + UDN_SECTOR_SIZE - 1) & ~(UDN_SECTOR_SIZE - 1);
        if (Lead != 0 || Span != Take) {
            /* partial sectors at the edges keep their other bytes */
            Status = UdnReadDisk(Vcb, First >> UDN_SECTOR_SHIFT, Span >> UDN_SECTOR_SHIFT, Bounce);
            if (!NT_SUCCESS(Status)) {
                break;
            }
        }
        RtlCopyMemory(Bounce + Lead, In, Take);
        Status = UdnWriteDisk(Vcb, First >> UDN_SECTOR_SHIFT, Span >> UDN_SECTOR_SHIFT, Bounce);
        if (!NT_SUCCESS(Status)) {
            break;
        }
        In += Take;
        Offset += Take;
        Length -= Take;
    }
    UdnBounceDone(Vcb, Bounce);
    return Status;
}

UDF_STATUS
UdfHostFlush (
    PUDF_VOL Vol
    )
{
    return UdnFlushDevice(VCB_OF(Vol));
}

/*
 * The engine's memory: paged (paging I/O of a paging file never enters the
 * engine). When the system is short of commit, paged pool can fail for a
 * while: then nonpaged pool, and failing that a few retries a moment apart,
 * since a commit that cannot get its buffers leaves the volume read-only.
 */
PVOID
UdfHostAlloc (
    ULONG Bytes
    )
{
    PVOID P;
    ULONG Try;
    if (Bytes == 0) {
        Bytes = 1;
    }
    for (Try = 0; ; Try++) {
        P = ExAllocatePoolWithTag(PagedPool, Bytes, UDN_TAG_ENGINE);
        if (P == NULL) {
            P = ExAllocatePoolWithTag(NonPagedPool, Bytes, UDN_TAG_ENGINE);
        }
        if (P != NULL || Try == 40) {       /* (always below DISPATCH_LEVEL: the engine runs under an ERESOURCE) */
            break;
        }
        {
            LARGE_INTEGER Wait;
            Wait.QuadPart = -250000;        /* 25 ms */
            (VOID)KeDelayExecutionThread(KernelMode, FALSE, &Wait);
        }
    }
    if (P == NULL) {
        UDN_DBG((UDN_PFX "engine allocation of %lu bytes failed\n", Bytes));
    } else if (Try != 0) {
        UDN_DBG((UDN_PFX "engine allocation of %lu bytes: %lu retries\n", Bytes, Try));
    }
    return P;
}

VOID
UdfHostFree (
    PVOID P
    )
{
    if (P != NULL) {
        ExFreePool(P);
    }
}

PVOID UfxHostAlloc(ULONG Bytes) { return UdfHostAlloc(Bytes); }
VOID  UfxHostFree(PVOID P) { UdfHostFree(P); }

ULONGLONG UdfHostNowNt(VOID) { LARGE_INTEGER Now; KeQuerySystemTime(&Now); return (ULONGLONG)Now.QuadPart; }

ULONGLONG
UfxHostNowNt (
    VOID
    )
{
    LARGE_INTEGER Now;
    KeQuerySystemTime(&Now);
    return (ULONGLONG)Now.QuadPart;
}

ULONG
UfxHostNow (
    VOID
    )
{
    LARGE_INTEGER Now;
    ULONG Seconds;

    KeQuerySystemTime(&Now);
    if (!RtlTimeToSecondsSince1970(&Now, &Seconds)) {
        Seconds = 0;
    }
    return Seconds;
}

/* ------------------------------------------------------------------ */
/* Engine calls and commits                                            */
/* ------------------------------------------------------------------ */

VOID UdnMetaInitialize(PUDN_VCB Vcb) { UNREFERENCED_PARAMETER(Vcb); }
VOID UdnMetaPurge(PUDN_VCB Vcb) { UNREFERENCED_PARAMETER(Vcb); }

VOID
UdnEngineEnter (
    PUDN_VCB Vcb
    )
{
    (VOID)ExAcquireResourceExclusiveLite(&Vcb->EngineResource, TRUE);
}

/* Writes the running transaction (engine held) */
NTSTATUS
UdnCommit (
    PUDN_VCB Vcb
    )
{
    NTSTATUS Status;

    if (!ENGINE_UP(Vcb) || !UfxTxnPending(&Vcb->Vol)) {
        return STATUS_SUCCESS;
    }
    Status = UfxTxnCommit(&Vcb->Vol);
    if (NT_SUCCESS(Status)) {
        Vcb->Commits++;
    } else {
        /* the transaction cannot be written: nothing more goes to the disk,
           which keeps its last consistent generation */
        UDN_DBG((UDN_PFX "commit failed %lX: the volume is read-only from now on\n", Status));
        Vcb->VcbState |= VCB_STATE_READ_ONLY;
        Vcb->Vol.ReadOnly = TRUE;
    }
    return Status;
}

NTSTATUS
UdnCommitNow (
    PUDN_VCB Vcb
    )
{
    UdnEngineEnter(Vcb);
    return UdnEngineLeave(Vcb, UdnCommit(Vcb));
}

/*
 * Ends an engine call: commits when the transaction has grown large (or the
 * system is going down). Returns Status, or the commit's error if the call
 * itself succeeded.
 */
NTSTATUS
UdnEngineLeave (
    PUDN_VCB Vcb,
    NTSTATUS Status
    )
{
    NTSTATUS Flush = STATUS_SUCCESS;

    if (ENGINE_UP(Vcb) && UfxTxnPending(&Vcb->Vol) &&
        (UfxDirtyBytes(&Vcb->Vol) > UDN_COMMIT_DIRTY || (Vcb->VcbState & VCB_STATE_SHUTDOWN))) {
        Flush = UdnCommit(Vcb);
    }

    UdnRelease(&Vcb->EngineResource);

    if (NT_SUCCESS(Status) && !NT_SUCCESS(Flush)) {
        Status = Flush;
    }
    return UdnStatus(Status);
}

/* ------------------------------------------------------------------ */
/* The periodic commit                                                 */
/* ------------------------------------------------------------------ */

static VOID
NTAPI
UdnCommitWorker (
    PVOID Context
    )
{
    PUDN_VCB Vcb = (PUDN_VCB)Context;

    if (!(Vcb->VcbState & (VCB_STATE_DISMOUNTED | VCB_STATE_IN_DISMOUNT)) && Vcb->Writing) {
        UdnEngineEnter(Vcb);
        (VOID)UdnCommit(Vcb);
        UdnRelease(&Vcb->EngineResource);
    }
    InterlockedExchange(&Vcb->CommitQueued, 0);
    if (Vcb->TimerSet) {
        LARGE_INTEGER Due;
        Due.QuadPart = -(LONGLONG)UDN_COMMIT_SECONDS * 10000000;
        KeSetTimer(&Vcb->CommitTimer, Due, &Vcb->CommitDpc);
    }
    KeSetEvent(&Vcb->CommitIdle, IO_NO_INCREMENT, FALSE);
}

static VOID
NTAPI
UdnCommitDpc (
    PKDPC Dpc,
    PVOID Context,
    PVOID Arg1,
    PVOID Arg2
    )
{
    PUDN_VCB Vcb = (PUDN_VCB)Context;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);

    if (!Vcb->TimerSet) {
        return;
    }
    if (InterlockedExchange(&Vcb->CommitQueued, 1) == 0) {
        (VOID)KeResetEvent(&Vcb->CommitIdle);
        ExQueueWorkItem(&Vcb->CommitItem, DelayedWorkQueue);
    }
}

VOID
UdnCommitTimerStart (
    PUDN_VCB Vcb
    )
{
    LARGE_INTEGER Due;

    if (Vcb->TimerSet || Vcb->Vol.ReadOnly) {
        return;
    }
    KeInitializeEvent(&Vcb->CommitIdle, NotificationEvent, TRUE);
    ExInitializeWorkItem(&Vcb->CommitItem, UdnCommitWorker, Vcb);
    KeInitializeDpc(&Vcb->CommitDpc, UdnCommitDpc, Vcb);
    KeInitializeTimer(&Vcb->CommitTimer);
    Due.QuadPart = -(LONGLONG)UDN_COMMIT_SECONDS * 10000000;
    Vcb->TimerSet = TRUE;
    KeSetTimer(&Vcb->CommitTimer, Due, &Vcb->CommitDpc);
}

VOID
UdnCommitTimerStop (
    PUDN_VCB Vcb
    )
{
    if (!Vcb->TimerSet) {
        return;
    }
    Vcb->TimerSet = FALSE;
    KeCancelTimer(&Vcb->CommitTimer);
    KeWaitForSingleObject(&Vcb->CommitIdle, Executive, KernelMode, FALSE, NULL);
    KeCancelTimer(&Vcb->CommitTimer);
}

/* ------------------------------------------------------------------ */
/* Status mapping                                                      */
/* ------------------------------------------------------------------ */

NTSTATUS
UdnStatus (
    UFX_STATUS S
    )
{
    switch (S) {
    case UFX_NO_SPACE:          return STATUS_EA_TOO_LARGE;
    case UFX_TOO_LARGE:         return STATUS_DISK_FULL;
    case UFX_NOT_A_DIRECTORY:   return STATUS_NOT_A_DIRECTORY;
    case UFX_NO_SUCH_EA:        return STATUS_NO_EAS_ON_FILE;
    case UFX_EA_CORRUPT:        return STATUS_EA_CORRUPT_ERROR;
    case UFX_READ_ONLY:         return STATUS_MEDIA_WRITE_PROTECTED;
    default:                    return (NTSTATUS)S;
    }
}
