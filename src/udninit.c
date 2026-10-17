/*
 * UDFNT - driver entry and settings
 *
 * Settings live in the service key (HKLM\System\CurrentControlSet\
 * Services\Udfnt) and are read at every mount, so a change applies to
 * volumes mounted afterwards:
 *
 *   EnableWriteSupport (REG_DWORD)  1 or missing: read/write, 0: read-only
 *   HideDotFiles       (REG_DWORD)  1: ".name" is hidden (0 or missing: not)
 *   MountOptical       (REG_DWORD)  1 or missing: UDF discs in CD / DVD / BD drives too
 *   FollowSymlinks     (REG_DWORD)  1 or missing: opens follow symbolic links
 *   CaseSensitive      (REG_DWORD)  0 or missing: names are found in any case
 *                                   (an exact match first), 1: exact only
 *   DefaultUid, DefaultGid (REG_DWORD)  owner of new files (0 = root)
 *   FileMode, DirMode  (REG_DWORD)  permissions of new files (0644) and
 *                                   directories (0755)
 */

#include "udn.h"

UDN_DATA UdnData;

/* extinst recognises this driver by this string */
#ifdef __GNUC__
__attribute__((used))
#endif
const char UdnSignature[] = "UDFNT file system driver";

#ifdef UDN_CROSS
PIRP UdnXTopLevelIrp;
#endif

/* Keeps a NUL-terminated copy of the service key path */
static VOID
UdnSaveRegistryPath (
    PUNICODE_STRING RegistryPath
    )
{
    USHORT Size;

    if (RegistryPath == NULL || RegistryPath->Length == 0) {
        return;
    }

    Size = (USHORT)(RegistryPath->Length + sizeof(WCHAR));
    UdnData.RegistryPath.Buffer = (PWCHAR)ExAllocatePoolWithTag(PagedPool, Size, UDN_TAG_REGISTRY);

    if (UdnData.RegistryPath.Buffer != NULL) {
        RtlCopyMemory(UdnData.RegistryPath.Buffer, RegistryPath->Buffer, RegistryPath->Length);
        UdnData.RegistryPath.Buffer[RegistryPath->Length / sizeof(WCHAR)] = 0;
        UdnData.RegistryPath.Length = RegistryPath->Length;
        UdnData.RegistryPath.MaximumLength = Size;
    }
}

static ULONG
UdnQueryDword (
    PCWSTR Name,
    ULONG Default
    )
{
    RTL_QUERY_REGISTRY_TABLE Table[2];
    ULONG Value = Default;

    if (UdnData.RegistryPath.Buffer == NULL) {
        return Default;
    }

    RtlZeroMemory(Table, sizeof(Table));
    Table[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
    Table[0].Name = (PWSTR)Name;
    Table[0].EntryContext = &Value;
    Table[0].DefaultType = REG_DWORD;
    Table[0].DefaultData = &Default;
    Table[0].DefaultLength = sizeof(ULONG);

    if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, UdnData.RegistryPath.Buffer,
                                           Table, NULL, NULL))) {
        return Default;
    }
    return Value;
}

/* Reads the settings; called at every mount */
static VOID
UdnLoadSettings (
    VOID
    )
{
    UdnData.Writable = (BOOLEAN)(UdnQueryDword(L"EnableWriteSupport", 1) != 0);
    UdnData.HideDotFiles = (BOOLEAN)(UdnQueryDword(L"HideDotFiles", 0) != 0);
    UdnData.MountOptical = (BOOLEAN)(UdnQueryDword(L"MountOptical", 1) != 0);
    UdnData.FollowLinks = (BOOLEAN)(UdnQueryDword(L"FollowSymlinks", 1) != 0);
    UdnData.CaseSensitive = (BOOLEAN)(UdnQueryDword(L"CaseSensitive", 0) != 0);
    UdnData.DefaultUid = UdnQueryDword(L"DefaultUid", 0);
    UdnData.DefaultGid = UdnQueryDword(L"DefaultGid", 0);
    UdnData.FileMode = UdnQueryDword(L"FileMode", 0644) & 07777;
    UdnData.DirMode = UdnQueryDword(L"DirMode", 0755) & 07777;
}

/* Settings again (a mount is starting); FALSE: mount read-only */
BOOLEAN
UdnWriteSupportEnabled (
    VOID
    )
{
    UdnLoadSettings();
    return UdnData.Writable;
}

#ifdef UDN_SEHTEST
/* Self-test of UdnTryCall (debug builds): two raises, both must be caught */
static VOID UdnRaiseBody(PVOID Context) { ExRaiseStatus(*(PNTSTATUS)Context); }
static VOID UdnNestedBody(PVOID Context) { (VOID)UdnTryCall(UdnRaiseBody, Context, TRUE); ExRaiseStatus(STATUS_DISK_FULL); }
static VOID
UdnSehSelfTest (
    VOID
    )
{
    NTSTATUS Code = STATUS_INVALID_USER_BUFFER, R;
    R = UdnTryCall(UdnRaiseBody, &Code, FALSE);
    DBGP("[BTN sehtest 1 "); DBGX(R); DBGP("]\n");
    Code = STATUS_IN_PAGE_ERROR;
    R = UdnTryCall(UdnNestedBody, &Code, TRUE);
    DBGP("[BTN sehtest 2 "); DBGX(R); DBGP("]\n");
    R = UdnSafeCopy(&Code, (PVOID)(ULONG_PTR)16, sizeof(Code));
    DBGP("[BTN sehtest 3 "); DBGX(R); DBGP("]\n");
}
#endif

