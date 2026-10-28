/*
 * UDFNT - IRP_MJ_FLUSH_BUFFERS, IRP_MJ_SHUTDOWN and the volume's in-use state
 *
 * While UDFNT may change a volume, the volume says so on the disk, as Linux
 * does: with a journal the needs_recovery flag is set (a crash leaves the
 * journal to replay), without one the clean state is cleared (fsck checks
 * it). UfxStartWriting does that at mount; UfxStopWriting - last commit,
 * journal empty, state restored - at lock, dismount and shutdown.
 * Flushing a file or the volume commits the journal.
 */

#include "udn.h"

/* ------------------------------------------------------------------ */
/* In use                                                              */
/* ------------------------------------------------------------------ */

NTSTATUS
UdnStartWriting (
    PUDN_VCB Vcb
    )
{
    NTSTATUS Status;

    if (Vcb->Writing || Vcb->Vol.ReadOnly ||
        (Vcb->VcbState & (VCB_STATE_READ_ONLY | VCB_STATE_SHUTDOWN | VCB_STATE_REMOVED))) {
        return STATUS_SUCCESS;
    }

    UdnEngineEnter(Vcb);
    Status = UfxStartWriting(&Vcb->Vol);
    if (NT_SUCCESS(Status)) {
        Vcb->Writing = TRUE;
    }
    Status = UdnEngineLeave(Vcb, Status);

    if (NT_SUCCESS(Status)) {
        UdnCommitTimerStart(Vcb);
    } else {
        UDN_DBG((UDN_PFX "cannot start writing (%lX): read-only\n", Status));
        Vcb->VcbState |= VCB_STATE_READ_ONLY;
        Vcb->Vol.ReadOnly = TRUE;
    }
    return Status;
}

/* Everything on the disk and the volume clean; the engine not held */
NTSTATUS
UdnStopWriting (
    PUDN_VCB Vcb
    )
{
    NTSTATUS Status;

    if (!Vcb->Writing) {
        return STATUS_SUCCESS;
    }

    UdnCommitTimerStop(Vcb);

    if (Vcb->BootView) {
        UdnUpdateBootView(Vcb);
    }

    UdnEngineEnter(Vcb);
    Status = UfxStopWriting(&Vcb->Vol);
    Vcb->Writing = FALSE;
    Status = UdnEngineLeave(Vcb, Status);
    UDN_DBG((UDN_PFX "%08lX clean (%lX), %lu commits\n", Vcb->SerialNumber, Status, Vcb->Commits));

    if (!NT_SUCCESS(Status)) {
        UDN_DBG((UDN_PFX "stop writing failed %lX\n", Status));
    }
    return Status;
}

/* ------------------------------------------------------------------ */
/* The boot view (see udfshim.c)                                       */
/* ------------------------------------------------------------------ */

static UDF_STATUS
UdnShimCommit (PVOID Ctx)
{
    return UdnCommit((PUDN_VCB)Ctx);
}

/* Files in use as paging files, or with changes not yet in their file entry, stay where they are */
static BOOLEAN
UdnShimCanMove (PVOID Ctx, const UDF_LOC *Loc)
{
    PUDN_VCB Vcb = (PUDN_VCB)Ctx;
    PLIST_ENTRY Entry;
    PUDN_FCB Fcb;
    ULONG Ino = UfxInoOfLoc(&Vcb->Vol, Loc);
    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {
        Fcb = CONTAINING_RECORD(Entry, UDN_FCB, FcbLinks);
        if (Fcb->Ino == Ino &&
            (Fcb->FcbState & (FCB_STATE_PAGING_FILE | FCB_STATE_INODE_DIRTY) || Fcb->Runs != NULL)) {
            return FALSE;
        }
    }
    return TRUE;
}

/* A file's data moved: the FCBs take the new map */
static VOID
UdnShimMoved (PVOID Ctx, const UDF_LOC *Loc)
{
    PUDN_VCB Vcb = (PUDN_VCB)Ctx;
    PLIST_ENTRY Entry;
    PUDN_FCB Fcb;
    ULONG Ino = UfxInoOfLoc(&Vcb->Vol, Loc);
    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {
        Fcb = CONTAINING_RECORD(Entry, UDN_FCB, FcbLinks);
        if (Fcb->Ino == Ino && !(Fcb->FcbState & FCB_STATE_DELETED)) {
            (VOID)UfxReadInode(&Vcb->Vol, Ino, &Fcb->Inode);
            Fcb->MapRun = 0;
        }
    }
}

