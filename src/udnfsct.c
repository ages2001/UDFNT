/*
 * UDFNT - IRP_MJ_FILE_SYSTEM_CONTROL (mount, verify, lock/unlock,
 * dismount, queries, UDFNT's own controls) and IRP_MJ_PNP
 */

#include "udn.h"
#include "udnctl.h"

/* ------------------------------------------------------------------ */
/* Mount                                                               */
/* ------------------------------------------------------------------ */

/* Hot-plug devices (W2K) get the removable-media flush policy */
#define UDN_IOCTL_GET_HOTPLUG_INFO CTL_CODE(0x0000002d, 0x0305, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _UDN_HOTPLUG_INFO {
    ULONG       Size;
    BOOLEAN     MediaRemovable;
    BOOLEAN     MediaHotplug;
    BOOLEAN     DeviceHotplug;
    BOOLEAN     WriteCacheEnableOverride;
} UDN_HOTPLUG_INFO;

static VOID
UdnCheckDeviceState (
    PUDN_VCB Vcb,
    PDEVICE_OBJECT TargetDevice
    )
{
    UDN_HOTPLUG_INFO Hotplug;
    NTSTATUS Status;

    Status = UdnDeviceIoctl(TargetDevice, IOCTL_DISK_IS_WRITABLE, NULL, 0, NULL, 0, TRUE, NULL);

    if (Status == STATUS_MEDIA_WRITE_PROTECTED || !UdnWriteSupportEnabled()) {
        Vcb->VcbState |= VCB_STATE_READ_ONLY;
    }

    /* a CD / DVD / BD drive is written only if it says it can be (a DVD-RAM, say): pressed and
       recordable discs, and drives that answer nothing, are mounted read-only */
    if (Vcb->Optical && !NT_SUCCESS(Status)) {
        Vcb->VcbState |= VCB_STATE_READ_ONLY;
    }

    if (Vcb->VcbState & VCB_STATE_REMOVABLE) {
        Vcb->VcbState |= VCB_STATE_FLUSH_ON_CLOSE;
    }

    RtlZeroMemory(&Hotplug, sizeof(Hotplug));

    Status = UdnDeviceIoctl(TargetDevice, UDN_IOCTL_GET_HOTPLUG_INFO, NULL, 0,
                             &Hotplug, sizeof(Hotplug), TRUE, NULL);

    if (NT_SUCCESS(Status) && (Hotplug.MediaRemovable || Hotplug.DeviceHotplug)) {
        Vcb->VcbState |= VCB_STATE_FLUSH_ON_CLOSE;
    }
}

/* The label as NT shows it: the logical volume identifier (the first 32 characters) */
VOID
UdnLoadLabel (
    PUDN_VCB Vcb
    )
{
    UNICODE_STRING U;
    ULONG Bytes = 0;
    ULONG i;
    PVPB Vpb = Vcb->Vpb;

    while (Bytes < 255 && Vcb->Vol.VolumeName[Bytes] != 0) {
        Bytes++;
    }

    Vcb->LabelLength = 0;
    if (Bytes != 0 && NT_SUCCESS(UdnExtToUnicode((const UCHAR *)Vcb->Vol.VolumeName, Bytes, &U))) {
        Vcb->LabelLength = U.Length / sizeof(WCHAR);
        if (Vcb->LabelLength > UDN_MAX_LABEL) {
            Vcb->LabelLength = UDN_MAX_LABEL;
        }
        RtlCopyMemory(Vcb->Label, U.Buffer, Vcb->LabelLength * sizeof(WCHAR));
        ExFreePool(U.Buffer);
        /* a label is not a file name: no escapes for a trailing space or dot */
        for (i = 0; i < Vcb->LabelLength; i++) {
            if (Vcb->Label[i] >= 0xF001 && Vcb->Label[i] <= 0xF07F) Vcb->Label[i] -= 0xF000;
        }
    }

    Vpb->VolumeLabelLength = (USHORT)(Vcb->LabelLength * sizeof(WCHAR));
    for (i = 0; i < Vcb->LabelLength; i++) {
        Vpb->VolumeLabel[i] = Vcb->Label[i];
    }
}

/*
 * A UDF volume recognition sequence at byte 32768. Its descriptors are 2048
 * bytes (or a sector) apart, or - as udffmt and mkudffs write them for blocks
 * larger than 2048 bytes - a block apart: when the first one is there but the
 * sequence does not follow, the wider spacings up to 64 KB are tried, the
 * descriptors gathered Len bytes apart in Buffer (16 * Len bytes, Len at
 * most 4096).
 */
static BOOLEAN
UdnProbeVrs (
    PDEVICE_OBJECT Device,
    ULONG SectorSize,
    PUCHAR Buffer
    )
{
    ULONG Len = SectorSize > 2048 ? SectorSize : 2048;
    ULONG Step, i;

    if (!NT_SUCCESS(UdnReadSectors(Device, 32768, 16 * Len, Buffer, TRUE))) {
        return FALSE;
    }
    if (UfxIsVrs(Buffer, 16 * Len, Len)) {
        return TRUE;
    }
    if (RtlCompareMemory(Buffer + 1, "BEA01", 5) != 5) {
        return FALSE;                           /* no recognition sequence at all */
    }
    for (Step = Len * 2; Step <= 65536; Step *= 2) {
        RtlZeroMemory(Buffer, 16 * Len);
        for (i = 0; i < 16; i++) {
            if (!NT_SUCCESS(UdnReadSectors(Device, 32768 + (ULONGLONG)i * Step, Len, Buffer + i * Len, TRUE))) {
                break;
            }
        }
        if (UfxIsVrs(Buffer, i * Len, Len)) {
            return TRUE;
        }
    }
    return FALSE;
}

#define UDN_PROBE_BYTES         (16 * 4096)

/* Is the UDF volume this VCB had still on the disk? */
BOOLEAN
UdnSameVolume (
    PUDN_VCB Vcb
    )
{
    BOOLEAN Same;
    UdnEngineEnter(Vcb);
    Same = UfxSameVolume(&Vcb->Vol);
    UdnRelease(&Vcb->EngineResource);
    return Same;
}

/*
 * (Re)reads the engine's view of the volume: superblock, chunk map, roots,
 * block groups. Vcb->Resource exclusive; writing not started.
 */