NTSTATUS
NTAPI
DriverEntry (
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath
    )
{
    UNICODE_STRING Name;
    PDEVICE_OBJECT DeviceObject;
    NTSTATUS Status;
    ULONG i;

#ifdef UDN_NT31
    /* Bind the executive-resource API to the running kernel (Lite on NT 3.51+,
     * legacy on NT 3.1) before any resource is initialised. */
    if (!UdnResolveResourceApi()) {
        DBGP("[UDFNT:resource API not found]\n");
        return STATUS_PROCEDURE_NOT_FOUND;
    }
#endif

    RtlZeroMemory(&UdnData, sizeof(UdnData));

#ifdef UDN_SEHTEST
    UdnSehSelfTest();
#endif

    UdnSaveRegistryPath(RegistryPath);
    UdnLoadSettings();

    RtlInitUnicodeString(&Name, UDN_FS_DEVICE_NAME);

    Status = IoCreateDevice(DriverObject,
                            0,
                            &Name,
                            FILE_DEVICE_DISK_FILE_SYSTEM,
                            0,
                            FALSE,
                            &DeviceObject);

    if (!NT_SUCCESS(Status)) {
        UDN_DBG((UDN_PFX "IoCreateDevice failed %lX\n", Status));
        return Status;
    }

    UdnData.DriverObject = DriverObject;
    UdnData.FileSystemDeviceObject = DeviceObject;

    ExInitializeResourceLite(&UdnData.Resource);
    InitializeListHead(&UdnData.VcbList);

    /*
     * Wire the dispatch table. The DRIVER_OBJECT.MajorFunction[] array is
     * sized IRP_MJ_MAXIMUM_FUNCTION+1 by the running kernel, and that count
     * grew over time: NT 3.1 - 3.51 stop well below the value the modern
     * (WDK / mingw) headers define for IRP_MJ_MAXIMUM_FUNCTION (0x1b), so a
     * loop to the header's maximum writes past the end of the kernel's array
     * and corrupts the adjacent pool block. Only the low majors exist on NT
     * 3.1 anyway (no Power/WMI/PnP), so cap at the highest one UDFNT actually
     * serves - IRP_MJ_CLEANUP (0x12), which every NT since 3.1 provides.
     */
#ifdef UDN_NT31
    for (i = 0; i <= IRP_MJ_CLEANUP; i++) {
        DriverObject->MajorFunction[i] = UdnFsdDispatch;
    }
    /*
     * Plug and Play (2000 and later), or "Safely Remove Hardware" is vetoed:
     * the I/O manager sets Size to the running kernel's sizeof(DRIVER_OBJECT),
     * whose last member is the MajorFunction array, so its length is known.
     */
    if (DriverObject->Size > FIELD_OFFSET(DRIVER_OBJECT, MajorFunction) &&
        (ULONG)(DriverObject->Size - FIELD_OFFSET(DRIVER_OBJECT, MajorFunction)) / sizeof(PDRIVER_DISPATCH) > IRP_MJ_PNP) {
        DriverObject->MajorFunction[IRP_MJ_PNP] = UdnFsdDispatch;
    }
#else
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        DriverObject->MajorFunction[i] = UdnFsdDispatch;
    }
#endif

    UdnInitializeFastIo(&UdnData.FastIoDispatch);
    DriverObject->FastIoDispatch = &UdnData.FastIoDispatch;

    UdnInitializeCacheCallbacks();

#ifdef DO_DEVICE_INITIALIZING
    DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
#endif

    /* Volumes are left clean at shutdown */
    (VOID)IoRegisterShutdownNotification(DeviceObject);

    IoRegisterFileSystem(DeviceObject);

    /* UDF discs: a second file system device for CD / DVD / BD drives */
    if (UdnData.MountOptical) {
        UNICODE_STRING CdName;
        PDEVICE_OBJECT Cd;
        RtlInitUnicodeString(&CdName, UDN_CDFS_DEVICE_NAME);
        if (NT_SUCCESS(IoCreateDevice(DriverObject, 0, &CdName, FILE_DEVICE_CD_ROM_FILE_SYSTEM, 0, FALSE, &Cd))) {
#ifdef DO_DEVICE_INITIALIZING
            Cd->Flags &= ~DO_DEVICE_INITIALIZING;
#endif
            UdnData.CdromDeviceObject = Cd;
            IoRegisterFileSystem(Cd);
        }
    }

    /* Linux partitions get no drive letter from Windows: give them one */
    UdnStartDriveLetters();

    UDN_DBG((UDN_PFX "Loaded\n"));

    return STATUS_SUCCESS;
}
