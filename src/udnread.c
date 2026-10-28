/*
 * UDFNT - IRP_MJ_READ
 */

#include "udn.h"

/* DASD reads of the whole volume */
static NTSTATUS
UdnReadVolume (
    PUDN_IRP_CONTEXT Ctx,
    PUDN_FCB Vfcb,
    PUDN_CCB Ccb,
    LONGLONG StartingByte,
    ULONG ByteCount
    )
{
    PUDN_VCB Vcb = Vfcb->Vcb;
    PIRP Irp = Ctx->Irp;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    LONGLONG Limit;
    NTSTATUS Status;

    if (Ccb == NULL || (Irp->Flags & IRP_PAGING_IO)) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (!(Ccb->Flags & CCB_FLAG_DISMOUNTED_VOLUME) && Vcb->LockFileObject != FileObject) {
        Status = UdnVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    if (((ULONG)StartingByte & (UDN_SECTOR_SIZE - 1)) != 0 ||
        (ByteCount & (UDN_SECTOR_SIZE - 1)) != 0) {
        return STATUS_INVALID_PARAMETER;
    }

    Limit = Vcb->PartitionBytes;
    if (StartingByte >= Limit) {
        return STATUS_END_OF_FILE;
    }
    if (StartingByte + ByteCount > Limit) {
        ByteCount = (ULONG)(Limit - StartingByte);
    }

    /* What the engine has changed must be on the disk before it is read raw
       (a journal commit writes it home) */
    (VOID)UdnCommitNow(Vcb);

    Status = UdnLockUserBuffer(Irp, IoWriteAccess, ByteCount);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Status = UdnNonCachedIo(Ctx, Vfcb, IRP_MJ_READ, StartingByte, ByteCount, Limit);

    if (NT_SUCCESS(Status)) {
        Irp->IoStatus.Information = ByteCount;
        if (FileObject->Flags & FO_SYNCHRONOUS_IO) {
            FileObject->CurrentByteOffset.QuadPart = StartingByte + ByteCount;
        }
    }

    return Status;
}

/*
 * A paging read the device cannot take as it stands. The Memory Manager reads
 * the sections of a program at their file alignment - 512 bytes for most -
 * and a CD's 2048-byte sectors (or a 4K disk's) do not divide that: sent on,
 * the read fails or overruns the pages it is for, and the program dies
 * (0xC0000005) or will not start. It goes through the engine instead, which
 * reads whole sectors and copies the bytes asked for; the rest of the buffer,
 * and what lies past valid data, is zeros.
 */
static NTSTATUS
UdnUnalignedPagingRead (
    PUDN_IRP_CONTEXT Ctx,
    PUDN_FCB Fcb,
    LONGLONG StartingVbo,
    ULONG ByteCount
    )
{
    PUDN_VCB Vcb = Fcb->Vcb;
    PIRP Irp = Ctx->Irp;
    LONGLONG ValidData = Fcb->Header.ValidDataLength.QuadPart;
    ULONG BufferBytes = Ctx->IrpSp->Parameters.Read.Length;
    ULONG Want = 0, Got = 0;
    PUCHAR Buffer;
    NTSTATUS Status;

    Buffer = (PUCHAR)UdnMdlAddress(Irp->MdlAddress);
    if (Buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (BufferBytes > MmGetMdlByteCount(Irp->MdlAddress)) {
        BufferBytes = MmGetMdlByteCount(Irp->MdlAddress);
    }
    if (ByteCount > BufferBytes) {
        ByteCount = BufferBytes;
    }
    RtlZeroMemory(Buffer, BufferBytes);

    if (StartingVbo < ValidData) {
        Want = (ValidData - StartingVbo < (LONGLONG)ByteCount) ? (ULONG)(ValidData - StartingVbo) : ByteCount;
    }
    if (Want == 0) {
        return STATUS_SUCCESS;
    }

    UdnEngineEnter(Vcb);
    Status = UfxReadFile(&Vcb->Vol, Fcb->Ino, &Fcb->Inode, (ULONGLONG)StartingVbo, Want, Buffer, &Got);
    return UdnEngineLeave(Vcb, Status);
}

/* The cached read, run through UdnTryCall (the Cache Manager raises) */
typedef struct _UDN_CC_READ {
    PFILE_OBJECT FileObject;
    PUDN_FCB   Fcb;
    PIRP        Irp;
    BOOLEAN     Mdl;
    LARGE_INTEGER Start;
    ULONG       Count;
    PVOID       Buffer;
    NTSTATUS    Status;
} UDN_CC_READ;

static VOID
UdnCcReadBody (
    PVOID Context
    )
{
    UDN_CC_READ *C = (UDN_CC_READ *)Context;

    if (C->FileObject->PrivateCacheMap == NULL) {
        CcInitializeCacheMap(C->FileObject, (PCC_FILE_SIZES)&C->Fcb->Header.AllocationSize, FALSE,
                             &UdnData.CacheManagerCallbacks, C->Fcb);
    }

    if (C->Mdl) {
        CcMdlRead(C->FileObject, &C->Start, C->Count, &C->Irp->MdlAddress, &C->Irp->IoStatus);
        C->Status = C->Irp->IoStatus.Status;
    } else if (!CcCopyRead(C->FileObject, &C->Start, C->Count, TRUE, C->Buffer, &C->Irp->IoStatus)) {
        C->Status = STATUS_UNSUCCESSFUL;
    } else {
        C->Status = C->Irp->IoStatus.Status;
    }
}

static NTSTATUS
UdnReadFile (
    PUDN_IRP_CONTEXT Ctx,
    PUDN_FCB Fcb,
    LARGE_INTEGER StartingByte,
    ULONG ByteCount
    )
{
    PUDN_VCB Vcb = Fcb->Vcb;
    PIRP Irp = Ctx->Irp;
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    BOOLEAN PagingIo = (BOOLEAN)((Irp->Flags & IRP_PAGING_IO) != 0);
    BOOLEAN NonCached = (BOOLEAN)((Irp->Flags & IRP_NOCACHE) != 0);
    BOOLEAN Acquired = FALSE;
    BOOLEAN PagingAcquired = FALSE;
    IO_STATUS_BLOCK Iosb;
    LONGLONG FileSize;
    ULONG Transfer;
    PVOID Buffer;
    UDN_CC_READ Cc;
    NTSTATUS Code;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!PagingIo) {

        Status = UdnVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }

        if (NonCached &&
            (((ULONG)StartingByte.QuadPart & (UDN_SECTOR_SIZE - 1)) != 0 ||
             (ByteCount & (UDN_SECTOR_SIZE - 1)) != 0)) {
            return STATUS_INVALID_PARAMETER;
        }
    }

    __try {

        if (PagingIo) {
            (VOID)ExAcquireResourceSharedLite(&Fcb->PagingIoResource, TRUE);
            PagingAcquired = TRUE;
        } else {
            (VOID)ExAcquireResourceSharedLite(&Fcb->Resource, TRUE);
            Acquired = TRUE;

            if (!FsRtlCheckLockForReadAccess(&Fcb->FileLock, Irp)) {
                Status = STATUS_FILE_LOCK_CONFLICT;
                __leave;
            }

            if (NonCached && Fcb->SectionObjectPointers.DataSectionObject != NULL) {
                CcFlushCache(&Fcb->SectionObjectPointers, &StartingByte, ByteCount, &Iosb);
                if (!NT_SUCCESS(Iosb.Status)) {
                    Status = Iosb.Status;
                    __leave;
                }
            }
        }

        FileSize = Fcb->Header.FileSize.QuadPart;

        if (StartingByte.QuadPart >= FileSize) {
            Irp->IoStatus.Information = 0;
            Status = STATUS_END_OF_FILE;
            __leave;
        }

        if (StartingByte.QuadPart + ByteCount > FileSize) {
            ByteCount = (ULONG)(FileSize - StartingByte.QuadPart);
        }

        if (!NonCached) {

            Buffer = NULL;
            if (!(IrpSp->MinorFunction & IRP_MN_MDL)) {
                Buffer = UdnMapUserBuffer(Irp);
                if (Buffer == NULL) {
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    __leave;
                }
            }

            Cc.FileObject = FileObject;
            Cc.Fcb = Fcb;
            Cc.Irp = Irp;
            Cc.Mdl = (BOOLEAN)((IrpSp->MinorFunction & IRP_MN_MDL) != 0);
            Cc.Start = StartingByte;
            Cc.Count = ByteCount;
            Cc.Buffer = Buffer;
            Code = UdnTryCall(UdnCcReadBody, &Cc, FALSE);
            Status = NT_SUCCESS(Code) ? Cc.Status : UdnExceptionStatus(Code);

            if (!NT_SUCCESS(Status)) {
                __leave;
            }

        } else {

            Transfer = (ULONG)UdnRoundUp(ByteCount, UDN_SECTOR_SIZE);

            if (PagingIo && !(Fcb->FcbState & FCB_STATE_PAGING_FILE) &&
                (((ULONG)StartingByte.QuadPart & (UDN_SECTOR_SIZE - 1)) != 0 ||
                 Transfer > IrpSp->Parameters.Read.Length)) {

                Status = UdnUnalignedPagingRead(Ctx, Fcb, StartingByte.QuadPart, ByteCount);
                if (NT_SUCCESS(Status)) {
                    Irp->IoStatus.Information = ByteCount;
                }
                __leave;
            }

            if (!PagingIo) {
                Status = UdnLockUserBuffer(Irp, IoWriteAccess, Transfer);
                if (!NT_SUCCESS(Status)) {
                    __leave;
                }
            }

            Status = UdnNonCachedIo(Ctx, Fcb, IRP_MJ_READ, StartingByte.QuadPart, Transfer,
                                     Fcb->Header.ValidDataLength.QuadPart);
        }

        if (NT_SUCCESS(Status)) {
            Irp->IoStatus.Information = ByteCount;
            if (!PagingIo && (FileObject->Flags & FO_SYNCHRONOUS_IO)) {
                FileObject->CurrentByteOffset.QuadPart = StartingByte.QuadPart + ByteCount;
            }
        }

    } __finally {

        if (Acquired) {
            UdnRelease(&Fcb->Resource);
        }
        if (PagingAcquired) {
            UdnRelease(&Fcb->PagingIoResource);
        }
    }

    return Status;
}

NTSTATUS
UdnCommonRead (
    PUDN_IRP_CONTEXT Ctx
    )
{
    PIRP Irp = Ctx->Irp;
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PUDN_FCB Fcb = (PUDN_FCB)FileObject->FsContext;
    PUDN_CCB Ccb = (PUDN_CCB)FileObject->FsContext2;
    ULONG ByteCount = IrpSp->Parameters.Read.Length;

    if (Fcb == NULL) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (IrpSp->MinorFunction & IRP_MN_COMPLETE) {
        CcMdlReadComplete(FileObject, Irp->MdlAddress);
        Irp->MdlAddress = NULL;
        return STATUS_SUCCESS;
    }

    Irp->IoStatus.Information = 0;

    if (ByteCount == 0) {
        return STATUS_SUCCESS;
    }

    if (UdnIsVfcb(Fcb)) {
        return UdnReadVolume(Ctx, Fcb, Ccb, IrpSp->Parameters.Read.ByteOffset.QuadPart, ByteCount);
    }

    if (!UdnIsFcb(Fcb)) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    return UdnReadFile(Ctx, Fcb, IrpSp->Parameters.Read.ByteOffset, ByteCount);
}
