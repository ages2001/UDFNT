/*
 * UDFNT - disk I/O
 *
 * Synchronous requests to the target device, sector I/O for the metadata
 * cache and the journal, caller buffers, and the non-cached transfer of
 * file data: one request per contiguous run of the file's blocks; holes
 * read as zeros and get blocks when written.
 */

#include "udn.h"

/* ------------------------------------------------------------------ */
/* Synchronous requests to the target device                           */
/* ------------------------------------------------------------------ */

/*
 * Completion of our own requests: the waiter is woken and frees the IRP
 * itself. No APC to the thread is involved, so this works where the
 * caller runs with kernel APCs blocked (a paging read from a page fault on
 * NT 3.1 waits for metadata this way).
 */
static NTSTATUS
NTAPI
UdnIoDone (
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp,
    PVOID Context
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* Reads or writes Length bytes of a nonpaged Buffer at byte Offset of Device */
NTSTATUS
UdnSyncIo (
    PDEVICE_OBJECT Device,
    UCHAR MajorFunction,
    LONGLONG Offset,
    ULONG Length,
    PVOID Buffer,
    BOOLEAN OverrideVerify
    )
{
    KEVENT Event;
    IO_STATUS_BLOCK Iosb;
    LARGE_INTEGER ByteOffset;
    PIRP Irp;
    PIO_STACK_LOCATION IrpSp;
    PMDL Mdl = NULL;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    ByteOffset.QuadPart = Offset;

    Irp = IoAllocateIrp(Device->StackSize, FALSE);
    if (Irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (Length != 0) {
        Mdl = IoAllocateMdl(Buffer, Length, FALSE, FALSE, NULL);
        if (Mdl == NULL) {
            IoFreeIrp(Irp);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        MmBuildMdlForNonPagedPool(Mdl);
    }

    Irp->MdlAddress = Mdl;
    Irp->Flags = IRP_NOCACHE;
    Irp->RequestorMode = KernelMode;
    Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    Irp->UserIosb = &Iosb;
    Irp->UserBuffer = Buffer;

    IrpSp = IoGetNextIrpStackLocation(Irp);
    IrpSp->MajorFunction = MajorFunction;
    IrpSp->Parameters.Read.Length = Length;
    IrpSp->Parameters.Read.ByteOffset = ByteOffset;
    if (OverrideVerify) {
        IrpSp->Flags |= SL_OVERRIDE_VERIFY_VOLUME;
    }

    IoSetCompletionRoutine(Irp, UdnIoDone, &Event, TRUE, TRUE, TRUE);

    Status = IoCallDriver(Device, Irp);
    if (Status == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    }
    Status = Irp->IoStatus.Status;
    if (NT_SUCCESS(Status) && Length != 0 && Irp->IoStatus.Information != Length) {
        Status = STATUS_UNEXPECTED_IO_ERROR;
    }

    if (Mdl != NULL) {
        IoFreeMdl(Mdl);
    }
    Irp->MdlAddress = NULL;
    IoFreeIrp(Irp);
    return Status;
}

NTSTATUS
UdnDeviceIoctl (
    PDEVICE_OBJECT Device,
    ULONG IoControlCode,
    PVOID InputBuffer,
    ULONG InputLength,
    PVOID OutputBuffer,
    ULONG OutputLength,
    BOOLEAN OverrideVerify,
    PULONG Information
    )
{
    KEVENT Event;
    IO_STATUS_BLOCK Iosb;
    PIRP Irp;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    Irp = IoBuildDeviceIoControlRequest(IoControlCode, Device, InputBuffer, InputLength,
                                        OutputBuffer, OutputLength, FALSE, &Event, &Iosb);
    if (Irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (OverrideVerify) {
        IoGetNextIrpStackLocation(Irp)->Flags |= SL_OVERRIDE_VERIFY_VOLUME;
    }

    Status = IoCallDriver(Device, Irp);

    if (Status == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Iosb.Status;
    }
    if (Information != NULL) {
        *Information = (ULONG)Iosb.Information;
    }
    return Status;
}

NTSTATUS
UdnFlushDevice (
    PUDN_VCB Vcb
    )
{
    NTSTATUS Status = UdnSyncIo(Vcb->TargetDeviceObject, IRP_MJ_FLUSH_BUFFERS, 0, 0, NULL, TRUE);

    /* NT 3.x disk drivers may not know the request: nothing to wait for then */
    if (Status == STATUS_INVALID_DEVICE_REQUEST || Status == STATUS_NOT_SUPPORTED) {
        Status = STATUS_SUCCESS;
    }
    return Status;
}

/* ------------------------------------------------------------------ */
/* Sector I/O                                                          */
/* ------------------------------------------------------------------ */

#define UDN_MAX_SYNC_IO     0x10000     /* bytes per request */

static NTSTATUS
UdnSectorIo (
    PUDN_VCB Vcb,
    UCHAR Major,
    ULONGLONG Sector,
    ULONG Count,
    PUCHAR Buffer
    )
{
    NTSTATUS Status;
    BOOLEAN Override = (BOOLEAN)(Vcb->VerifyThread == KeGetCurrentThread());

#ifdef UDN_TRACEIO
    DBGP("[sio "); DBGX(Major); DBGP(" "); DBGX((ULONG)Sector); DBGP(" "); DBGX(Count); DBGP("]\n");
#endif
    while (Count != 0) {
        ULONG Run = Count > UDN_MAX_SYNC_IO / UDN_SECTOR_SIZE ? UDN_MAX_SYNC_IO / UDN_SECTOR_SIZE : Count;
        Status = UdnSyncIo(Vcb->TargetDeviceObject, Major, (LONGLONG)(Sector << UDN_SECTOR_SHIFT),
                           Run << UDN_SECTOR_SHIFT, Buffer, Override);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
        Sector += Run;
        Count -= Run;
        Buffer += Run << UDN_SECTOR_SHIFT;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
UdnReadDisk (
    PUDN_VCB Vcb,
    ULONGLONG Sector,
    ULONG Count,
    PVOID Buffer
    )
{
    return UdnSectorIo(Vcb, IRP_MJ_READ, Sector, Count, (PUCHAR)Buffer);
}

NTSTATUS
UdnWriteDisk (
    PUDN_VCB Vcb,
    ULONGLONG Sector,
    ULONG Count,
    const VOID *Buffer
    )
{
    if (Vcb->VcbState & VCB_STATE_READ_ONLY) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }
    return UdnSectorIo(Vcb, IRP_MJ_WRITE, Sector, Count, (PUCHAR)Buffer);
}

/* File data at a device sector: written to every copy the chunk keeps (DUP data) */
NTSTATUS
UdnWriteData (
    PUDN_VCB Vcb,
    ULONGLONG Sector,
    ULONG Count,
    const VOID *Buffer
    )
{
    NTSTATUS Status = UdnWriteDisk(Vcb, Sector, Count, Buffer);
    if (NT_SUCCESS(Status) && Vcb->Vol.DataCopies) {
        ULONGLONG Other[3];
        ULONG Shift = Vcb->Vol.BlockShift - UDN_SECTOR_SHIFT;
        ULONG Within = (ULONG)(Sector & ((1UL << Shift) - 1));
        ULONG n = UfxDataMirrors(&Vcb->Vol, Sector >> Shift, Other, 3), i;
        for (i = 0; i < n && NT_SUCCESS(Status); i++) {
            Status = UdnWriteDisk(Vcb, (Other[i] << Shift) + Within, Count, Buffer);
        }
    }
    return Status;
}

/* ------------------------------------------------------------------ */
/* Caller buffers                                                      */
/* ------------------------------------------------------------------ */

typedef struct _UDN_PROBE {
    PMDL        Mdl;
    KPROCESSOR_MODE Mode;
    LOCK_OPERATION Operation;
} UDN_PROBE;

static VOID
UdnProbeBody (
    PVOID Context
    )
{
    UDN_PROBE *P = (UDN_PROBE *)Context;
    MmProbeAndLockPages(P->Mdl, P->Mode, P->Operation);
}

NTSTATUS
UdnLockUserBuffer (
    PIRP Irp,
    LOCK_OPERATION Operation,
    ULONG Length
    )
{
    PMDL Mdl;
    UDN_PROBE Probe;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Irp->MdlAddress != NULL) {
        return STATUS_SUCCESS;
    }

    Mdl = IoAllocateMdl(Irp->UserBuffer, Length, FALSE, FALSE, Irp);
    if (Mdl == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Probe.Mdl = Mdl;
    Probe.Mode = Irp->RequestorMode;
    Probe.Operation = Operation;
    if (!NT_SUCCESS(UdnTryCall(UdnProbeBody, &Probe, FALSE))) {
        Status = STATUS_INVALID_USER_BUFFER;
    }

    if (!NT_SUCCESS(Status)) {
        IoFreeMdl(Mdl);
        Irp->MdlAddress = NULL;
    }
    return Status;
}

PVOID
UdnMapUserBuffer (
    PIRP Irp
    )
{
    if (Irp->MdlAddress == NULL) {
        return Irp->UserBuffer;
    }
    return UdnMdlAddress(Irp->MdlAddress);
}

/* ------------------------------------------------------------------ */
/* File offsets to disk offsets                                        */
/* ------------------------------------------------------------------ */

#define UDN_MAX_CONTIG      0x40000000

/* A paging file: binary search of its nonpaged run list */
static NTSTATUS
UdnRunsLookup (
    PUDN_VCB Vcb,
    PUDN_FCB Fcb,
    ULONGLONG Blk,
    PULONGLONG Disk,
    PULONGLONG Run
    )
{
    ULONG Lo = 0, Hi = Fcb->RunCount, Mid;
    PUDN_RUN R;

    UNREFERENCED_PARAMETER(Vcb);
    if (Hi == 0) {
        return STATUS_FILE_CORRUPT_ERROR;
    }
    while (Lo + 1 < Hi) {
        Mid = (Lo + Hi) / 2;
        if (Fcb->Runs[Mid].FileBlock <= Blk) Lo = Mid; else Hi = Mid;
    }
    R = &Fcb->Runs[Lo];
    if (Blk < R->FileBlock || Blk - R->FileBlock >= R->Length) {
        return STATUS_FILE_CORRUPT_ERROR;
    }
    *Disk = R->DiskBlock + (Blk - R->FileBlock);
    *Run = R->Length - (Blk - R->FileBlock);
    return STATUS_SUCCESS;
}

/*
 * Maps byte Vbo of a file to its volume byte offset and the bytes that
 * follow in the same state. A hole gives *Lbo = -1 (reads zeros), unless
 * Allocate, which gives it a block. The caller holds the file's paging
 * resource (or more).
 */
NTSTATUS
UdnVboToLbo (
    PUDN_VCB Vcb,
    PUDN_FCB Fcb,
    LONGLONG Vbo,
    BOOLEAN Allocate,
    PLONGLONG Lbo,
    PULONG Contiguous
    )
{
    ULONG Shift = Vcb->Vol.BlockShift;
    ULONGLONG Blk, Disk = 0, Run = 0;
    ULONG Within;
    LONGLONG Left;
    NTSTATUS Status;

    if (Vbo < 0) {
        return STATUS_INVALID_PARAMETER;
    }

    if (UdnIsVfcb(Fcb)) {
        Left = Fcb->Header.AllocationSize.QuadPart - Vbo;
        if (Left <= 0) {
            return STATUS_END_OF_FILE;
        }
        *Lbo = Vbo;
        *Contiguous = (Left > UDN_MAX_CONTIG) ? UDN_MAX_CONTIG : (ULONG)Left;
        return STATUS_SUCCESS;
    }

    Blk = (ULONGLONG)Vbo >> Shift;
    Within = (ULONG)(Vbo & (Vcb->Vol.BlockSize - 1));
#ifdef UDN_TRACEIO
    DBGP("[v2l "); DBGX(Fcb->Ino); DBGP(" "); DBGX((ULONG)Blk); DBGP("]\n");
#endif

    if (Fcb->Runs != NULL) {
        Status = UdnRunsLookup(Vcb, Fcb, Blk, &Disk, &Run);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    } else {
        /* the one-entry map cache lives under the engine lock, like the tree */
        UdnEngineEnter(Vcb);
        if (Fcb->MapRun != 0 && Blk >= Fcb->MapFileBlock && Blk - Fcb->MapFileBlock < Fcb->MapRun &&
            !(Allocate && Fcb->MapHole)) {
            Run = Fcb->MapRun - (Blk - Fcb->MapFileBlock);
            Disk = Fcb->MapHole ? 0 : Fcb->MapDiskBlock + (Blk - Fcb->MapFileBlock);
            Status = STATUS_SUCCESS;
        } else {
            ULONG R32 = 0;
            Status = UfxMapFileBlock(&Vcb->Vol, Fcb->Ino, &Fcb->Inode, Blk, &Disk, &R32);
            if (NT_SUCCESS(Status) && Disk == 0 && Allocate) {
                BOOLEAN Fresh;
                Status = UfxGetFileBlock(&Vcb->Vol, Fcb->Ino, &Fcb->Inode, Blk, &Disk, &Fresh);
                if (NT_SUCCESS(Status)) {
                    Status = UfxWriteInode(&Vcb->Vol, Fcb->Ino, &Fcb->Inode);
                }
                R32 = 1;
            }
            if (R32 == 0) R32 = 1;
            Run = R32;
            if (NT_SUCCESS(Status)) {
                Fcb->MapFileBlock = Blk;
                Fcb->MapDiskBlock = Disk;
                Fcb->MapRun = R32;
                Fcb->MapHole = (BOOLEAN)(Disk == 0);
            } else {
                Fcb->MapRun = 0;
            }
        }
        Status = UdnEngineLeave(Vcb, Status);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    Left = (LONGLONG)(Run << Shift) - Within;
    *Contiguous = (Left > UDN_MAX_CONTIG) ? UDN_MAX_CONTIG : (ULONG)Left;
    *Lbo = (Disk == 0) ? -1 : (LONGLONG)((Disk << Shift) + Within);
    return STATUS_SUCCESS;
}

/*
 * A file about to be written in place: compressed or inline data is
 * rewritten into plain extents, checksummed or shared extents become the
 * file's own (nodatacow, nodatasum). Engine held.
 */
static NTSTATUS
UdnPrepareLocked (
    PUDN_VCB Vcb,
    PUDN_FCB Fcb
    )
{
    NTSTATUS Status = STATUS_SUCCESS;
    if (!UdnIsVfcb(Fcb) && (Fcb->Inode.Flags & (UFX_IF_INLINE_DATA | UFX_IF_NEEDS_PREP)) &&
        !(Fcb->FcbState & FCB_STATE_SYMLINK)) {
        Status = UfxUninline(&Vcb->Vol, Fcb->Ino, &Fcb->Inode);
        UdnForgetMapping(Fcb);
    }
    return Status;
}

NTSTATUS
UdnPrepareForWrite (
    PUDN_VCB Vcb,
    PUDN_FCB Fcb
    )
{
    NTSTATUS Status;
    if (UdnIsVfcb(Fcb) || !(Fcb->Inode.Flags & (UFX_IF_INLINE_DATA | UFX_IF_NEEDS_PREP))) {
        return STATUS_SUCCESS;
    }
    UdnEngineEnter(Vcb);
    Status = UdnPrepareLocked(Vcb, Fcb);
    return UdnEngineLeave(Vcb, Status);
}

/*
 * Gives every hole in [Start, End) of a file its blocks (user writes do
 * this before the data reaches the cache, so "disk full" goes back to the
 * writer instead of the lazy writer). Preallocated ranges become regular,
 * zeroed on the disk; new blocks bigger than a page are zeroed too, since
 * the cache writes them only page by page.
 */
NTSTATUS
UdnAllocateRange (
    PUDN_VCB Vcb,
    PUDN_FCB Fcb,
    LONGLONG Start,
    LONGLONG End
    )
{
    ULONG Shift = Vcb->Vol.BlockShift;
    ULONGLONG Blk, Last;
    NTSTATUS Status;

    if (Fcb->Runs != NULL || End <= Start || UdnIsVfcb(Fcb)) {
        return STATUS_SUCCESS;
    }
    Blk = (ULONGLONG)Start >> Shift;
    Last = ((ULONGLONG)End + Vcb->Vol.BlockSize - 1) >> Shift;

    UdnEngineEnter(Vcb);
    Status = UdnPrepareLocked(Vcb, Fcb);
    if (NT_SUCCESS(Status)) {
        Status = UfxAllocateRange(&Vcb->Vol, Fcb->Ino, &Fcb->Inode, Blk, Last - Blk);
    }
    UdnForgetMapping(Fcb);              /* under the engine lock, like its readers */
    return UdnEngineLeave(Vcb, Status);
}

/* ------------------------------------------------------------------ */
/* Non-cached transfers                                                */
/* ------------------------------------------------------------------ */

static NTSTATUS
NTAPI
UdnSyncCompletion (
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp,
    PVOID Context
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* Moves Length bytes between the disk at Lbo and the master MDL from BufferOffset */
static NTSTATUS
UdnTransferRun (
    PUDN_VCB Vcb,
    UCHAR MajorFunction,
    PIRP MasterIrp,
    ULONG BufferOffset,
    LONGLONG Lbo,
    ULONG Length
    )
{
    PMDL MasterMdl = MasterIrp->MdlAddress;
    PMDL Mdl = MasterMdl;
    PUCHAR Va;
    PIRP Irp;
    PIO_STACK_LOCATION IrpSp;
    KEVENT Event;
    NTSTATUS Status;

    Irp = IoAllocateIrp(Vcb->TargetDeviceObject->StackSize, FALSE);
    if (Irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (BufferOffset != 0 || Length != MmGetMdlByteCount(MasterMdl)) {

        Va = (PUCHAR)MmGetMdlVirtualAddress(MasterMdl) + BufferOffset;

        Mdl = IoAllocateMdl(Va, Length, FALSE, FALSE, NULL);
        if (Mdl == NULL) {
            IoFreeIrp(Irp);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        IoBuildPartialMdl(MasterMdl, Mdl, Va, Length);
    }

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    Irp->MdlAddress = Mdl;
    Irp->Flags = IRP_NOCACHE | (MasterIrp->Flags & IRP_PAGING_IO);
    Irp->RequestorMode = KernelMode;
    Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    Irp->UserIosb = NULL;

    IrpSp = IoGetNextIrpStackLocation(Irp);
    IrpSp->MajorFunction = MajorFunction;
    IrpSp->Parameters.Read.Length = Length;
    IrpSp->Parameters.Read.ByteOffset.QuadPart = Lbo;

    if (Vcb->VerifyThread == KeGetCurrentThread()) {
        IrpSp->Flags |= SL_OVERRIDE_VERIFY_VOLUME;
    }

    IoSetCompletionRoutine(Irp, UdnSyncCompletion, &Event, TRUE, TRUE, TRUE);

#ifdef UDN_TRACEIO
    DBGP("[run "); DBGX((ULONG)(Lbo >> 9)); DBGP(" "); DBGX(Length); DBGP("]\n");
#endif
    Status = IoCallDriver(Vcb->TargetDeviceObject, Irp);

    if (Status == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    }

    Status = Irp->IoStatus.Status;

    if (NT_SUCCESS(Status) && Irp->IoStatus.Information != Length) {
        Status = STATUS_UNEXPECTED_IO_ERROR;
    }

    if (Mdl != MasterMdl) {
        IoFreeMdl(Mdl);
    }

    Irp->MdlAddress = NULL;
    IoFreeIrp(Irp);

    return Status;
}

/*
 * A non-cached write that covers only part of a block that is still a hole:
 * the block is allocated and zeroed first, so its other bytes read as zero.
 */
static NTSTATUS
UdnPrepareEdge (
    PUDN_VCB Vcb,
    PUDN_FCB Fcb,
    LONGLONG Vbo
    )
{
    LONGLONG Lbo;
    ULONG Contiguous;
    PVOID Zero;
    NTSTATUS Status;

    Status = UdnVboToLbo(Vcb, Fcb, Vbo, FALSE, &Lbo, &Contiguous);
    if (!NT_SUCCESS(Status) || Lbo != -1) {
        return Status;
    }
    Status = UdnVboToLbo(Vcb, Fcb, Vbo, TRUE, &Lbo, &Contiguous);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Zero = ExAllocatePoolWithTag(NonPagedPool, Vcb->Vol.BlockSize, UDN_TAG_BUFFER);
    if (Zero == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(Zero, Vcb->Vol.BlockSize);
    Lbo &= ~(LONGLONG)(Vcb->Vol.BlockSize - 1);
    Status = UdnWriteData(Vcb, (ULONGLONG)Lbo >> UDN_SECTOR_SHIFT, Vcb->Vol.SectorsPerBlock, Zero);
    ExFreePool(Zero);
    return Status;
}

/*
 * Moves ByteCount bytes (whole sectors) between the disk and Irp->MdlAddress,
 * from StartingVbo of Fcb. Reads return zeros from ValidData on and in
 * holes; writes give holes their blocks.
 */
NTSTATUS
UdnNonCachedIo (
    PUDN_IRP_CONTEXT Ctx,
    PUDN_FCB Fcb,
    UCHAR MajorFunction,
    LONGLONG StartingVbo,
    ULONG ByteCount,
    LONGLONG ValidData
    )
{
    PUDN_VCB Vcb = Fcb->Vcb;
    PIRP Irp = Ctx->Irp;
    BOOLEAN Write = (BOOLEAN)(MajorFunction == IRP_MJ_WRITE);
    LONGLONG TransferEnd = StartingVbo + ByteCount;
    LONGLONG Vbo, Lbo;
    ULONG Contiguous, Offset, Length, ZeroOffset;
    ULONG BlockMask = Vcb->Vol.BlockSize - 1;
    PUCHAR Buffer = NULL;
    NTSTATUS Status = STATUS_SUCCESS;

    /* data the map cannot serve (compressed, inline, a symbolic link) reads through the engine */
    if (!UdnIsVfcb(Fcb) && ((Fcb->Inode.Flags & UFX_IF_INLINE_DATA) || (Fcb->FcbState & FCB_STATE_SYMLINK))) {
        if (!Write) {
            ULONG Got = 0;
            Buffer = (PUCHAR)UdnMdlAddress(Irp->MdlAddress);
            if (Buffer == NULL) {
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            RtlZeroMemory(Buffer, ByteCount);
            UdnEngineEnter(Vcb);
            Status = UfxReadFile(&Vcb->Vol, Fcb->Ino, &Fcb->Inode, (ULONGLONG)StartingVbo,
                                 ByteCount, Buffer, &Got);
            return UdnEngineLeave(Vcb, Status);
        }
        if (Fcb->FcbState & FCB_STATE_SYMLINK) {
            return STATUS_ACCESS_DENIED;
        }
    }
    if (Write) {
        Status = UdnPrepareForWrite(Vcb, Fcb);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    if (!Write && TransferEnd > ValidData) {
        TransferEnd = UdnRoundUp(ValidData, UDN_SECTOR_SIZE);
        if (TransferEnd < StartingVbo) {
            TransferEnd = StartingVbo;
        }
        if (TransferEnd > StartingVbo + ByteCount) {
            TransferEnd = StartingVbo + ByteCount;
        }
    }

    if (Write && !UdnIsVfcb(Fcb) && Fcb->Runs == NULL) {
        if (StartingVbo & BlockMask) {
            Status = UdnPrepareEdge(Vcb, Fcb, StartingVbo);
        }
        if (NT_SUCCESS(Status) && (TransferEnd & BlockMask) &&
            ((TransferEnd - 1) & ~(LONGLONG)BlockMask) != (StartingVbo & ~(LONGLONG)BlockMask)) {
            Status = UdnPrepareEdge(Vcb, Fcb, TransferEnd - 1);
        } else if (NT_SUCCESS(Status) && (TransferEnd & BlockMask) && !(StartingVbo & BlockMask)) {
            Status = UdnPrepareEdge(Vcb, Fcb, StartingVbo);
        }
        /*
         * The rest of the range in one go (the edges have their blocks now,
         * zeroed where the write leaves part of them): one block at a time
         * below it is an engine call and a block-sized transfer per block,
         * and a large write into new space crawls.
         */
        if (NT_SUCCESS(Status)) {
            Status = UdnAllocateRange(Vcb, Fcb, StartingVbo, TransferEnd);
        }
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    Vbo = StartingVbo;
    Offset = 0;

    while (Vbo < TransferEnd) {

        Status = UdnVboToLbo(Vcb, Fcb, Vbo, Write, &Lbo, &Contiguous);
        if (!NT_SUCCESS(Status)) {
            UDN_DBG((UDN_PFX "No block for offset %I64X of inode %lu: %lX\n", Vbo, Fcb->Ino, Status));
            return Status;
        }

        Length = (ULONG)(TransferEnd - Vbo);
        if (Length > Contiguous) {
            Length = Contiguous;
        }
        /*
         * Contiguous can reach 1 GB, and a disk driver need not split what it
         * is sent (not every one does, nor every port driver below it):
         * each request to the device stays within UDN_MAX_SYNC_IO, as the
         * engine's own sector I/O does.
         */
        if (Lbo != -1 && Length > UDN_MAX_SYNC_IO) {
            Length = UDN_MAX_SYNC_IO;
        }

        if (Lbo == -1) {
            /* a hole reads as zeros */
            if (Buffer == NULL) {
                Buffer = (PUCHAR)UdnMdlAddress(Irp->MdlAddress);
                if (Buffer == NULL) {
                    return STATUS_INSUFFICIENT_RESOURCES;
                }
            }
            RtlZeroMemory(Buffer + Offset, Length);
        } else {
            Status = UdnTransferRun(Vcb, MajorFunction, Irp, Offset, Lbo, Length);
            if (NT_SUCCESS(Status) && Write && Vcb->Vol.DataCopies && !UdnIsVfcb(Fcb)) {
                /* DUP data: the other copy too */
                ULONGLONG Other[3];
                ULONG Within = (ULONG)(Lbo & (Vcb->Vol.BlockSize - 1));
                ULONG n = UfxDataMirrors(&Vcb->Vol, (ULONGLONG)Lbo >> Vcb->Vol.BlockShift, Other, 3), i;
                for (i = 0; i < n && NT_SUCCESS(Status); i++) {
                    Status = UdnTransferRun(Vcb, MajorFunction, Irp, Offset,
                                            (LONGLONG)((Other[i] << Vcb->Vol.BlockShift) + Within), Length);
                }
            }
            if (!NT_SUCCESS(Status)) {
                UDN_DBG((UDN_PFX "Disk transfer at %I64X failed %lX\n", Lbo, Status));
                return Status;
            }
        }

        Vbo += Length;
        Offset += Length;
    }

    if (!Write && StartingVbo + ByteCount > ValidData) {

        ZeroOffset = (ValidData > StartingVbo) ? (ULONG)(ValidData - StartingVbo) : 0;
        if (Buffer == NULL) {
            Buffer = (PUCHAR)UdnMdlAddress(Irp->MdlAddress);
            if (Buffer == NULL) {
                return STATUS_INSUFFICIENT_RESOURCES;
            }
        }
        RtlZeroMemory(Buffer + ZeroOffset, ByteCount - ZeroOffset);
    }

    return Status;
}

/*
 * Writes zeros over [Start, End) of Fcb on the disk where it has blocks
 * (holes are zero already); a partial first sector keeps its bytes before
 * Start. End is rounded up to a sector.
 */
NTSTATUS
UdnZeroDisk (
    PUDN_VCB Vcb,
    PUDN_FCB Fcb,
    LONGLONG Start,
    LONGLONG End
    )
{
    PUCHAR Zero;
    LONGLONG Vbo, Lbo;
    ULONG Contiguous, Length, Within;
    NTSTATUS Status = STATUS_SUCCESS;

    End = UdnRoundUp(End, UDN_SECTOR_SIZE);
    if (Start >= End) {
        return STATUS_SUCCESS;
    }
    Status = UdnPrepareForWrite(Vcb, Fcb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Zero = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, UDN_ZERO_CHUNK, UDN_TAG_BUFFER);
    if (Zero == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Vbo = Start & ~(LONGLONG)(UDN_SECTOR_SIZE - 1);
    Within = (ULONG)(Start - Vbo);

    if (Within != 0) {
        Status = UdnVboToLbo(Vcb, Fcb, Vbo, FALSE, &Lbo, &Contiguous);
        if (!NT_SUCCESS(Status)) goto Done;
        if (Lbo != -1) {
            Status = UdnReadDisk(Vcb, (ULONGLONG)Lbo >> UDN_SECTOR_SHIFT, 1, Zero);
            if (!NT_SUCCESS(Status)) goto Done;
            RtlZeroMemory(Zero + Within, UDN_SECTOR_SIZE - Within);
            Status = UdnWriteData(Vcb, (ULONGLONG)Lbo >> UDN_SECTOR_SHIFT, 1, Zero);
            if (!NT_SUCCESS(Status)) goto Done;
        }
        Vbo += UDN_SECTOR_SIZE;
    }

    RtlZeroMemory(Zero, UDN_ZERO_CHUNK);

    while (Vbo < End) {
        Status = UdnVboToLbo(Vcb, Fcb, Vbo, FALSE, &Lbo, &Contiguous);
        if (!NT_SUCCESS(Status)) goto Done;
        Length = UDN_ZERO_CHUNK;
        if (Length > Contiguous) {
            Length = Contiguous;
        }
        if ((LONGLONG)Length > End - Vbo) {
            Length = (ULONG)(End - Vbo);
        }
        if (Lbo != -1) {
            Status = UdnWriteData(Vcb, (ULONGLONG)Lbo >> UDN_SECTOR_SHIFT,
                                  (Length + UDN_SECTOR_SIZE - 1) >> UDN_SECTOR_SHIFT, Zero);
            if (!NT_SUCCESS(Status)) goto Done;
        }
        Vbo += Length;
    }

Done:
    ExFreePool(Zero);
    return Status;
}