/*
 * Brings the FAT view NTLDR reads up to date with the files it shows:
 * a boot file that changed, grew, or was replaced shows its current blocks.
 * Called while the volume stops being written (shutdown, lock, dismount),
 * everything flushed; Vcb->Resource exclusive.
 */
VOID
UdnUpdateBootView (
    PUDN_VCB Vcb
    )
{
    UDF_SHIM_HOST Host;
    UDF_SHIM_RESULT Result;
    NTSTATUS Status;

    if (Vcb->Vol.ReadOnly || (Vcb->VcbState & (VCB_STATE_READ_ONLY | VCB_STATE_DISMOUNTED))) {
        return;
    }
    RtlZeroMemory(&Host, sizeof(Host));
    Host.Vol = &Vcb->Vol.Udf;
    Host.Ctx = Vcb;
    Host.Commit = UdnShimCommit;
    Host.CanMove = UdnShimCanMove;
    Host.Moved = UdnShimMoved;

    UdnEngineEnter(Vcb);
    Status = UdnCommit(Vcb);
    if (NT_SUCCESS(Status)) {
        Status = UdfShimUpdate(&Host, NULL, NULL, &Result);
        UDN_DBG((UDN_PFX "boot view: %lX, %lu files, %lu moved in, %lu out%s%s\n", Status, Result.Files,
                 Result.MovedIn, Result.MovedOut, Result.Error ? ": " : "", Result.Error ? Result.Error : ""));
    }
    (VOID)UdnEngineLeave(Vcb, Status);
}

/* Back in use after a lock (or a refused one); Vcb->Resource exclusive */
VOID
UdnResumeWriting (
    PUDN_VCB Vcb
    )
{
    if (!(Vcb->VcbState & (VCB_STATE_DISMOUNTED | VCB_STATE_LOCKED | VCB_STATE_PNP_LOCKED))) {
        (VOID)UdnStartWriting(Vcb);
    }
}

/* ------------------------------------------------------------------ */
/* Flushing                                                            */
/* ------------------------------------------------------------------ */

/* Data and inode of one file; Vcb->Resource exclusive */
NTSTATUS
UdnFlushFile (
    PUDN_VCB Vcb,
    PUDN_FCB Fcb
    )
{
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status = STATUS_SUCCESS;

    (VOID)ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);

    __try {

        if (Fcb->SectionObjectPointers.DataSectionObject != NULL) {

            CcFlushCache(&Fcb->SectionObjectPointers, NULL, 0, &Iosb);
            Status = Iosb.Status;

            /* Let a lazy write in progress finish */
            (VOID)ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
            UdnRelease(&Fcb->PagingIoResource);
        }

        /*
         * ext keeps no valid-data length on disk, so blocks allocated past
         * the last byte written must be zeroed before the larger size
         * reaches the inode, or stale data would show through on the next
         * mount (holes need nothing).
         */
        if (UdnIsFcb(Fcb) && !(Fcb->FcbState & FCB_STATE_DELETED) &&
            Fcb->Header.ValidDataLength.QuadPart < Fcb->Header.FileSize.QuadPart) {

            NTSTATUS Zero = UdnZeroDisk(Vcb, Fcb, Fcb->Header.ValidDataLength.QuadPart,
                                        Fcb->Header.FileSize.QuadPart);

            if (NT_SUCCESS(Zero)) {
                Fcb->Header.ValidDataLength = Fcb->Header.FileSize;
            } else if (NT_SUCCESS(Status)) {
                Status = Zero;
            }
        }

        if ((Fcb->FcbState & FCB_STATE_INODE_DIRTY) && !(Fcb->FcbState & FCB_STATE_DELETED)) {

            if (NT_SUCCESS(Status)) {
                Status = UdnUpdateInode(Vcb, Fcb);
            } else {
                (VOID)UdnUpdateInode(Vcb, Fcb);
            }
        }

    } __finally {

        UdnRelease(&Fcb->Resource);
    }

    return Status;
}

/* Metadata: a journal commit (or, without a journal, the cached blocks) */
static NTSTATUS
UdnFlushMetadata (
    PUDN_VCB Vcb
    )
{
    return UdnCommitNow(Vcb);
}

/*
 * Writes everything cached for the volume; Final also ends its use
 * (journal empty, clean state), for a lock, dismount or shutdown.
 * Vcb->Resource exclusive.
 */