NTSTATUS
UdnLoadVolume (
    PUDN_VCB Vcb
    )
{
    BOOLEAN ReadOnly = (BOOLEAN)((Vcb->VcbState & VCB_STATE_READ_ONLY) != 0);
    NTSTATUS Status;

    UdnEngineEnter(Vcb);

    UfxUnmountFree(&Vcb->Vol);
    RtlZeroMemory(&Vcb->Vol, sizeof(UFX_VOL));
    Vcb->Vol.HostContext = Vcb;
    Vcb->Vol.DeviceBytes = (ULONGLONG)Vcb->PartitionBytes;
    Vcb->Vol.DeviceSector = Vcb->SectorSize;
    Vcb->Vol.Optical = Vcb->Optical;
    Vcb->Writing = FALSE;

    Status = UfxMount(&Vcb->Vol, ReadOnly);

    if (NT_SUCCESS(Status) && Vcb->PartitionBytes != 0 &&
        (Vcb->Vol.BlocksCount << Vcb->Vol.BlockShift) > (ULONGLONG)Vcb->PartitionBytes) {
        UDN_DBG((UDN_PFX "file system (%I64u blocks) larger than the partition\n",
                 Vcb->Vol.BlocksCount));
        Status = STATUS_DISK_CORRUPT_ERROR;
    }

    if (NT_SUCCESS(Status)) {
        if (Vcb->Vol.ReadOnly) {
            Vcb->VcbState |= VCB_STATE_READ_ONLY;
        }
        Vcb->VolumeBytes = (LONGLONG)(Vcb->Vol.BlocksCount << Vcb->Vol.BlockShift);
        if (Vcb->PartitionBytes < Vcb->VolumeBytes) {
            Vcb->PartitionBytes = Vcb->VolumeBytes;
        }
        Vcb->SerialNumber = Vcb->Vol.SerialNumber;
        /* made bootable by udfboot? (the engine found the view and keeps its window) */
        Vcb->BootView = (BOOLEAN)(Vcb->Vol.Udf.AvoidHi > Vcb->Vol.Udf.AvoidLo);
    } else {
        UfxUnmountFree(&Vcb->Vol);
    }

    UdnRelease(&Vcb->EngineResource);

    if (Status == UFX_CORRUPT || Status == UFX_NOT_SUPPORTED || Status == UFX_UNRECOGNIZED) {
        Status = STATUS_UNRECOGNIZED_VOLUME;
    }
    return Status;
}

/* After a mount or a reload: in use, and what a crashed Linux left on the orphan list */
static VOID
UdnBeginUse (
    PUDN_VCB Vcb
    )
{
    NTSTATUS Status;

    if (Vcb->VcbState & VCB_STATE_READ_ONLY) {
        return;
    }
    Status = UdnStartWriting(Vcb);
    if (NT_SUCCESS(Status) && Vcb->Writing) {
        UdnEngineEnter(Vcb);
        Status = UdnEngineLeave(Vcb, UfxProcessOrphans(&Vcb->Vol));
        if (!NT_SUCCESS(Status)) {
            UDN_DBG((UDN_PFX "orphan list: %lX\n", Status));
        }
    }
}

/* ntddcdrm.h: CTL_CODE(FILE_DEVICE_CD_ROM, 0x13, METHOD_BUFFERED, FILE_READ_ACCESS) */
#define UDN_IOCTL_CDROM_GET_DRIVE_GEOMETRY  0x0002404CUL

/* ntddcdrm.h: IOCTL_CDROM_READ_TOC, CTL_CODE(FILE_DEVICE_CD_ROM, 0, METHOD_BUFFERED, FILE_READ_ACCESS) */
#define UDN_IOCTL_CDROM_READ_TOC            0x00024000UL

/* The bytes of a disc: the lead-out's address in the table of contents (MSF), or 0 */
static LONGLONG
UdnDiscBytes (
    PDEVICE_OBJECT Device,
    ULONG SectorSize
    )
{
    PUCHAR Toc;
    LONGLONG Bytes = 0;
    ULONG Got = 0;
    Toc = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, 804, UDN_TAG_BUFFER);
    if (Toc == NULL) {
        return 0;
    }
    RtlZeroMemory(Toc, 804);
    if (NT_SUCCESS(UdnDeviceIoctl(Device, UDN_IOCTL_CDROM_READ_TOC, NULL, 0, Toc, 804, TRUE, &Got)) && Got >= 12) {
        ULONG First = Toc[2], Last = Toc[3], Entry = 4 + 8 * (Last - First + 1);
        if (Last >= First && Last - First < 99 && Entry + 8 <= 804 && Toc[Entry + 2] == 0xAA) {
            ULONG Lba = ((ULONG)Toc[Entry + 5] * 60 + Toc[Entry + 6]) * 75 + Toc[Entry + 7];
            if (Lba > 150) {
                Bytes = (LONGLONG)(Lba - 150) * SectorSize;
            }
        }
    }
    ExFreePool(Toc);
    return Bytes;
}

