/*
 * UDFNT - IRP_MJ_QUERY_VOLUME_INFORMATION and IRP_MJ_SET_VOLUME_INFORMATION
 */

#include "udn.h"

static NTSTATUS
UdnCopyString (
    PWSTR Target,
    ULONG Room,
    const WCHAR *Source,
    ULONG Bytes,
    PULONG Copied
    )
{
    *Copied = (Bytes < Room) ? Bytes : Room;
    RtlCopyMemory(Target, Source, *Copied);

    return (*Copied < Bytes) ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

/* Blocks: the partition file data goes to, and what is free in it */
static VOID
UdnVolumeBlocks (
    PUDN_VCB Vcb,
    PULONGLONG Total,
    PULONGLONG Free,
    PULONGLONG Available
    )
{
    UdnEngineEnter(Vcb);
    UfxCountFree(&Vcb->Vol);
    *Total = Vcb->Vol.DataBlocks ? Vcb->Vol.DataBlocks : Vcb->Vol.BlocksCount;
    *Free = Vcb->Vol.FreeBlocksCount;
    UdnRelease(&Vcb->EngineResource);
    *Available = *Free;
}

/* the name Windows calls it by */
static const WCHAR *
UdnFsName (
    PUDN_VCB Vcb
    )
{
    UNREFERENCED_PARAMETER(Vcb);
    return L"UDF";
}

NTSTATUS
UdnCommonQueryVolumeInformation (
    PUDN_IRP_CONTEXT Ctx
    )
{
    PIRP Irp = Ctx->Irp;
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PUDN_VCB Vcb = Ctx->Vcb;
    ULONG Class = IrpSp->Parameters.QueryVolume.FsInformationClass;
    ULONG Length = IrpSp->Parameters.QueryVolume.Length;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    PFILE_FS_VOLUME_INFORMATION Volume;
    PFILE_FS_SIZE_INFORMATION Size;
    PFILE_FS_DEVICE_INFORMATION Device;
    PFILE_FS_ATTRIBUTE_INFORMATION Attribute;
    PUDN_FILE_FS_FULL_SIZE_INFORMATION FullSize;
    ULONG Base;
    ULONG Copied;
    ULONGLONG Total = 0, Free = 0, Available = 0;
    ULONG Used = 0;
    const WCHAR *Name;
    NTSTATUS Status = STATUS_SUCCESS;

    (VOID)ExAcquireResourceSharedLite(&Vcb->Resource, TRUE);

    __try {

        Status = UdnVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        switch (Class) {

        case FileFsVolumeInformation:
            Base = FIELD_OFFSET(FILE_FS_VOLUME_INFORMATION, VolumeLabel);
            if (Length < Base) {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }

            Volume = (PFILE_FS_VOLUME_INFORMATION)Buffer;
            RtlZeroMemory(Volume, Base);
            Volume->VolumeSerialNumber = Vcb->SerialNumber;
            Volume->VolumeLabelLength = Vcb->LabelLength * sizeof(WCHAR);
            Volume->SupportsObjects = FALSE;

            Status = UdnCopyString(Volume->VolumeLabel, Length - Base, Vcb->Label,
                                    Vcb->LabelLength * sizeof(WCHAR), &Copied);
            Used = Base + Copied;
            break;

        case FileFsSizeInformation:
            if (Length < sizeof(FILE_FS_SIZE_INFORMATION)) {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }
            UdnVolumeBlocks(Vcb, &Total, &Free, &Available);
            Size = (PFILE_FS_SIZE_INFORMATION)Buffer;
            Size->TotalAllocationUnits.QuadPart = (LONGLONG)Total;
            Size->AvailableAllocationUnits.QuadPart = (LONGLONG)Available;
            Size->SectorsPerAllocationUnit = Vcb->Vol.SectorsPerBlock;
            Size->BytesPerSector = UDN_SECTOR_SIZE;
            Used = sizeof(FILE_FS_SIZE_INFORMATION);
            break;

        case UDN_CLASS_FS_FULL_SIZE:
            if (Length < sizeof(UDN_FILE_FS_FULL_SIZE_INFORMATION)) {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }
            UdnVolumeBlocks(Vcb, &Total, &Free, &Available);
            FullSize = (PUDN_FILE_FS_FULL_SIZE_INFORMATION)Buffer;
            FullSize->TotalAllocationUnits.QuadPart = (LONGLONG)Total;
            FullSize->CallerAvailableAllocationUnits.QuadPart = (LONGLONG)Available;
            FullSize->ActualAvailableAllocationUnits.QuadPart = (LONGLONG)Free;
            FullSize->SectorsPerAllocationUnit = Vcb->Vol.SectorsPerBlock;
            FullSize->BytesPerSector = UDN_SECTOR_SIZE;
            Used = sizeof(UDN_FILE_FS_FULL_SIZE_INFORMATION);
            break;

        case FileFsDeviceInformation:
            if (Length < sizeof(FILE_FS_DEVICE_INFORMATION)) {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }
            Device = (PFILE_FS_DEVICE_INFORMATION)Buffer;
            Device->DeviceType = Vcb->Optical ? FILE_DEVICE_CD_ROM : FILE_DEVICE_DISK;
            Device->Characteristics = Vcb->Vpb->RealDevice->Characteristics;
            Used = sizeof(FILE_FS_DEVICE_INFORMATION);
            break;

        case FileFsAttributeInformation:
            Base = FIELD_OFFSET(FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName);
            if (Length < Base) {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }

            Attribute = (PFILE_FS_ATTRIBUTE_INFORMATION)Buffer;
            Attribute->FileSystemAttributes = FILE_CASE_SENSITIVE_SEARCH | FILE_CASE_PRESERVED_NAMES |
                                              FILE_UNICODE_ON_DISK;
            if (Vcb->VcbState & VCB_STATE_READ_ONLY) {
                Attribute->FileSystemAttributes |= FILE_READ_ONLY_VOLUME;
            }
            Attribute->MaximumComponentNameLength = UDN_MAX_NAME;
            Name = UdnFsName(Vcb);
            Attribute->FileSystemNameLength = 5 * sizeof(WCHAR);

            Status = UdnCopyString(Attribute->FileSystemName, Length - Base, Name,
                                    5 * sizeof(WCHAR), &Copied);
            Used = Base + Copied;
            break;

        default:
            Status = STATUS_INVALID_PARAMETER;
            break;
        }

        if (NT_SUCCESS(Status) || Status == STATUS_BUFFER_OVERFLOW) {
            Irp->IoStatus.Information = Used;
        }

    } __finally {

        UdnRelease(&Vcb->Resource);
    }

    return Status;
}

/* Some NT4 headers lack FILE_FS_LABEL_INFORMATION */
typedef struct _UDN_FS_LABEL {
    ULONG       VolumeLabelLength;
    WCHAR       VolumeLabel[1];
} UDN_FS_LABEL, *PUDN_FS_LABEL;

NTSTATUS
UdnCommonSetVolumeInformation (
    PUDN_IRP_CONTEXT Ctx
    )
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PUDN_VCB Vcb = Ctx->Vcb;
    PUDN_FS_LABEL Label = (PUDN_FS_LABEL)Ctx->Irp->AssociatedIrp.SystemBuffer;
    /* NT4 ntddk.h has no SetVolume member; QueryVolume has the same layout */
    ULONG Length = IrpSp->Parameters.QueryVolume.Length;
    UCHAR Bytes[UDN_MAX_NAME + 1];
    ULONG ByteLength = 0;
    UNICODE_STRING U;
    ULONG Count;
    ULONG i;
    NTSTATUS Status;

    if (IrpSp->Parameters.QueryVolume.FsInformationClass != FileFsLabelInformation) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Length < FIELD_OFFSET(UDN_FS_LABEL, VolumeLabel) ||
        Label->VolumeLabelLength > Length - FIELD_OFFSET(UDN_FS_LABEL, VolumeLabel) ||
        (Label->VolumeLabelLength & 1) != 0 ||
        Label->VolumeLabelLength > UDN_MAX_LABEL * sizeof(WCHAR)) {

        return STATUS_INVALID_VOLUME_LABEL;
    }

    Count = Label->VolumeLabelLength / sizeof(WCHAR);

    for (i = 0; i < Count; i++) {
        if (Label->VolumeLabel[i] < 0x20) {
            return STATUS_INVALID_VOLUME_LABEL;
        }
    }

    /* the UDF label: up to 126 Latin-1 or 63 other characters (NT allows 32) */
    U.Buffer = Label->VolumeLabel;
    U.Length = U.MaximumLength = (USHORT)(Count * sizeof(WCHAR));
    if (Count != 0 && (!NT_SUCCESS(UdnUnicodeToExt(&U, Bytes, &ByteLength)) || ByteLength > 255)) {
        return STATUS_INVALID_VOLUME_LABEL;
    }
    Bytes[ByteLength] = 0;

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Status = UdnVerifyWritable(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        UdnEngineEnter(Vcb);
        Status = UdnEngineLeave(Vcb, UfxSetLabel(&Vcb->Vol, (const char *)Bytes));
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        UdnLoadLabel(Vcb);

    } __finally {

        UdnRelease(&Vcb->Resource);
    }

    return Status;
}
