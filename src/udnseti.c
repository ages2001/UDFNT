/*
 * UDFNT - IRP_MJ_SET_INFORMATION
 *
 * Every class but FilePositionInformation takes the volume exclusive and
 * the file exclusive; the inode is written before returning.
 */

#include "udn.h"

/* Cuts FileSize and VDL and drops the cached data past them */
static VOID
UdnShrinkFileSize (
    PUDN_FCB Fcb,
    PFILE_OBJECT FileObject,
    LONGLONG NewSize
    )
{
    LARGE_INTEGER Page;
    IO_STATUS_BLOCK Iosb;

    /* The cached page holding the new end keeps the old bytes after it.
       Should the file grow again they would show through, so that page
       goes to the disk and out of the cache, to come back zeroed past VDL. */
    if ((NewSize & (UDN_PAGE_SIZE - 1)) != 0 &&
        NewSize < Fcb->Header.ValidDataLength.QuadPart &&
        Fcb->SectionObjectPointers.DataSectionObject != NULL) {

        Page.QuadPart = NewSize & ~(LONGLONG)(UDN_PAGE_SIZE - 1);

        CcFlushCache(&Fcb->SectionObjectPointers, &Page, UDN_PAGE_SIZE, &Iosb);
        (VOID)CcPurgeCacheSection(&Fcb->SectionObjectPointers, &Page, UDN_PAGE_SIZE, FALSE);
    }

    (VOID)ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);

    Fcb->Header.FileSize.QuadPart = NewSize;

    if (Fcb->Header.ValidDataLength.QuadPart > NewSize) {
        Fcb->Header.ValidDataLength.QuadPart = NewSize;
    }

    __try {

        if (Fcb->SectionObjectPointers.SharedCacheMap != NULL) {
            CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);
        } else if (Fcb->SectionObjectPointers.DataSectionObject != NULL) {
            /* mapped but not cached: drop the pages past the new end */
            LARGE_INTEGER From;
            From.QuadPart = (NewSize + UDN_PAGE_SIZE - 1) & ~(LONGLONG)(UDN_PAGE_SIZE - 1);
            (VOID)CcPurgeCacheSection(&Fcb->SectionObjectPointers, &From, 0, FALSE);
        }

    } __finally {

        UdnRelease(&Fcb->PagingIoResource);
    }
}

/*
 * Changes FileSize, and with it VDL and the allocation when they no
 * longer fit. Called with the volume and the file exclusive; the caller
 * writes the inode.
 *
 * Growing allocates nothing: the new part is a hole until written. When
 * the file shrinks, the rest of its new last block is zeroed on the disk,
 * as Linux does, so that a later growth shows zeros there.
 */
NTSTATUS
UdnSetFileSize (
    PUDN_IRP_CONTEXT Ctx,
    PUDN_FCB Fcb,
    PFILE_OBJECT FileObject,
    LONGLONG NewSize
    )
{
    PUDN_VCB Vcb = Fcb->Vcb;
    LARGE_INTEGER Size;
    LONGLONG BlockEnd;
    NTSTATUS Status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(Ctx);

    if (NewSize < 0 || (ULONGLONG)NewSize > UdnMaxFileSize(Vcb)) {
        return STATUS_INVALID_PARAMETER;
    }

    if (NewSize == Fcb->Header.FileSize.QuadPart) {
        return STATUS_SUCCESS;
    }

    Size.QuadPart = NewSize;

    if (NewSize < Fcb->Header.FileSize.QuadPart &&
        !MmCanFileBeTruncated(&Fcb->SectionObjectPointers, &Size)) {

        return STATUS_USER_MAPPED_FILE;
    }

    /* Data kept inside the inode leaves it first: its tail is not ours to cut */
    if (Fcb->Inode.Flags & UFX_IF_INLINE_DATA) {
        (VOID)ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
        UdnEngineEnter(Vcb);
        Status = UfxUninline(&Vcb->Vol, Fcb->Ino, &Fcb->Inode);
        UdnForgetMapping(Fcb);
        Status = UdnEngineLeave(Vcb, Status);
        UdnRelease(&Fcb->PagingIoResource);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    if (NewSize > Fcb->Header.FileSize.QuadPart) {

        if (NewSize > Fcb->Header.AllocationSize.QuadPart) {
            Status = UdnSetAllocation(Vcb, Fcb, (ULONGLONG)NewSize);
            if (!NT_SUCCESS(Status)) {
                return Status;
            }
        }

        Fcb->Header.FileSize.QuadPart = NewSize;

        /* The Memory Manager never relies on the file system to zero a
         * paging file past valid data: the whole file counts as valid, and
         * all of it has blocks, mapped in nonpaged pool. */
        if (Fcb->FcbState & FCB_STATE_PAGING_FILE) {
            Fcb->Header.ValidDataLength.QuadPart = NewSize;
            Status = UdnLoadRuns(Vcb, Fcb);
            if (!NT_SUCCESS(Status)) {
                return Status;
            }
        }

        if (Fcb->SectionObjectPointers.SharedCacheMap != NULL) {
            CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);
        }

    } else {

        UdnShrinkFileSize(Fcb, FileObject, NewSize);

        Status = UdnSetAllocation(Vcb, Fcb, (ULONGLONG)NewSize);

        if (NT_SUCCESS(Status)) {
            BlockEnd = UdnRoundUp(NewSize, (LONGLONG)Vcb->Vol.BlockSize);
            if (BlockEnd > NewSize) {
                (VOID)UdnZeroDisk(Vcb, Fcb, NewSize, BlockEnd);
            }
            if (Fcb->FcbState & FCB_STATE_PAGING_FILE) {
                Status = UdnLoadRuns(Vcb, Fcb);
            }
        }
    }

    Fcb->FcbState |= FCB_STATE_INODE_DIRTY;

    return Status;
}