static NTSTATUS
UdnMountVolume (
    PUDN_IRP_CONTEXT Ctx
    )
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PDEVICE_OBJECT TargetDevice = IrpSp->Parameters.MountVolume.DeviceObject;
    PVPB Vpb = IrpSp->Parameters.MountVolume.Vpb;
    PDEVICE_OBJECT RealDevice = Vpb->RealDevice;
    DISK_GEOMETRY Geometry;
    PARTITION_INFORMATION Partition;
    PDEVICE_OBJECT VolumeDevice = NULL;
    PUDN_VCB Vcb = NULL;
    PUCHAR Super = NULL;
    LONGLONG PartitionBytes;
    BOOLEAN ClearedVerify = FALSE;
    BOOLEAN Resources = FALSE;
    NTSTATUS Status;

    RtlZeroMemory(&Geometry, sizeof(Geometry));
    UDN_DBG((UDN_PFX "mount offered: device %p (type %lu)\n", TargetDevice, (ULONG)TargetDevice->DeviceType));
    Status = UdnDeviceIoctl(TargetDevice, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0,
                             &Geometry, sizeof(Geometry), TRUE, NULL);
    if (!NT_SUCCESS(Status) && TargetDevice->DeviceType == FILE_DEVICE_CD_ROM) {
        /* CD / DVD / BD drives: their own geometry request (the size from the medium's capacity) */
        Status = UdnDeviceIoctl(TargetDevice, UDN_IOCTL_CDROM_GET_DRIVE_GEOMETRY, NULL, 0,
                                 &Geometry, sizeof(Geometry), TRUE, NULL);
    }
    if (!NT_SUCCESS(Status) || Geometry.BytesPerSector < 512 || Geometry.BytesPerSector > 4096 ||
        (Geometry.BytesPerSector & (Geometry.BytesPerSector - 1))) {
        UDN_DBG((UDN_PFX "  no geometry (%lX, %lu bytes a sector)\n", Status, Geometry.BytesPerSector));
        return STATUS_UNRECOGNIZED_VOLUME;
    }

    RtlZeroMemory(&Partition, sizeof(Partition));
    Status = UdnDeviceIoctl(TargetDevice, IOCTL_DISK_GET_PARTITION_INFO, NULL, 0,
                             &Partition, sizeof(Partition), TRUE, NULL);
    PartitionBytes = NT_SUCCESS(Status) ? Partition.PartitionLength.QuadPart : 0;
    if (PartitionBytes == 0 && TargetDevice->DeviceType == FILE_DEVICE_CD_ROM) {
        /* a disc: up to the lead-out of its last session (the geometry rounds down to 4 MB) */
        PartitionBytes = UdnDiscBytes(TargetDevice, Geometry.BytesPerSector);
    }
    if (PartitionBytes == 0) {
        /* Floppies and superfloppies: the geometry gives the size */
        PartitionBytes = Geometry.Cylinders.QuadPart * Geometry.TracksPerCylinder *
                         Geometry.SectorsPerTrack * Geometry.BytesPerSector;
    }

    Super = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, UDN_PROBE_BYTES, UDN_TAG_BUFFER);
    if (Super == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    __try {

        /* Cheap test first: most volumes offered to us are not UDF */
        if (!UdnProbeVrs(TargetDevice, Geometry.BytesPerSector, Super)) {
            UDN_DBG((UDN_PFX "  no UDF recognition sequence\n"));
            Status = STATUS_UNRECOGNIZED_VOLUME;
            __leave;
        }

        Status = IoCreateDevice(UdnData.DriverObject,
                                sizeof(UDN_VCB),
                                NULL,
                                (TargetDevice->DeviceType == FILE_DEVICE_CD_ROM ||
                                 TargetDevice->DeviceType == FILE_DEVICE_DVD) ?
                                    FILE_DEVICE_CD_ROM_FILE_SYSTEM : FILE_DEVICE_DISK_FILE_SYSTEM,
                                0,
                                FALSE,
                                &VolumeDevice);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        VolumeDevice->StackSize = (CCHAR)(TargetDevice->StackSize + 1);

        if (TargetDevice->AlignmentRequirement > VolumeDevice->AlignmentRequirement) {
            VolumeDevice->AlignmentRequirement = TargetDevice->AlignmentRequirement;
        }

        VolumeDevice->SectorSize = (USHORT)Geometry.BytesPerSector;

#ifdef DO_DEVICE_INITIALIZING
        VolumeDevice->Flags &= ~DO_DEVICE_INITIALIZING;
#endif

        Vcb = (PUDN_VCB)VolumeDevice->DeviceExtension;
        RtlZeroMemory(Vcb, sizeof(UDN_VCB));

        Vcb->NodeTypeCode = UDN_NTC_VCB;
        Vcb->NodeByteSize = (CSHORT)sizeof(UDN_VCB);
        ExInitializeResourceLite(&Vcb->Resource);
        ExInitializeResourceLite(&Vcb->EngineResource);
        Resources = TRUE;
        InitializeListHead(&Vcb->FcbList);
        InitializeListHead(&Vcb->DirNotifyList);
        InitializeListHead(&Vcb->VcbLinks);
        FsRtlNotifyInitializeSync(&Vcb->NotifySync);
        UdnMetaInitialize(Vcb);

        Vcb->Vpb = Vpb;
        Vcb->TargetDeviceObject = TargetDevice;
        Vcb->VolumeDeviceObject = VolumeDevice;
        Vcb->VerifyThread = KeGetCurrentThread();
        Vcb->PartitionBytes = PartitionBytes;
        Vcb->SectorSize = Geometry.BytesPerSector;
        for (Vcb->SectorShift = 9; (1UL << Vcb->SectorShift) < Vcb->SectorSize; Vcb->SectorShift++)
            ;
        Vcb->Optical = (BOOLEAN)(TargetDevice->DeviceType == FILE_DEVICE_CD_ROM ||
                                 TargetDevice->DeviceType == FILE_DEVICE_DVD);

        if (RealDevice->Characteristics & FILE_REMOVABLE_MEDIA) {
            Vcb->VcbState |= VCB_STATE_REMOVABLE;
        }

        UdnCheckDeviceState(Vcb, TargetDevice);

        if (RealDevice->Flags & DO_VERIFY_VOLUME) {
            RealDevice->Flags &= ~DO_VERIFY_VOLUME;
            ClearedVerify = TRUE;
        }

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
        Status = UdnLoadVolume(Vcb);
        UdnRelease(&Vcb->Resource);
        if (!NT_SUCCESS(Status)) {
            UDN_DBG((UDN_PFX "mount failed %lX\n", Status));
            if (Status == STATUS_DISK_CORRUPT_ERROR || Status == STATUS_FILE_CORRUPT_ERROR) {
                Status = STATUS_UNRECOGNIZED_VOLUME;
            }
            __leave;
        }

        Vcb->RootDcb = UdnCreateRootDcb(Vcb);
        Vcb->VolumeFcb = UdnCreateVolumeFcb(Vcb);
        if (Vcb->RootDcb == NULL || Vcb->VolumeFcb == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        UdnLoadLabel(Vcb);

        Vpb->SerialNumber = Vcb->SerialNumber;
        Vpb->DeviceObject = VolumeDevice;
        Vcb->VcbState |= VCB_STATE_MOUNTED;
        Vcb->VerifyThread = NULL;

        (VOID)ObReferenceObjectByPointer(TargetDevice, 0, NULL, KernelMode);

        (VOID)ExAcquireResourceExclusiveLite(&UdnData.Resource, TRUE);
        InsertTailList(&UdnData.VcbList, &Vcb->VcbLinks);
        UdnRelease(&UdnData.Resource);

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
        UdnBeginUse(Vcb);
        UdnRelease(&Vcb->Resource);

        /*
         * Claim the drive letter the registry binds to this volume, now, before
         * the session manager runs: the boot volume needs its letter (usually
         * C:) even on an unrecognized (Linux-type 83h) partition, which the I/O
         * manager would otherwise leave unlettered. No-op on NT 3.1 - 4.0 (no
         * MountedDevices key) and when the letter is already assigned.
         */
        UdnClaimRegistryLetter(Vcb, TargetDevice, Partition.StartingOffset.QuadPart);

        UDN_DBG((UDN_PFX "Mounted UDF %x.%02x %08lX: %I64u blocks of %lu%s%s\n",
                 Vcb->Vol.Udf.Revision >> 8, Vcb->Vol.Udf.Revision & 0xFF,
                 Vcb->SerialNumber, Vcb->Vol.BlocksCount, Vcb->Vol.BlockSize,
                 Vcb->Vol.ReadOnlyForced ? ", read-only (features)" : "",
                 (Vcb->VcbState & VCB_STATE_READ_ONLY) ? ", read-only" : ""));

    } __finally {

        if (Super != NULL) {
            ExFreePool(Super);
        }

        if (!NT_SUCCESS(Status)) {

            if (Vcb != NULL) {

                Vpb->DeviceObject = NULL;

                if (ClearedVerify) {
                    RealDevice->Flags |= DO_VERIFY_VOLUME;
                }

                while (!IsListEmpty(&Vcb->FcbList)) {
                    UdnDeleteFcb(CONTAINING_RECORD(Vcb->FcbList.Flink, UDN_FCB, FcbLinks));
                }
                if (Vcb->VolumeFcb != NULL) {
                    UdnDeleteFcb(Vcb->VolumeFcb);
                }

                UfxUnmountFree(&Vcb->Vol);
                UdnBounceFree(Vcb);
                FsRtlNotifyUninitializeSync(&Vcb->NotifySync);
                if (Resources) {
                    ExDeleteResourceLite(&Vcb->EngineResource);
                    ExDeleteResourceLite(&Vcb->Resource);
                }
            }

            if (VolumeDevice != NULL) {
                IoDeleteDevice(VolumeDevice);
            }
        }
    }

    return Status;
}

/* ------------------------------------------------------------------ */
/* Verify                                                              */
/* ------------------------------------------------------------------ */

static NTSTATUS
UdnVerifyVolume (
    PUDN_IRP_CONTEXT Ctx
    )
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PVPB Vpb = IrpSp->Parameters.VerifyVolume.Vpb;
    PDEVICE_OBJECT VolumeDevice = IrpSp->Parameters.VerifyVolume.DeviceObject;
    PUDN_VCB Vcb;
    PUCHAR Super = NULL;
    BOOLEAN Delete = FALSE;
    NTSTATUS Status = STATUS_SUCCESS;

    if (VolumeDevice == NULL || VolumeDevice->DriverObject != UdnData.DriverObject ||
        UdnIsFsDevice(VolumeDevice)) {

        return STATUS_WRONG_VOLUME;
    }

    Vcb = (PUDN_VCB)VolumeDevice->DeviceExtension;

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        if (Vcb->VcbState & VCB_STATE_DISMOUNTED) {
            Status = STATUS_WRONG_VOLUME;
            __leave;
        }

        if (!(Vpb->RealDevice->Flags & DO_VERIFY_VOLUME)) {
            __leave;
        }

        Vcb->VerifyThread = KeGetCurrentThread();

        if (UdnSameVolume(Vcb)) {

            /* Same volume; it may have been changed while away (another
               system wrote it): read it again */
            (VOID)UdnStopWriting(Vcb);
            UdnPurgeCachedFiles(Vcb);
            Status = UdnLoadVolume(Vcb);
            if (NT_SUCCESS(Status)) {
                UdnLoadLabel(Vcb);
                UdnBeginUse(Vcb);
            } else {
                Delete = UdnDismountVcb(Vcb);
                Status = STATUS_WRONG_VOLUME;
            }

        } else {

            UDN_DBG((UDN_PFX "Verify: volume %08lX is gone\n", Vcb->SerialNumber));

            Delete = UdnDismountVcb(Vcb);
            Status = STATUS_WRONG_VOLUME;
        }

        Vpb->RealDevice->Flags &= ~DO_VERIFY_VOLUME;

    } __finally {

        if (Super != NULL) {
            ExFreePool(Super);
        }

        Vcb->VerifyThread = NULL;
        UdnRelease(&Vcb->Resource);
    }

    if (Delete) {
        UdnDeleteVcb(Vcb);
        Ctx->Vcb = NULL;
    }

    return Status;
}