NTSTATUS
UdnFlushVolume (
    PUDN_VCB Vcb,
    BOOLEAN Final
    )
{
    PLIST_ENTRY Entry;
    PUDN_FCB Fcb;
    BOOLEAN Restart;
    NTSTATUS Status = STATUS_SUCCESS;
    NTSTATUS Result;

    if (Vcb->VcbState & (VCB_STATE_READ_ONLY | VCB_STATE_DISMOUNTED)) {
        return STATUS_SUCCESS;
    }

    /* A flush can close file objects, so the list is walked with markers */
    do {

        Restart = FALSE;

        for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {

            Fcb = CONTAINING_RECORD(Entry, UDN_FCB, FcbLinks);

            if ((Fcb->FcbState & FCB_STATE_VISITED) || !UdnIsFcb(Fcb)) {
                continue;
            }

            Fcb->FcbState |= FCB_STATE_VISITED;
            Fcb->RefCount++;

            Result = UdnFlushFile(Vcb, Fcb);
            if (NT_SUCCESS(Status)) {
                Status = Result;
            }

            UdnDereferenceFcb(Vcb, Fcb);

            Restart = TRUE;
            break;
        }

    } while (Restart);

    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {
        Fcb = CONTAINING_RECORD(Entry, UDN_FCB, FcbLinks);
        Fcb->FcbState &= ~FCB_STATE_VISITED;
    }

    Result = UdnFlushMetadata(Vcb);
    if (NT_SUCCESS(Status)) {
        Status = Result;
    }

    if (Final) {
        if (Vcb->Writing) {
            Result = UdnStopWriting(Vcb);
            if (NT_SUCCESS(Status)) {
                Status = Result;
            }
        } else if (Vcb->BootView && (Vcb->VcbState & VCB_STATE_SHUTDOWN)) {
            /* the last shutdown call (after the registry's last flush): the view once more */
            UdnUpdateBootView(Vcb);
        }
    }

    (VOID)UdnFlushDevice(Vcb);

    return Status;
}

/* ------------------------------------------------------------------ */
/* Requests                                                            */
/* ------------------------------------------------------------------ */

NTSTATUS
UdnCommonFlushBuffers (
    PUDN_IRP_CONTEXT Ctx
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PUDN_FCB Fcb = (PUDN_FCB)FileObject->FsContext;
    PUDN_VCB Vcb = Ctx->Vcb;
    NTSTATUS Status;

    if (Fcb == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Status = UdnVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status) || (Vcb->VcbState & VCB_STATE_READ_ONLY)) {
            __leave;
        }

        if (UdnIsVfcb(Fcb)) {

            Status = UdnFlushVolume(Vcb, FALSE);

        } else {

            if (UdnIsFcb(Fcb)) {
                Status = UdnFlushFile(Vcb, Fcb);
            }

            if (NT_SUCCESS(Status)) {
                Status = UdnFlushMetadata(Vcb);
            } else {
                (VOID)UdnFlushMetadata(Vcb);
            }

            (VOID)UdnFlushDevice(Vcb);
        }

    } __finally {

        UdnRelease(&Vcb->Resource);
    }

    return Status;
}

/* Sent to the file system device: leave every volume clean */
NTSTATUS
UdnCommonShutdown (
    PUDN_IRP_CONTEXT Ctx
    )
{
    PLIST_ENTRY Entry;
    PUDN_VCB Vcb;

    UNREFERENCED_PARAMETER(Ctx);

    UDN_DBG((UDN_PFX "shutdown\n"));
    (VOID)ExAcquireResourceSharedLite(&UdnData.Resource, TRUE);

    __try {

        for (Entry = UdnData.VcbList.Flink; Entry != &UdnData.VcbList; Entry = Entry->Flink) {

            Vcb = CONTAINING_RECORD(Entry, UDN_VCB, VcbLinks);

            (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

            /* (a locked volume is the locker's: it was left clean at the lock) */
            if ((Vcb->VcbState & VCB_STATE_MOUNTED) && !(Vcb->VcbState & VCB_STATE_LOCKED)) {
                /* From now on every change goes to the disk at once (UdnEngineLeave):
                   the journal is closed and the volume marked clean */
                Vcb->VcbState |= VCB_STATE_SHUTDOWN;
                (VOID)UdnFlushVolume(Vcb, TRUE);
            }

            UdnRelease(&Vcb->Resource);
        }

    } __finally {

        UdnRelease(&UdnData.Resource);
    }

    return STATUS_SUCCESS;
}