/* Times and attributes; FILE_ATTRIBUTE_READONLY is the owner write bit */
static NTSTATUS
UdnSetBasicInfo (
    PUDN_VCB Vcb,
    PUDN_FCB Fcb,
    PUDN_CCB Ccb,
    PFILE_BASIC_INFORMATION Info
    )
{
    ULONG Filter = 0;
    ULONG Attributes;
    ULONG Dos;
    USHORT Mode;
    BOOLEAN UserChange = FALSE;
    LARGE_INTEGER Now;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Info->FileAttributes != 0) {

        Attributes = Info->FileAttributes;

        if (UdnIsDcb(Fcb)) {
            if (Attributes & FILE_ATTRIBUTE_TEMPORARY) {
                return STATUS_INVALID_PARAMETER;
            }
        } else if (Attributes & FILE_ATTRIBUTE_DIRECTORY) {
            return STATUS_INVALID_PARAMETER;
        }

        Mode = Fcb->Inode.Mode;
        if (UdnIsFcb(Fcb) && !(Fcb->FcbState & FCB_STATE_SYMLINK)) {
            if (Attributes & FILE_ATTRIBUTE_READONLY) {
                Mode &= (USHORT)~0222;
            } else if (!(Mode & 0200)) {
                if (Fcb->Inode.Flags & UFX_IF_IMMUTABLE) {
                    return STATUS_ACCESS_DENIED;    /* chattr +i: only Linux takes it away */
                }
                Mode |= 0200;
            }
        }

        Dos = Attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE);

        if (Mode != Fcb->Inode.Mode || Dos != Fcb->DosAttributes) {

            UdnEngineEnter(Vcb);
            if (Mode != Fcb->Inode.Mode) {
                Fcb->Inode.Mode = Mode;
                Status = UfxWriteInode(&Vcb->Vol, Fcb->Ino, &Fcb->Inode);
            }
            if (NT_SUCCESS(Status) && Dos != Fcb->DosAttributes) {
                Status = UdnStoreDosAttributes(Vcb, Fcb, Dos);
            }
            Status = UdnEngineLeave(Vcb, Status);
            if (!NT_SUCCESS(Status)) {
                return Status;
            }

            Fcb->Attributes = UdnComputeAttributes(Vcb, &Fcb->Inode, Fcb->UfxName, Fcb->UfxNameLength,
                                                   Fcb->DosAttributes);
            Filter |= FILE_NOTIFY_CHANGE_ATTRIBUTES;
            UserChange = TRUE;
        }
    }

    /* Zero leaves a time alone; so does -1, as later Windows versions use it */
    if (Info->CreationTime.QuadPart != 0 && Info->CreationTime.QuadPart != -1) {
        Fcb->CreationTime = Info->CreationTime;
        Filter |= FILE_NOTIFY_CHANGE_CREATION;
    }

    if (Info->LastAccessTime.QuadPart != 0 && Info->LastAccessTime.QuadPart != -1) {
        Fcb->LastAccessTime = Info->LastAccessTime;
        Ccb->Flags |= CCB_FLAG_USER_SET_ACCESS;
        Filter |= FILE_NOTIFY_CHANGE_LAST_ACCESS;
    }

    if (Info->LastWriteTime.QuadPart != 0 && Info->LastWriteTime.QuadPart != -1) {
        Fcb->LastWriteTime = Info->LastWriteTime;
        Ccb->Flags |= CCB_FLAG_USER_SET_WRITE;
        Filter |= FILE_NOTIFY_CHANGE_LAST_WRITE;
    }

    if (Info->ChangeTime.QuadPart != 0 && Info->ChangeTime.QuadPart != -1) {
        Fcb->ChangeTime = Info->ChangeTime;
    } else if (Filter != 0) {
        /* an attribute or time change is an inode change (ctime), as Linux */
        KeQuerySystemTime(&Now);
        Fcb->ChangeTime = Now;
    }

    if (Filter == 0) {
        return STATUS_SUCCESS;
    }

    UNREFERENCED_PARAMETER(UserChange);
    Fcb->FcbState |= FCB_STATE_INODE_DIRTY;
    UdnNotifyChange(Vcb, Fcb, Filter, FILE_ACTION_MODIFIED);

    return UdnUpdateInode(Vcb, Fcb);
}