/* ------------------------------------------------------------------ */
/* User requests                                                       */
/* ------------------------------------------------------------------ */

static NTSTATUS
UdnGetVolumeOpen (
    PUDN_IRP_CONTEXT Ctx,
    PUDN_CCB *Ccb
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PUDN_FCB Fcb;

    if (FileObject == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    Fcb = (PUDN_FCB)FileObject->FsContext;
    *Ccb = (PUDN_CCB)FileObject->FsContext2;

    if (Fcb == NULL || *Ccb == NULL || !UdnIsVfcb(Fcb)) {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

static VOID
UdnNotifyVolume (
    PFILE_OBJECT FileObject,
    ULONG Event
    )
{
#ifndef UDN_NT4
    (VOID)FsRtlNotifyVolumeEvent(FileObject, Event);
#else
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(Event);
#endif
}

#ifndef FSRTL_VOLUME_DISMOUNT
#define FSRTL_VOLUME_DISMOUNT       1
#endif
#ifndef FSRTL_VOLUME_LOCK
#define FSRTL_VOLUME_LOCK           3
#endif
#ifndef FSRTL_VOLUME_LOCK_FAILED
#define FSRTL_VOLUME_LOCK_FAILED    4
#endif
#ifndef FSRTL_VOLUME_UNLOCK
#define FSRTL_VOLUME_UNLOCK         5
#endif

static NTSTATUS
UdnLockVolume (
    PUDN_IRP_CONTEXT Ctx
    )
{
    PUDN_VCB Vcb = Ctx->Vcb;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PUDN_CCB Ccb;
    KIRQL Irql;
    NTSTATUS Status;

    Status = UdnGetVolumeOpen(Ctx, &Ccb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    UdnNotifyVolume(FileObject, FSRTL_VOLUME_LOCK);

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        if (Vcb->VcbState & (VCB_STATE_LOCKED | VCB_STATE_PNP_LOCKED)) {
            Status = STATUS_ACCESS_DENIED;
            __leave;
        }

        /* Whoever locks is about to work on the disk itself */
        if (!(Vcb->VcbState & VCB_STATE_DISMOUNTED)) {
            (VOID)UdnFlushVolume(Vcb, TRUE);
            UdnPurgeCachedFiles(Vcb);
        }

        /* Only the caller's handle may be open */
        if (Vcb->UncleanCount != 1) {
            Status = STATUS_ACCESS_DENIED;
            __leave;
        }

        Vcb->VcbState |= VCB_STATE_LOCKED;
        Vcb->LockFileObject = FileObject;

        IoAcquireVpbSpinLock(&Irql);
        Vcb->Vpb->Flags |= VPB_LOCKED;
        IoReleaseVpbSpinLock(Irql);

    } __finally {

        UdnRelease(&Vcb->Resource);
    }

    if (!NT_SUCCESS(Status)) {
        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
        UdnResumeWriting(Vcb);
        UdnRelease(&Vcb->Resource);
        UdnNotifyVolume(FileObject, FSRTL_VOLUME_LOCK_FAILED);
    }

    return Status;
}

static NTSTATUS
UdnUnlockVolume (
    PUDN_IRP_CONTEXT Ctx
    )
{
    PUDN_VCB Vcb = Ctx->Vcb;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PUDN_CCB Ccb;
    NTSTATUS Status;

    Status = UdnGetVolumeOpen(Ctx, &Ccb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        if (Vcb->LockFileObject != FileObject) {

            Status = STATUS_NOT_LOCKED;

        } else {

            UdnUnlockVcb(Vcb);

            /* Metadata may have been rewritten under us */
            if (Vcb->VcbState & VCB_STATE_DASD_WRITTEN) {
                (VOID)UdnDismountVcb(Vcb);
            } else {
                UdnResumeWriting(Vcb);
            }
        }

    } __finally {

        UdnRelease(&Vcb->Resource);
    }

    if (NT_SUCCESS(Status)) {
        UdnNotifyVolume(FileObject, FSRTL_VOLUME_UNLOCK);
    }

    return Status;
}

static NTSTATUS
UdnDismountVolume (
    PUDN_IRP_CONTEXT Ctx
    )
{
    PUDN_VCB Vcb = Ctx->Vcb;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PUDN_CCB Ccb;
    NTSTATUS Status;

    Status = UdnGetVolumeOpen(Ctx, &Ccb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    /*
     * Programs running from the volume (the system volume while autochk
     * runs, say) cannot be dismounted from under: refuse, as a lock would be.
     */
    {
        PLIST_ENTRY Entry;
        for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {
            PUDN_FCB Fcb = CONTAINING_RECORD(Entry, UDN_FCB, FcbLinks);
            if ((Fcb->FcbState & FCB_STATE_PAGING_FILE) ||
                (Fcb->SectionObjectPointers.ImageSectionObject != NULL &&
                 !MmFlushImageSection(&Fcb->SectionObjectPointers, MmFlushForWrite))) {
                UdnRelease(&Vcb->Resource);
                UDN_DBG((UDN_PFX "dismount refused: the volume is in use by running programs\n"));
                return STATUS_ACCESS_DENIED;
            }
        }
    }

    UdnNotifyVolume(FileObject, FSRTL_VOLUME_DISMOUNT);

    __try {

        if (!(Vcb->VcbState & VCB_STATE_DISMOUNTED)) {
            (VOID)UdnFlushVolume(Vcb, TRUE);
        }

        /* The caller's handle stays open, so the VCB cannot go away here */
        Ccb->Flags |= CCB_FLAG_DISMOUNTED_VOLUME;
        (VOID)UdnDismountVcb(Vcb);

    } __finally {

        UdnRelease(&Vcb->Resource);
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
UdnIsVolumeDirty (
    PUDN_IRP_CONTEXT Ctx
    )
{
    PUDN_VCB Vcb = Ctx->Vcb;
    PIRP Irp = Ctx->Irp;
    PULONG Result = (PULONG)Irp->AssociatedIrp.SystemBuffer;

    if (UdnXSp(Ctx->IrpSp)->Parameters.FileSystemControl.OutputBufferLength < sizeof(ULONG)) {
        return STATUS_INVALID_PARAMETER;
    }

    /* the integrity descriptor was open when mounted (not closed cleanly) */
    *Result = Vcb->Vol.Dirty ? VOLUME_IS_DIRTY : 0;
    Irp->IoStatus.Information = sizeof(ULONG);

    return STATUS_SUCCESS;
}

static NTSTATUS
UdnMarkVolumeDirtyRequest (
    PUDN_IRP_CONTEXT Ctx
    )
{
    PUDN_VCB Vcb = Ctx->Vcb;
    NTSTATUS Status;

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Status = UdnVerifyWritable(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        /* remembered until the volume goes (an open integrity descriptor says the same) */
        Vcb->Vol.Dirty = TRUE;
        Status = STATUS_SUCCESS;

    } __finally {

        UdnRelease(&Vcb->Resource);
    }

    return Status;
}

/* UDFNT's own controls (extctl.h) */
static NTSTATUS
UdnPrivateRequest (
    PUDN_IRP_CONTEXT Ctx,
    ULONG Code
    )
{
    PUDN_VCB Vcb = Ctx->Vcb;
    PIRP Irp = Ctx->Irp;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PUDN_FCB Fcb = (FileObject != NULL) ? (PUDN_FCB)FileObject->FsContext : NULL;
    ULONG InLength = UdnXSp(Ctx->IrpSp)->Parameters.FileSystemControl.InputBufferLength;
    ULONG OutLength = UdnXSp(Ctx->IrpSp)->Parameters.FileSystemControl.OutputBufferLength;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    BOOLEAN FcbAcquired = FALSE;
    NTSTATUS Status;

    if (Fcb == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Status = UdnVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        switch (Code) {

        case UDN_FSCTL_QUERY_VOLUME: {
            PUDN_VOLUME_INFO Info = (PUDN_VOLUME_INFO)Buffer;
            ULONG i;
            if (OutLength < sizeof(UDN_VOLUME_INFO)) {
                Status = STATUS_BUFFER_TOO_SMALL;
                __leave;
            }
            RtlZeroMemory(Info, sizeof(UDN_VOLUME_INFO));
            UdnEngineEnter(Vcb);
            Info->Version = 1;
            Info->BlockSize = Vcb->Vol.BlockSize;
            Info->BlocksCount = Vcb->Vol.BlocksCount;
            UfxCountFree(&Vcb->Vol);
            Info->FreeBlocks = Vcb->Vol.FreeBlocksCount;
            Info->DataBlocks = Vcb->Vol.DataBlocks;
            Info->Revision = Vcb->Vol.Udf.Revision;
            Info->Maps = Vcb->Vol.Udf.NMaps;
            Info->Files = Vcb->Vol.Udf.NumFiles;
            Info->Directories = Vcb->Vol.Udf.NumDirs;
            Info->ReadOnly = (Vcb->VcbState & VCB_STATE_READ_ONLY) ? 1 : 0;
            Info->ReadOnlyForced = Vcb->Vol.ReadOnlyForced ? 1 : 0;
            Info->Commits = Vcb->Commits;
            RtlCopyMemory(Info->Uuid, Vcb->Vol.Uuid, 16);
            for (i = 0; i < sizeof(Info->Label) - 1 && Vcb->Vol.VolumeName[i]; i++) {
                Info->Label[i] = Vcb->Vol.VolumeName[i];
            }
            UdnRelease(&Vcb->EngineResource);
            Irp->IoStatus.Information = sizeof(UDN_VOLUME_INFO);
            break;
        }

        case UDN_FSCTL_COMMIT:
            if (!(Vcb->VcbState & VCB_STATE_READ_ONLY)) {
                Status = UdnFlushVolume(Vcb, FALSE);
            }
            break;

        case UDN_FSCTL_GET_POSIX: {
            PUDN_POSIX_INFO Out = (PUDN_POSIX_INFO)Buffer;
            PUFX_INODE I;
            if (UdnIsVfcb(Fcb)) {
                Status = STATUS_INVALID_PARAMETER;
                __leave;
            }
            if (OutLength < sizeof(UDN_POSIX_INFO)) {
                Status = STATUS_BUFFER_TOO_SMALL;
                __leave;
            }
            (VOID)ExAcquireResourceSharedLite(&Fcb->Resource, TRUE);
            FcbAcquired = TRUE;
            I = &Fcb->Inode;
            RtlZeroMemory(Out, sizeof(UDN_POSIX_INFO));
            Out->Valid = UDN_POSIX_MODE | UDN_POSIX_UID | UDN_POSIX_GID;
            Out->Mode = I->Mode;
            Out->Uid = (ULONG)I->UidLo | ((ULONG)I->UidHi << 16);
            Out->Gid = (ULONG)I->GidLo | ((ULONG)I->GidHi << 16);
            Out->Links = I->LinksCount;
            Out->Inode = Fcb->Ino;
            Out->Flags = I->Flags;
            Out->Generation = I->Generation;
            Irp->IoStatus.Information = sizeof(UDN_POSIX_INFO);
            break;
        }

        case UDN_FSCTL_SET_POSIX: {
            PUDN_POSIX_INFO In = (PUDN_POSIX_INFO)Buffer;
            UFX_INODE Before;
            if (UdnIsVfcb(Fcb)) {
                Status = STATUS_INVALID_PARAMETER;
                __leave;
            }
            if (InLength < sizeof(UDN_POSIX_INFO)) {
                Status = STATUS_INVALID_PARAMETER;
                __leave;
            }
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
            if (Fcb->FcbState & FCB_STATE_INODE_DIRTY) {
                Status = UdnUpdateInode(Vcb, Fcb);
                if (!NT_SUCCESS(Status)) {
                    __leave;
                }
            }
            UdnEngineEnter(Vcb);
            Before = Fcb->Inode;
            if (In->Valid & UDN_POSIX_MODE) {
                Fcb->Inode.Mode = (USHORT)((Fcb->Inode.Mode & UFX_S_IFMT) | (In->Mode & 07777));
            }
            if (In->Valid & UDN_POSIX_UID) {
                Fcb->Inode.UidLo = (USHORT)In->Uid;
                Fcb->Inode.UidHi = (USHORT)(In->Uid >> 16);
            }
            if (In->Valid & UDN_POSIX_GID) {
                Fcb->Inode.GidLo = (USHORT)In->Gid;
                Fcb->Inode.GidHi = (USHORT)(In->Gid >> 16);
            }
            Fcb->Inode.Ctime = UfxHostNow();
            Status = UfxWriteInode(&Vcb->Vol, Fcb->Ino, &Fcb->Inode);
            if (!NT_SUCCESS(Status)) {
                Fcb->Inode = Before;
            }
            Status = UdnEngineLeave(Vcb, Status);
            if (NT_SUCCESS(Status)) {
                KeQuerySystemTime(&Fcb->ChangeTime);
                Fcb->Attributes = UdnComputeAttributes(Vcb, &Fcb->Inode, Fcb->UfxName, Fcb->UfxNameLength,
                                                       Fcb->DosAttributes);
                UdnNotifyChange(Vcb, Fcb, FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SECURITY,
                                FILE_ACTION_MODIFIED);
            }
            break;
        }

        case UDN_FSCTL_READ_LINK: {
            ULONG Length = 0;
            PCHAR Text;
            if (UdnIsVfcb(Fcb) || (Fcb->Inode.Mode & UFX_S_IFMT) != UFX_S_IFLNK) {
                Status = STATUS_NOT_A_REPARSE_POINT;
                __leave;
            }
            Text = (PCHAR)ExAllocatePoolWithTag(PagedPool, Vcb->Vol.BlockSize + 1, UDN_TAG_BUFFER);
            if (Text == NULL) {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                __leave;
            }
            (VOID)ExAcquireResourceSharedLite(&Fcb->Resource, TRUE);
            FcbAcquired = TRUE;
            UdnEngineEnter(Vcb);
            Status = UdnEngineLeave(Vcb, UfxReadLink(&Vcb->Vol, Fcb->Ino, &Fcb->Inode, Text,
                                                     Vcb->Vol.BlockSize + 1, &Length));
            if (NT_SUCCESS(Status)) {
                if (OutLength < Length) {
                    Status = STATUS_BUFFER_OVERFLOW;
                    Length = OutLength;
                }
                RtlCopyMemory(Buffer, Text, Length);
                Irp->IoStatus.Information = Length;
            }
            ExFreePool(Text);
            break;
        }

        case UDN_FSCTL_CREATE_LINK: {
            PUDN_LINK_REQUEST Req = (PUDN_LINK_REQUEST)Buffer;
            UNICODE_STRING Name;
            UCHAR Bytes[UDN_MAX_NAME + 1];
            ULONG Length;
            PCHAR Target;
            ULONG NewIno;
            UFX_INODE NewInode;
            UFX_ENTRY *Found;

            if (!UdnIsDcb(Fcb)) {
                Status = STATUS_NOT_A_DIRECTORY;
                __leave;
            }
            if (InLength < FIELD_OFFSET(UDN_LINK_REQUEST, Name) ||
                (ULONG)FIELD_OFFSET(UDN_LINK_REQUEST, Name) + Req->NameLength + Req->TargetLength > InLength ||
                Req->NameLength == 0 || (Req->NameLength & 1) || Req->TargetLength == 0 ||
                Req->TargetLength >= Vcb->Vol.BlockSize) {
                Status = STATUS_INVALID_PARAMETER;
                __leave;
            }
            Status = UdnVerifyWritable(Ctx, Vcb);
            if (!NT_SUCCESS(Status)) {
                __leave;
            }
            Name.Buffer = Req->Name;
            Name.Length = Name.MaximumLength = Req->NameLength;
            if (!UdnIsValidUnicodeName(&Name) ||
                !NT_SUCCESS(UdnUnicodeToExt(&Name, Bytes, &Length)) || Length == 0) {
                Status = STATUS_OBJECT_NAME_INVALID;
                __leave;
            }
            Bytes[Length] = 0;

            (VOID)ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);
            FcbAcquired = TRUE;
            if (Fcb->FcbState & (FCB_STATE_DELETED | FCB_STATE_DELETE_PENDING)) {
                Status = STATUS_DELETE_PENDING;
                __leave;
            }

            Found = (UFX_ENTRY *)ExAllocatePoolWithTag(PagedPool, sizeof(UFX_ENTRY), UDN_TAG_BUFFER);
            if (Found == NULL) {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                __leave;
            }
            Status = UdnLookupName(Vcb, Fcb, &Name, UdnData.CaseSensitive, Found);
            ExFreePool(Found);
            if (NT_SUCCESS(Status)) {
                Status = STATUS_OBJECT_NAME_COLLISION;
                __leave;
            }
            if (Status != STATUS_OBJECT_NAME_NOT_FOUND) {
                __leave;
            }

            Target = (PCHAR)ExAllocatePoolWithTag(PagedPool, Req->TargetLength + 1, UDN_TAG_BUFFER);
            if (Target == NULL) {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                __leave;
            }
            RtlCopyMemory(Target, (PUCHAR)Req->Name + Req->NameLength, Req->TargetLength);
            Target[Req->TargetLength] = 0;

            UdnEngineEnter(Vcb);
            Status = UfxCreate(&Vcb->Vol, Fcb->Ino, &Fcb->Inode, (const char *)Bytes, Length,
                               (USHORT)(UFX_S_IFLNK | 0777), Target, &NewIno, &NewInode);
            if (NT_SUCCESS(Status) && (UdnData.DefaultUid || UdnData.DefaultGid)) {
                NewInode.UidLo = (USHORT)UdnData.DefaultUid;
                NewInode.UidHi = (USHORT)(UdnData.DefaultUid >> 16);
                NewInode.GidLo = (USHORT)UdnData.DefaultGid;
                NewInode.GidHi = (USHORT)(UdnData.DefaultGid >> 16);
                Status = UfxWriteInode(&Vcb->Vol, NewIno, &NewInode);
            }
            Status = UdnEngineLeave(Vcb, Status);
            ExFreePool(Target);

            if (NT_SUCCESS(Status)) {
                UdnIndexAdd(Fcb, Bytes, Length, NewIno);
                if (NT_SUCCESS(UdnBuildFullName(Fcb))) {
                    UNICODE_STRING Full;
                    USHORT Prefix = (USHORT)(Fcb->FullName.Length + (Fcb->ParentDcb != NULL ? sizeof(WCHAR) : 0));
                    Full.MaximumLength = (USHORT)(Prefix + Name.Length);
                    Full.Buffer = (PWSTR)ExAllocatePoolWithTag(PagedPool, Full.MaximumLength, UDN_TAG_NAME);
                    if (Full.Buffer != NULL) {
                        RtlCopyMemory(Full.Buffer, Fcb->FullName.Buffer, Fcb->FullName.Length);
                        if (Fcb->ParentDcb != NULL) {
                            Full.Buffer[Fcb->FullName.Length / sizeof(WCHAR)] = L'\\';
                        }
                        RtlCopyMemory((PUCHAR)Full.Buffer + Prefix, Name.Buffer, Name.Length);
                        Full.Length = Full.MaximumLength;
                        FsRtlNotifyFullReportChange(Vcb->NotifySync, &Vcb->DirNotifyList, (PSTRING)&Full,
                                                    Prefix, NULL, NULL, FILE_NOTIFY_CHANGE_FILE_NAME,
                                                    FILE_ACTION_ADDED, NULL);
                        ExFreePool(Full.Buffer);
                    }
                }
            }
            break;
        }

        default:
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
        }

    } __finally {

        if (FcbAcquired) {
            UdnRelease(&Fcb->Resource);
        }
        UdnRelease(&Vcb->Resource);
    }

    return Status;
}

NTSTATUS
UdnUserFsRequest (
    PUDN_IRP_CONTEXT Ctx
    )
{
    ULONG Code = UdnXSp(Ctx->IrpSp)->Parameters.FileSystemControl.FsControlCode;

    switch (Code) {

    case FSCTL_LOCK_VOLUME:
        return UdnLockVolume(Ctx);

    case FSCTL_UNLOCK_VOLUME:
        return UdnUnlockVolume(Ctx);

    case FSCTL_DISMOUNT_VOLUME:
        return UdnDismountVolume(Ctx);

    case FSCTL_IS_VOLUME_MOUNTED:
        return UdnVerifyVcb(Ctx, Ctx->Vcb);

    case FSCTL_IS_PATHNAME_VALID:
        return STATUS_SUCCESS;

    case FSCTL_IS_VOLUME_DIRTY:
        return UdnIsVolumeDirty(Ctx);

    case FSCTL_MARK_VOLUME_DIRTY:
        return UdnMarkVolumeDirtyRequest(Ctx);

    case UDN_FSCTL_QUERY_VOLUME:
    case UDN_FSCTL_GET_POSIX:
    case UDN_FSCTL_SET_POSIX:
    case UDN_FSCTL_READ_LINK:
    case UDN_FSCTL_CREATE_LINK:
    case UDN_FSCTL_COMMIT:
        return UdnPrivateRequest(Ctx, Code);

    case FSCTL_REQUEST_OPLOCK_LEVEL_1:
    case FSCTL_REQUEST_OPLOCK_LEVEL_2:
    case FSCTL_REQUEST_BATCH_OPLOCK:
    case FSCTL_REQUEST_FILTER_OPLOCK:
        return STATUS_OPLOCK_NOT_GRANTED;

    case FSCTL_OPLOCK_BREAK_ACKNOWLEDGE:
    case FSCTL_OPBATCH_ACK_CLOSE_PENDING:
    case FSCTL_OPLOCK_BREAK_NOTIFY:
    case FSCTL_OPLOCK_BREAK_ACK_NO_2:
        return STATUS_INVALID_OPLOCK_PROTOCOL;
    }

    return STATUS_INVALID_DEVICE_REQUEST;
}

NTSTATUS
UdnCommonFileSystemControl (
    PUDN_IRP_CONTEXT Ctx
    )
{
    switch (Ctx->IrpSp->MinorFunction) {

    case IRP_MN_MOUNT_VOLUME:
        if (Ctx->Vcb != NULL) {
            return STATUS_INVALID_DEVICE_REQUEST;
        }
        return UdnMountVolume(Ctx);

    case IRP_MN_VERIFY_VOLUME:
        return UdnVerifyVolume(Ctx);

    case IRP_MN_USER_FS_REQUEST:
#ifdef IRP_MN_KERNEL_CALL
    case IRP_MN_KERNEL_CALL:
#endif
        if (Ctx->Vcb == NULL) {
            return STATUS_INVALID_DEVICE_REQUEST;
        }
        return UdnUserFsRequest(Ctx);
    }

    return STATUS_INVALID_DEVICE_REQUEST;
}

/* ------------------------------------------------------------------ */
/* Plug and Play (Windows 2000)                                        */
/* ------------------------------------------------------------------ */

/*
 * Plug and Play (2000 and later): a removal of the volume's device asks the
 * mounted file system first. Nothing here needs more than NT 3.1's kernel, so
 * the one i386 binary has it too; DriverEntry wires IRP_MJ_PNP only where the
 * running kernel has it.
 */
static NTSTATUS
NTAPI
UdnPnpCompletion (
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

/* Sends the IRP down and waits; the caller completes it */
static NTSTATUS
UdnForwardAndWait (
    PUDN_IRP_CONTEXT Ctx,
    PUDN_VCB Vcb
    )
{
    PIRP Irp = Ctx->Irp;
    KEVENT Event;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    UdnCopyStackToNext(Irp);
    IoSetCompletionRoutine(Irp, UdnPnpCompletion, &Event, TRUE, TRUE, TRUE);

    if (IoCallDriver(Vcb->TargetDeviceObject, Irp) == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    }

    return Irp->IoStatus.Status;
}

NTSTATUS
UdnCommonPnp (
    PUDN_IRP_CONTEXT Ctx
    )
{
    PUDN_VCB Vcb = Ctx->Vcb;
    PIRP Irp = Ctx->Irp;
    BOOLEAN Delete = FALSE;
    NTSTATUS Status;
    ULONG Wait;

    switch (Ctx->IrpSp->MinorFunction) {

    case IRP_MN_QUERY_REMOVE_DEVICE:

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

        if (!(Vcb->VcbState & VCB_STATE_DISMOUNTED)) {
            (VOID)UdnFlushVolume(Vcb, TRUE);
            UdnPurgeCachedFiles(Vcb);
        }

        UDN_DBG((UDN_PFX "query remove %08lX: %lu open, state %lX\n", Vcb->SerialNumber, Vcb->UncleanCount, Vcb->VcbState));
        if (Vcb->UncleanCount != 0 || (Vcb->VcbState & VCB_STATE_LOCKED)) {
            UdnResumeWriting(Vcb);
            UdnRelease(&Vcb->Resource);
            return STATUS_ACCESS_DENIED;
        }

        Vcb->VcbState |= VCB_STATE_PNP_LOCKED;      /* no new opens from here */
        UdnRelease(&Vcb->Resource);

        /*
         * Every handle is closed, but the purged files' file objects close only
         * when the Cache and Memory Managers let go of them, a moment later (the
         * closes need the resource). Removal is refused while any is left open,
         * so give them up to five seconds.
         */
        for (Wait = 0; Wait < 100 && *(volatile ULONG *)&Vcb->OpenCount != 0; Wait++) {
            LARGE_INTEGER Delay;
            Delay.QuadPart = -500000;               /* 50 ms */
            (VOID)KeDelayExecutionThread(KernelMode, FALSE, &Delay);
        }
        UDN_DBG((UDN_PFX "query remove %08lX: %lu file objects left after %lu ms\n",
                 Vcb->SerialNumber, Vcb->OpenCount, Wait * 50));

        Status = UdnForwardAndWait(Ctx, Vcb);

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

        if (NT_SUCCESS(Status)) {
            Delete = UdnDismountVcb(Vcb);
        } else {
            Vcb->VcbState &= ~VCB_STATE_PNP_LOCKED;
            UdnResumeWriting(Vcb);
        }

        UdnRelease(&Vcb->Resource);
        break;

    case IRP_MN_REMOVE_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:

        Status = UdnForwardAndWait(Ctx, Vcb);

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
        Vcb->VcbState &= ~VCB_STATE_PNP_LOCKED;
        Vcb->VcbState |= VCB_STATE_REMOVED;
        Delete = UdnDismountVcb(Vcb);
        UdnRelease(&Vcb->Resource);
        break;

    case IRP_MN_CANCEL_REMOVE_DEVICE:

        Status = UdnForwardAndWait(Ctx, Vcb);

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
        Vcb->VcbState &= ~VCB_STATE_PNP_LOCKED;
        UdnResumeWriting(Vcb);
        UdnRelease(&Vcb->Resource);
        break;

    default:

        if (Vcb->VcbState & VCB_STATE_REMOVED) {
            return STATUS_NO_SUCH_DEVICE;
        }

        UdnSkipStack(Irp);
        Ctx->Flags |= UDN_CTX_NO_COMPLETE;
        return IoCallDriver(Vcb->TargetDeviceObject, Irp);
    }

    if (Delete) {
        UdnDeleteVcb(Vcb);
        Ctx->Vcb = NULL;
    }

    return Status;
}