static NTSTATUS
UdnSetDispositionInfo (
    PUDN_VCB Vcb,
    PUDN_FCB Fcb,
    PUDN_CCB Ccb,
    PFILE_OBJECT FileObject,
    PFILE_DISPOSITION_INFORMATION Info
    )
{
    BOOLEAN Empty;
    NTSTATUS Status;

    if (!Info->DeleteFile) {
        Fcb->FcbState &= ~FCB_STATE_DELETE_PENDING;
        Fcb->DeleteNameLength = 0;
        FileObject->DeletePending = FALSE;
        return STATUS_SUCCESS;
    }

    if ((Fcb->Attributes & FILE_ATTRIBUTE_READONLY) && !(Fcb->FcbState & FCB_STATE_SYMLINK)) {
        return STATUS_CANNOT_DELETE;
    }

    if (Fcb->Inode.Flags & (UFX_IF_IMMUTABLE | UFX_IF_APPEND)) {
        return STATUS_CANNOT_DELETE;
    }

    if (UdnIsDcb(Fcb)) {

        UdnEngineEnter(Vcb);
        Status = UdnEngineLeave(Vcb, UfxDirIsEmpty(&Vcb->Vol, Fcb->Ino, &Fcb->Inode, &Empty));
        if (!NT_SUCCESS(Status)) {
            return Status;
        }

        if (!Empty) {
            return STATUS_DIRECTORY_NOT_EMPTY;
        }

    } else if (!MmFlushImageSection(&Fcb->SectionObjectPointers, MmFlushForDelete)) {

        /* A running program */
        return STATUS_CANNOT_DELETE;
    }

    Fcb->FcbState |= FCB_STATE_DELETE_PENDING;
    UdnSetDeleteName(Fcb, Ccb);
    FileObject->DeletePending = TRUE;

    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Rename and hard links                                               */
/* ------------------------------------------------------------------ */

static VOID
UdnReportName (
    PUDN_VCB Vcb,
    PUNICODE_STRING FullName,
    USHORT NameOffset,
    ULONG Filter,
    ULONG Action
    )
{
    FsRtlNotifyFullReportChange(Vcb->NotifySync,
                                &Vcb->DirNotifyList,
                                (PSTRING)FullName,
                                NameOffset,
                                NULL,
                                NULL,
                                Filter,
                                Action,
                                NULL);
}

/* Reports Name (ext bytes) in directory Dcb */
static VOID
UdnReportChild (
    PUDN_VCB Vcb,
    PUDN_FCB Dcb,
    const UCHAR *Name,
    ULONG NameLength,
    ULONG Filter,
    ULONG Action
    )
{
    UNICODE_STRING U, Full;
    USHORT Prefix;

    if (Dcb == NULL || !NT_SUCCESS(UdnBuildFullName(Dcb))) {
        return;
    }
    if (!NT_SUCCESS(UdnExtToUnicode(Name, NameLength, &U))) {
        return;
    }
    Prefix = Dcb->FullName.Length;
    if (Dcb->ParentDcb != NULL) {
        Prefix += sizeof(WCHAR);            /* the root's name is the separator already */
    }
    Full.MaximumLength = (USHORT)(Prefix + U.Length);
    Full.Buffer = (PWSTR)ExAllocatePoolWithTag(PagedPool, Full.MaximumLength, UDN_TAG_NAME);
    if (Full.Buffer != NULL) {
        RtlCopyMemory(Full.Buffer, Dcb->FullName.Buffer, Dcb->FullName.Length);
        if (Dcb->ParentDcb != NULL) {
            Full.Buffer[Dcb->FullName.Length / sizeof(WCHAR)] = L'\\';
        }
        RtlCopyMemory((PUCHAR)Full.Buffer + Prefix, U.Buffer, U.Length);
        Full.Length = Full.MaximumLength;
        UdnReportName(Vcb, &Full, Prefix, Filter, Action);
        ExFreePool(Full.Buffer);
    }
    ExFreePool(U.Buffer);
}

/*
 * Where a rename or link goes: the directory (its inode number and, when
 * open, its DCB) and the new name as ext bytes.
 */
typedef struct _UDN_TARGET {
    PUDN_FCB    Dcb;                /* may be NULL (a hard link's other directory) */
    ULONG       DirIno;
    ULONG       Length;
    UCHAR       Name[UDN_MAX_NAME + 1];
} UDN_TARGET, *PUDN_TARGET;

static NTSTATUS
UdnGetTarget (
    PUDN_IRP_CONTEXT Ctx,
    PUDN_VCB Vcb,
    ULONG SourceDirIno,
    PWSTR FileName,
    ULONG FileNameLength,
    PUDN_TARGET T
    )
{
    PFILE_OBJECT TargetObject = Ctx->IrpSp->Parameters.SetFile.FileObject;
    UNICODE_STRING NewName;
    NTSTATUS Status;

    if (TargetObject != NULL) {

        /* The I/O manager opened the target directory for us */
        T->Dcb = (PUDN_FCB)TargetObject->FsContext;

        if (T->Dcb == NULL || !UdnIsDcb(T->Dcb)) {
            return STATUS_INVALID_PARAMETER;
        }
        if (T->Dcb->Vcb != Vcb) {
            return STATUS_NOT_SAME_DEVICE;
        }
        T->DirIno = T->Dcb->Ino;
        NewName = TargetObject->FileName;

    } else {

        T->DirIno = SourceDirIno;
        T->Dcb = UdnFindFcbByIno(Vcb, SourceDirIno, FALSE);
        if (T->Dcb != NULL && !UdnIsDcb(T->Dcb)) {
            T->Dcb = NULL;
        }

        if (FileNameLength > 0xFFFE) {
            return STATUS_OBJECT_NAME_INVALID;
        }
        NewName.Buffer = FileName;
        NewName.Length = (USHORT)FileNameLength;
        NewName.MaximumLength = NewName.Length;

        /* a simple rename names no directory */
        {
            ULONG i;
            for (i = 0; i < NewName.Length / sizeof(WCHAR); i++) {
                if (NewName.Buffer[i] == L'\\') {
                    return STATUS_INVALID_PARAMETER;
                }
            }
        }
    }

    if (T->Dcb != NULL && (T->Dcb->FcbState & (FCB_STATE_DELETE_PENDING | FCB_STATE_DELETED))) {
        return STATUS_DELETE_PENDING;
    }

    if (!UdnIsValidUnicodeName(&NewName)) {
        return STATUS_OBJECT_NAME_INVALID;
    }

    Status = UdnUnicodeToExt(&NewName, T->Name, &T->Length);
    if (!NT_SUCCESS(Status) || T->Length == 0) {
        return STATUS_OBJECT_NAME_INVALID;
    }
    T->Name[T->Length] = 0;

    if ((T->Length == 1 && T->Name[0] == '.') ||
        (T->Length == 2 && T->Name[0] == '.' && T->Name[1] == '.')) {
        return STATUS_OBJECT_NAME_INVALID;
    }

    return STATUS_SUCCESS;
}

/* Looks the target name up: exactly, then (unless case-sensitive) without regard to case */
static NTSTATUS
UdnFindTarget (
    PUDN_VCB Vcb,
    PUDN_TARGET T,
    PUFX_ENTRY Found
    )
{
    UFX_INODE Temp;
    PUFX_INODE Dir;
    UNICODE_STRING U;
    NTSTATUS Status;

    if (T->Dcb != NULL && !UdnData.CaseSensitive) {
        Status = UdnExtToUnicode(T->Name, T->Length, &U);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
        Status = UdnLookupName(Vcb, T->Dcb, &U, FALSE, Found);
        ExFreePool(U.Buffer);
        return Status;
    }

    UdnEngineEnter(Vcb);
    Dir = UdnDirInode(Vcb, T->DirIno, &Temp);
    Status = (Dir == NULL) ? STATUS_FILE_CORRUPT_ERROR :
             UfxLookup(&Vcb->Vol, T->DirIno, Dir, (const char *)T->Name, T->Length, Found);
    return UdnEngineLeave(Vcb, Status);
}

/*
 * The target exists and may be replaced: remove that name. It must be a
 * file (not a directory, not read-only) that nobody has open.
 */
static NTSTATUS
UdnReplaceTarget (
    PUDN_VCB Vcb,
    PUDN_TARGET T,
    PUFX_ENTRY Found
    )
{
    PUDN_FCB Existing;
    UFX_INODE Inode;
    UFX_INODE Temp;
    PUFX_INODE Dir;
    ULONG Pass;
    NTSTATUS Status;

    UdnEngineEnter(Vcb);
    Status = UdnEngineLeave(Vcb, UfxReadInode(&Vcb->Vol, Found->Inode, &Inode));
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    if ((Inode.Mode & UFX_S_IFMT) == UFX_S_IFDIR) {
        return STATUS_ACCESS_DENIED;
    }
    if ((Inode.Mode & UFX_S_IFMT) == UFX_S_IFREG && !(Inode.Mode & 0200)) {
        return STATUS_ACCESS_DENIED;        /* read-only */
    }
    if (Inode.Flags & (UFX_IF_IMMUTABLE | UFX_IF_APPEND)) {
        return STATUS_ACCESS_DENIED;
    }

    /* Open as itself (a symbolic link) or followed: either FCB must be idle */
    for (Pass = 0; Pass < 2; Pass++) {
        Existing = UdnFindFcbByIno(Vcb, Found->Inode, (BOOLEAN)(Pass == 1));
        if (Existing != NULL &&
            (Existing->UncleanCount != 0 ||
             !MmFlushImageSection(&Existing->SectionObjectPointers, MmFlushForDelete))) {
            return STATUS_ACCESS_DENIED;
        }
    }

    Existing = UdnFindFcbByIno(Vcb, Found->Inode, FALSE);
    if (Existing == NULL) {
        Existing = UdnFindFcbByIno(Vcb, Found->Inode, TRUE);
    }

    if (Existing != NULL) {

        /* Only the cache holds it: drop that and remove the name through the FCB */
        Existing->RefCount++;

        (VOID)ExAcquireResourceExclusiveLite(&Existing->Resource, TRUE);

        if (Existing->FcbState & FCB_STATE_INODE_DIRTY) {
            (VOID)UdnUpdateInode(Vcb, Existing);
        }

        Existing->DeleteParentIno = T->DirIno;
        Existing->DeleteNameLength = Found->NameLength;
        RtlCopyMemory(Existing->DeleteName, Found->Name, Found->NameLength);
        Existing->DeleteName[Found->NameLength] = 0;

        Status = UdnUnlinkName(Vcb, Existing);

        if (NT_SUCCESS(Status) && (Existing->FcbState & FCB_STATE_DELETED)) {
            (VOID)CcPurgeCacheSection(&Existing->SectionObjectPointers, NULL, 0, FALSE);
        }

        UdnRelease(&Existing->Resource);

        if (NT_SUCCESS(Status)) {
            UdnReportChild(Vcb, T->Dcb, (const UCHAR *)Found->Name, Found->NameLength,
                           FILE_NOTIFY_CHANGE_FILE_NAME, FILE_ACTION_REMOVED);
            if (Existing->FcbState & FCB_STATE_DELETED) {
                (VOID)MmForceSectionClosed(&Existing->SectionObjectPointers, TRUE);
            }
        }

        UdnDereferenceFcb(Vcb, Existing);
        return Status;
    }

    UdnEngineEnter(Vcb);
    Dir = UdnDirInode(Vcb, T->DirIno, &Temp);
    Status = (Dir == NULL) ? STATUS_FILE_CORRUPT_ERROR :
             UfxUnlink(&Vcb->Vol, T->DirIno, Dir, Found->Name, Found->NameLength);
    Status = UdnEngineLeave(Vcb, Status);

    if (NT_SUCCESS(Status)) {
        if (T->Dcb != NULL) {
            UdnIndexRemove(T->Dcb, (const UCHAR *)Found->Name, Found->NameLength);
        }
        UdnReportChild(Vcb, T->Dcb, (const UCHAR *)Found->Name, Found->NameLength,
                       FILE_NOTIFY_CHANGE_FILE_NAME, FILE_ACTION_REMOVED);
    }
    return Status;
}

/* The name a rename or link through this handle starts from */
static VOID
UdnSourceName (
    PUDN_FCB Fcb,
    PUDN_CCB Ccb,
    PULONG DirIno,
    const UCHAR **Name,
    PULONG Length
    )
{
    if ((Ccb->Flags & CCB_FLAG_HAVE_NAME) && UdnIsFcb(Fcb) && Fcb->Inode.LinksCount > 1) {
        *DirIno = Ccb->ParentIno;
        *Name = Ccb->Name;
        *Length = Ccb->NameLength;
    } else {
        *DirIno = Fcb->ParentDcb->Ino;
        *Name = Fcb->UfxName;
        *Length = Fcb->UfxNameLength;
    }
}

static NTSTATUS
UdnSetRenameInfo (
    PUDN_IRP_CONTEXT Ctx,
    PUDN_VCB Vcb,
    PUDN_FCB Fcb,
    PUDN_CCB Ccb,
    PFILE_RENAME_INFORMATION Info
    )
{
    BOOLEAN ReplaceIfExists = Ctx->IrpSp->Parameters.SetFile.ReplaceIfExists;
    PUDN_FCB OldParent;
    PUDN_FCB Walk;
    PLIST_ENTRY Entry;
    PUDN_TARGET T = NULL;
    PUFX_ENTRY Found = NULL;
    UCHAR OldName[UDN_MAX_NAME + 1];
    ULONG OldLength, OldDirIno;
    const UCHAR *Source;
    BOOLEAN OwnName;
    UNICODE_STRING NewUnicode;
    UNICODE_STRING OldFull;
    USHORT OldNameOffset = 0;
    UFX_INODE OldTemp, NewTemp;
    PUFX_INODE OldDir, NewDir;
    ULONG Filter;
    NTSTATUS Status;

    RtlZeroMemory(&OldFull, sizeof(OldFull));
    RtlZeroMemory(&NewUnicode, sizeof(NewUnicode));

    if (Fcb->FcbState & FCB_STATE_DELETE_PENDING) {
        return STATUS_DELETE_PENDING;
    }
    if (Fcb->Inode.Flags & (UFX_IF_IMMUTABLE | UFX_IF_APPEND)) {
        return STATUS_ACCESS_DENIED;
    }

    UdnSourceName(Fcb, Ccb, &OldDirIno, &Source, &OldLength);
    RtlCopyMemory(OldName, Source, OldLength);
    OldName[OldLength] = 0;
    OwnName = (BOOLEAN)(Fcb->ParentDcb != NULL && OldDirIno == Fcb->ParentDcb->Ino &&
                        OldLength == Fcb->UfxNameLength &&
                        RtlCompareMemory(OldName, Fcb->UfxName, OldLength) == OldLength);
    OldParent = OwnName ? Fcb->ParentDcb : UdnFindFcbByIno(Vcb, OldDirIno, FALSE);
    if (OldParent != NULL && !UdnIsDcb(OldParent)) {
        OldParent = NULL;
    }

    T = (PUDN_TARGET)ExAllocatePoolWithTag(PagedPool, sizeof(UDN_TARGET), UDN_TAG_BUFFER);
    Found = (PUFX_ENTRY)ExAllocatePoolWithTag(PagedPool, sizeof(UFX_ENTRY), UDN_TAG_BUFFER);
    if (T == NULL || Found == NULL) {
        if (T != NULL) ExFreePool(T);
        if (Found != NULL) ExFreePool(Found);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    __try {

        Status = UdnGetTarget(Ctx, Vcb, OldDirIno, Info->FileName, Info->FileNameLength, T);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        if (UdnIsDcb(Fcb)) {

            /* Not into itself or below */
            if (T->Dcb == NULL || T->Dcb == Fcb || UdnIsAncestor(Fcb, T->Dcb)) {
                Status = (T->Dcb == NULL) ? STATUS_ACCESS_DENIED : STATUS_INVALID_PARAMETER;
                __leave;
            }

            /* Nothing below it may be open */
            for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {
                Walk = CONTAINING_RECORD(Entry, UDN_FCB, FcbLinks);
                if (Walk != Fcb && Walk->UncleanCount != 0 && UdnIsAncestor(Fcb, Walk)) {
                    Status = STATUS_ACCESS_DENIED;
                    __leave;
                }
            }
        }

        /* Exactly the same name: nothing to do */
        if (T->DirIno == OldDirIno && T->Length == OldLength &&
            RtlCompareMemory(T->Name, OldName, OldLength) == OldLength) {
            Status = STATUS_SUCCESS;
            __leave;
        }

        Status = UdnFindTarget(Vcb, T, Found);

        if (NT_SUCCESS(Status)) {

            BOOLEAN SameEntry = (BOOLEAN)(T->DirIno == OldDirIno && Found->NameLength == OldLength &&
                                          RtlCompareMemory(Found->Name, OldName, OldLength) == OldLength);

            /* Another spelling of its own name (a change of case) is fine */
            if (!SameEntry) {
                if (!ReplaceIfExists) {
                    Status = STATUS_OBJECT_NAME_COLLISION;
                    __leave;
                }
                if (Found->Inode == Fcb->Ino) {
                    /* another link to the same file: POSIX would do nothing,
                       NT expects the source name gone - remove it, the target stays */
                    UFX_INODE Temp;
                    PUFX_INODE Dir;
                    UdnEngineEnter(Vcb);
                    Dir = UdnDirInode(Vcb, OldDirIno, &Temp);
                    Status = (Dir == NULL) ? STATUS_FILE_CORRUPT_ERROR :
                             UfxRemoveEntry(&Vcb->Vol, OldDirIno, Dir, (const char *)OldName, OldLength);
                    if (NT_SUCCESS(Status)) {
                        Fcb->Inode.LinksCount--;
                        Fcb->Inode.Ctime = UfxHostNow();
                        Status = UfxWriteInode(&Vcb->Vol, Fcb->Ino, &Fcb->Inode);
                    }
                    Status = UdnEngineLeave(Vcb, Status);
                    if (NT_SUCCESS(Status) && OldParent != NULL) {
                        UdnIndexRemove(OldParent, OldName, OldLength);
                    }
                    if (NT_SUCCESS(Status)) {
                        RtlCopyMemory(OldName, Found->Name, Found->NameLength);
                        OldLength = Found->NameLength;
                        OldDirIno = T->DirIno;
                        OldParent = T->Dcb;
                        if (Ccb->Flags & CCB_FLAG_HAVE_NAME) {
                            Ccb->ParentIno = OldDirIno;
                            Ccb->NameLength = OldLength;
                            RtlCopyMemory(Ccb->Name, OldName, OldLength);
                            Ccb->Name[OldLength] = 0;
                        }
                        if (OwnName) {
                            /* the FCB keeps a name that still exists */
                            Fcb->UfxNameLength = OldLength;
                            RtlCopyMemory(Fcb->UfxName, OldName, OldLength);
                            Fcb->UfxName[OldLength] = 0;
                        }
                        if (T->Length == OldLength && RtlCompareMemory(T->Name, OldName, OldLength) == OldLength) {
                            __leave;
                        }
                        /* else: the target differs only in case, rename on */
                    } else {
                        __leave;
                    }
                } else {
                    Status = UdnReplaceTarget(Vcb, T, Found);
                    if (!NT_SUCCESS(Status)) {
                        __leave;
                    }
                }
            }

        } else if (Status != STATUS_OBJECT_NAME_NOT_FOUND) {
            __leave;
        }

        Status = UdnExtToUnicode(T->Name, T->Length, &NewUnicode);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        /* Keep the old name for the notification */
        if (OwnName && NT_SUCCESS(UdnBuildFullName(Fcb))) {
            OldFull.Buffer = (PWSTR)ExAllocatePoolWithTag(PagedPool, Fcb->FullName.Length, UDN_TAG_NAME);
            if (OldFull.Buffer != NULL) {
                RtlCopyMemory(OldFull.Buffer, Fcb->FullName.Buffer, Fcb->FullName.Length);
                OldFull.Length = Fcb->FullName.Length;
                OldFull.MaximumLength = OldFull.Length;
                OldNameOffset = (USHORT)(Fcb->FullName.Length - Fcb->Name.Length);
            }
        }

        /* The inode on the disk must carry the FCB's current size and times:
           the engine reads it and writes it back */
        if (Fcb->FcbState & FCB_STATE_INODE_DIRTY) {
            Status = UdnUpdateInode(Vcb, Fcb);
            if (!NT_SUCCESS(Status)) {
                __leave;
            }
        }

        UdnEngineEnter(Vcb);
        OldDir = UdnDirInode(Vcb, OldDirIno, &OldTemp);
        NewDir = (T->DirIno == OldDirIno) ? OldDir : UdnDirInode(Vcb, T->DirIno, &NewTemp);
        if (OldDir == NULL || NewDir == NULL) {
            Status = STATUS_FILE_CORRUPT_ERROR;
        } else {
            Status = UfxRename(&Vcb->Vol, OldDirIno, OldDir, (const char *)OldName, OldLength,
                               T->DirIno, NewDir, (const char *)T->Name, T->Length);
        }
        if (NT_SUCCESS(Status)) {
            /* ctime, and a directory's ".." */
            (VOID)UfxReadInode(&Vcb->Vol, Fcb->Ino, &Fcb->Inode);
        }
        Status = UdnEngineLeave(Vcb, Status);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        KeQuerySystemTime(&Fcb->ChangeTime);

        if (OldParent != NULL) {
            UdnIndexRemove(OldParent, OldName, OldLength);
        }
        if (T->Dcb != NULL) {
            UdnIndexAdd(T->Dcb, T->Name, T->Length, Fcb->Ino);
        }

        if (Ccb->Flags & CCB_FLAG_HAVE_NAME) {
            Ccb->ParentIno = T->DirIno;
            Ccb->NameLength = T->Length;
            RtlCopyMemory(Ccb->Name, T->Name, T->Length + 1);
        }

        Filter = UdnIsDcb(Fcb) ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME;

        if (OwnName) {

            /* The FCB follows */
            UdnForgetNames(Vcb, Fcb);

            if (Fcb->Name.Buffer != NULL) {
                ExFreePool(Fcb->Name.Buffer);
            }
            Fcb->Name = NewUnicode;
            RtlZeroMemory(&NewUnicode, sizeof(NewUnicode));

            RtlCopyMemory(Fcb->UfxName, T->Name, T->Length + 1);
            Fcb->UfxNameLength = T->Length;

            if (T->Dcb != NULL && T->Dcb != Fcb->ParentDcb) {
                PUDN_FCB Previous = Fcb->ParentDcb;
                T->Dcb->RefCount++;
                Fcb->ParentDcb = T->Dcb;
                UdnDereferenceFcb(Vcb, Previous);
            }

            /* a leading dot hides */
            Fcb->Attributes = UdnComputeAttributes(Vcb, &Fcb->Inode, Fcb->UfxName, Fcb->UfxNameLength,
                                                   Fcb->DosAttributes);

            if (OldFull.Buffer != NULL) {
                UdnReportName(Vcb, &OldFull, OldNameOffset, Filter,
                              (T->DirIno == OldDirIno) ? FILE_ACTION_RENAMED_OLD_NAME : FILE_ACTION_REMOVED);
            }
            UdnNotifyChange(Vcb, Fcb, Filter,
                            (T->DirIno == OldDirIno) ? FILE_ACTION_RENAMED_NEW_NAME : FILE_ACTION_ADDED);

        } else {

            UdnReportChild(Vcb, OldParent, OldName, OldLength, Filter, FILE_ACTION_REMOVED);
            UdnReportChild(Vcb, T->Dcb, T->Name, T->Length, Filter, FILE_ACTION_ADDED);
        }

    } __finally {

        ExFreePool(T);
        ExFreePool(Found);

        if (NewUnicode.Buffer != NULL) {
            ExFreePool(NewUnicode.Buffer);
        }
        if (OldFull.Buffer != NULL) {
            ExFreePool(OldFull.Buffer);
        }
    }

    return Status;
}

/* FileLinkInformation: another name for a file (ext hard links) */
static NTSTATUS
UdnSetLinkInfo (
    PUDN_IRP_CONTEXT Ctx,
    PUDN_VCB Vcb,
    PUDN_FCB Fcb,
    PUDN_CCB Ccb,
    PFILE_LINK_INFORMATION Info
    )
{
    BOOLEAN ReplaceIfExists = Info->ReplaceIfExists;
    PUDN_TARGET T = NULL;
    PUFX_ENTRY Found = NULL;
    ULONG SourceDirIno, SourceLength;
    const UCHAR *Source;
    UFX_INODE Temp;
    PUFX_INODE Dir;
    NTSTATUS Status;

    if (UdnIsDcb(Fcb)) {
        return STATUS_FILE_IS_A_DIRECTORY;
    }
    if (Fcb->FcbState & FCB_STATE_DELETE_PENDING) {
        return STATUS_DELETE_PENDING;
    }
    if (Fcb->Inode.Flags & (UFX_IF_IMMUTABLE | UFX_IF_APPEND)) {
        return STATUS_ACCESS_DENIED;
    }

    UdnSourceName(Fcb, Ccb, &SourceDirIno, &Source, &SourceLength);

    T = (PUDN_TARGET)ExAllocatePoolWithTag(PagedPool, sizeof(UDN_TARGET), UDN_TAG_BUFFER);
    Found = (PUFX_ENTRY)ExAllocatePoolWithTag(PagedPool, sizeof(UFX_ENTRY), UDN_TAG_BUFFER);
    if (T == NULL || Found == NULL) {
        if (T != NULL) ExFreePool(T);
        if (Found != NULL) ExFreePool(Found);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    __try {

        Status = UdnGetTarget(Ctx, Vcb, SourceDirIno, Info->FileName, Info->FileNameLength, T);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        Status = UdnFindTarget(Vcb, T, Found);
        if (NT_SUCCESS(Status)) {
            if (Found->Inode == Fcb->Ino) {
                Status = STATUS_SUCCESS;        /* already a name of this file */
                __leave;
            }
            if (!ReplaceIfExists) {
                Status = STATUS_OBJECT_NAME_COLLISION;
                __leave;
            }
            Status = UdnReplaceTarget(Vcb, T, Found);
            if (!NT_SUCCESS(Status)) {
                __leave;
            }
        } else if (Status != STATUS_OBJECT_NAME_NOT_FOUND) {
            __leave;
        }

        if (Fcb->FcbState & FCB_STATE_INODE_DIRTY) {
            Status = UdnUpdateInode(Vcb, Fcb);
            if (!NT_SUCCESS(Status)) {
                __leave;
            }
        }

        UdnEngineEnter(Vcb);
        Dir = UdnDirInode(Vcb, T->DirIno, &Temp);
        Status = (Dir == NULL) ? STATUS_FILE_CORRUPT_ERROR :
                 UfxLink(&Vcb->Vol, T->DirIno, Dir, (const char *)T->Name, T->Length, Fcb->Ino, &Fcb->Inode);
        Status = UdnEngineLeave(Vcb, Status);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        KeQuerySystemTime(&Fcb->ChangeTime);
        if (T->Dcb != NULL) {
            UdnIndexAdd(T->Dcb, T->Name, T->Length, Fcb->Ino);
        }
        UdnReportChild(Vcb, T->Dcb, T->Name, T->Length, FILE_NOTIFY_CHANGE_FILE_NAME, FILE_ACTION_ADDED);

    } __finally {

        ExFreePool(T);
        ExFreePool(Found);
    }

    return Status;
}

static NTSTATUS
UdnSetAllocationInfo (
    PUDN_IRP_CONTEXT Ctx,
    PUDN_VCB Vcb,
    PUDN_FCB Fcb,
    PFILE_OBJECT FileObject,
    PFILE_ALLOCATION_INFORMATION Info
    )
{
    LONGLONG NewAllocation = Info->AllocationSize.QuadPart;
    NTSTATUS Status;

    if (!UdnIsFcb(Fcb) || (Fcb->FcbState & FCB_STATE_SYMLINK)) {
        return STATUS_INVALID_PARAMETER;
    }

    if (NewAllocation < 0 || (ULONGLONG)NewAllocation > UdnMaxFileSize(Vcb)) {
        return STATUS_INVALID_PARAMETER;
    }

    if (NewAllocation < Fcb->Header.FileSize.QuadPart) {

        /* Shrinking below the data cuts the file */
        Status = UdnSetFileSize(Ctx, Fcb, FileObject, NewAllocation);

        if (NT_SUCCESS(Status)) {
            FileObject->Flags |= FO_FILE_MODIFIED;
            UdnNotifyChange(Vcb, Fcb, FILE_NOTIFY_CHANGE_SIZE, FILE_ACTION_MODIFIED);
        }

    } else {

        /* No blocks are reserved: ext files are sparse, and the space a
           write needs is found when the data is written */
        Status = UdnSetAllocation(Vcb, Fcb, (ULONGLONG)NewAllocation);

        if (NT_SUCCESS(Status) &&
            Fcb->Header.AllocationSize.QuadPart >
                UdnRoundUp(Fcb->Header.FileSize.QuadPart, (LONGLONG)Vcb->Vol.BlockSize)) {

            /* Allocation past the data is given back when the file closes */
            Fcb->FcbState |= FCB_STATE_TRUNCATE_ON_CLOSE;
        }

        if (NT_SUCCESS(Status) && (Fcb->FcbState & FCB_STATE_PAGING_FILE)) {
            Status = UdnLoadRuns(Vcb, Fcb);
        }

        if (NT_SUCCESS(Status) && Fcb->SectionObjectPointers.SharedCacheMap != NULL) {
            CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);
        }
    }

    if (NT_SUCCESS(Status) && (Fcb->FcbState & FCB_STATE_INODE_DIRTY)) {
        Status = UdnUpdateInode(Vcb, Fcb);
    }

    return Status;
}

static NTSTATUS
UdnSetEndOfFileInfo (
    PUDN_IRP_CONTEXT Ctx,
    PUDN_VCB Vcb,
    PUDN_FCB Fcb,
    PFILE_OBJECT FileObject,
    PFILE_END_OF_FILE_INFORMATION Info
    )
{
    NTSTATUS Status;

    if (!UdnIsFcb(Fcb) || (Fcb->FcbState & FCB_STATE_SYMLINK)) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (Info->EndOfFile.QuadPart == Fcb->Header.FileSize.QuadPart) {
        return STATUS_SUCCESS;
    }

    Status = UdnSetFileSize(Ctx, Fcb, FileObject, Info->EndOfFile.QuadPart);

    if (NT_SUCCESS(Status)) {

        FileObject->Flags |= FO_FILE_MODIFIED;
        UdnNotifyChange(Vcb, Fcb, FILE_NOTIFY_CHANGE_SIZE, FILE_ACTION_MODIFIED);

        Status = UdnUpdateInode(Vcb, Fcb);
    }

    return Status;
}

NTSTATUS
UdnCommonSetInformation (
    PUDN_IRP_CONTEXT Ctx
    )
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PUDN_FCB Fcb = (PUDN_FCB)FileObject->FsContext;
    PUDN_CCB Ccb = (PUDN_CCB)FileObject->FsContext2;
    PUDN_VCB Vcb = Ctx->Vcb;
    FILE_INFORMATION_CLASS Class = IrpSp->Parameters.SetFile.FileInformationClass;
    PVOID Buffer = Ctx->Irp->AssociatedIrp.SystemBuffer;
    PFILE_POSITION_INFORMATION Position;
    BOOLEAN FcbAcquired = FALSE;
    NTSTATUS Status;

    /* The lazy writer reporting what reached the disk (possibly through a file
     * object whose handle is already closed, so with no CCB): the inode is
     * written at cleanup and close anyway */
    if (Class == FileEndOfFileInformation && IrpSp->Parameters.SetFile.AdvanceOnly) {
        return STATUS_SUCCESS;
    }

    if (Fcb == NULL || Ccb == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Class == FilePositionInformation) {

        if (!UdnIsVfcb(Fcb)) {
            Status = UdnVerifyVcb(Ctx, Vcb);
            if (!NT_SUCCESS(Status)) {
                return Status;
            }
        }

        Position = (PFILE_POSITION_INFORMATION)Buffer;

        if ((FileObject->Flags & FO_NO_INTERMEDIATE_BUFFERING) &&
            ((ULONG)Position->CurrentByteOffset.QuadPart & (UDN_SECTOR_SIZE - 1)) != 0) {

            return STATUS_INVALID_PARAMETER;
        }

        FileObject->CurrentByteOffset = Position->CurrentByteOffset;
        return STATUS_SUCCESS;
    }

    if (UdnIsVfcb(Fcb)) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Fcb->FcbState & FCB_STATE_ROOT) {
        if (Class == FileBasicInformation) {
            /* the root's times and attributes can change; not its name */
        } else {
            return (Class == FileDispositionInformation) ? STATUS_CANNOT_DELETE : STATUS_INVALID_PARAMETER;
        }
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Status = UdnVerifyWritable(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        (VOID)ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);
        FcbAcquired = TRUE;

        if (Fcb->FcbState & FCB_STATE_DELETED) {
            Status = STATUS_FILE_DELETED;
            __leave;
        }

        /*
         * A paging file in use must not be deleted or renamed, but the Memory
         * Manager DOES size it (FileAllocationInformation / FileEndOfFileInformation)
         * when it sets the paging file up and when it grows it - those must be
         * allowed, or Mm reports "no properly sized paging file" and runs with
         * almost no virtual memory.
         */
        if ((Fcb->FcbState & FCB_STATE_PAGING_FILE) &&
            (Class == FileDispositionInformation || Class == FileRenameInformation ||
             Class == FileLinkInformation)) {

            Status = STATUS_ACCESS_DENIED;
            __leave;
        }

        switch ((ULONG)Class) {

        case FileBasicInformation:
            Status = UdnSetBasicInfo(Vcb, Fcb, Ccb, (PFILE_BASIC_INFORMATION)Buffer);
            break;

        case FileDispositionInformation:
            Status = UdnSetDispositionInfo(Vcb, Fcb, Ccb, FileObject, (PFILE_DISPOSITION_INFORMATION)Buffer);
            break;

        case FileRenameInformation:
            Status = UdnSetRenameInfo(Ctx, Vcb, Fcb, Ccb, (PFILE_RENAME_INFORMATION)Buffer);
            break;

        case FileLinkInformation:
            Status = UdnSetLinkInfo(Ctx, Vcb, Fcb, Ccb, (PFILE_LINK_INFORMATION)Buffer);
            break;

        case FileAllocationInformation:
            Status = UdnSetAllocationInfo(Ctx, Vcb, Fcb, FileObject,
                                          (PFILE_ALLOCATION_INFORMATION)Buffer);
            break;

        case FileEndOfFileInformation:
            Status = UdnSetEndOfFileInfo(Ctx, Vcb, Fcb, FileObject,
                                         (PFILE_END_OF_FILE_INFORMATION)Buffer);
            break;

        default:
            Status = STATUS_INVALID_PARAMETER;
            break;
        }

        if (UdnIsFcb(Fcb)) {
            Fcb->Header.IsFastIoPossible = UdnIsFastIoPossible(Fcb);
        }

    } __finally {

        if (FcbAcquired) {
            UdnRelease(&Fcb->Resource);
        }

        UdnRelease(&Vcb->Resource);
    }

    return Status;
}
