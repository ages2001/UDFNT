/*
 * UDFNT - udfinst: installs or removes the UDFNT file system driver.
 *
 *   udfinst                Install: copy udfnt.sys (from the folder udfinst.exe
 *                          is in) to System32\drivers and create the Udfnt
 *                          service (system start, group "File system").
 *   udfinst /BOOT          Boot start, for running Windows NT from a UDF
 *                          volume (the volume is then prepared with udfboot).
 *   udfinst /READONLY      Mount UDF volumes read-only.
 *   udfinst /NOLETTERS     Do not give UDF partitions of other types drive letters.
 *   udfinst /NOOPTICAL     Do not mount UDF discs in CD / DVD / BD drives.
 *   udfinst /CASE          Case-sensitive names (exact case only).
 *   udfinst /HIDEDOT       Hide names starting with a dot.
 *   udfinst /UID:n /GID:n  Owner of the files UDFNT creates (default: none).
 *   udfinst /U             Uninstall: disable the service, remove the driver,
 *                          give Windows' own UDF driver back.
 *
 * Windows 2000 and XP have a UDF reader of their own (udfs.sys, loaded on
 * demand by the file system recognizer); it would take UDF volumes before
 * UDFNT, so udfinst turns it off (its ImagePath is kept as UdfntSavedImage)
 * in every control set, the Last Known Good one too.
 *
 * A restart applies the change. Runs on Windows NT 3.1 and later; on 64-bit
 * Windows use the 64-bit udfinst and udfnt.sys.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SVC_UDFNT           "SYSTEM\\CurrentControlSet\\Services\\Udfnt"
#define SVC_UDFS            "\\Services\\Udfs"

#define SERVICE_BOOT_START      0
#define SERVICE_SYSTEM_START    1
#define SERVICE_DISABLED        4
#define SERVICE_FILE_SYSTEM     2

static LONG SetDword(HKEY Key, const char *Name, DWORD Value)
{
    return RegSetValueExA(Key, Name, 0, REG_DWORD, (const BYTE *)&Value, sizeof(Value));
}

static LONG SetSz(HKEY Key, const char *Name, const char *Value)
{
    return RegSetValueExA(Key, Name, 0, REG_SZ, (const BYTE *)Value, (DWORD)(strlen(Value) + 1));
}

/*
 * Make "UDFNT" a known load-order group placed last, by appending it to
 * HKLM\SYSTEM\CurrentControlSet\Control\ServiceGroupOrder\List (a REG_MULTI_SZ).
 * The boot loader loads boot-start drivers group by group in this order, and
 * NT 3.1's loader will not load a boot-start driver whose group is not listed.
 * Placing UDFNT last means udfnt registers after the FAT driver and so is
 * offered - and claims - the UDF boot volume before FAT recognizes its view.
 */
static void AddGroupOrder(void)
{
    HKEY Key;
    DWORD Type = 0, Size = 0;
    char *List, *P;
    LONG R;

    R = RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SYSTEM\\CurrentControlSet\\Control\\ServiceGroupOrder",
                      0, KEY_READ | KEY_WRITE, &Key);
    if (R != ERROR_SUCCESS) {
        return;
    }
    RegQueryValueExA(Key, "List", NULL, &Type, NULL, &Size);
    if (Type != REG_MULTI_SZ || Size == 0) {
        Size = 0;
    }
    List = (char *)malloc(Size + 32);
    if (List == NULL) {
        RegCloseKey(Key);
        return;
    }
    if (Size) {
        RegQueryValueExA(Key, "List", NULL, &Type, (BYTE *)List, &Size);
        if (Size >= 2 && List[Size - 1] == 0 && List[Size - 2] == 0) {
            Size--;     /* drop one of the two terminators; we re-add it */
        }
    } else {
        Size = 1; List[0] = 0;
    }
    for (P = List; *P; P += strlen(P) + 1) {
        if (_stricmp(P, "UDFNT") == 0) {    /* already present */
            RegCloseKey(Key);
            free(List);
            return;
        }
    }
    strcpy(List + Size, "UDFNT");
    Size += (DWORD)strlen("UDFNT") + 1;
    List[Size++] = 0;                       /* final terminator */
    RegSetValueExA(Key, "List", 0, REG_MULTI_SZ, (const BYTE *)List, Size);
    RegCloseKey(Key);
    free(List);
}

/* TRUE if the file is UDFNT's driver (it carries this string) */
static int IsUdfnt(const char *Path)
{
    static const char Mark[] = "UDFNT file system driver";
    HANDLE H;
    DWORD Size, Got, I, N = sizeof(Mark) - 1;
    char *Buf;
    int Found = 0;

    H = CreateFileA(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (H == INVALID_HANDLE_VALUE) return 0;
    Size = GetFileSize(H, NULL);
    if (Size != 0xFFFFFFFF && Size < 4 * 1024 * 1024) {
        Buf = (char *)HeapAlloc(GetProcessHeap(), 0, Size + 1);
        if (Buf != NULL) {
            if (ReadFile(H, Buf, Size, &Got, NULL) && Got == Size) {
                for (I = 0; I + N <= Size && !Found; I++) {
                    if (Buf[I] == Mark[0] && memcmp(Buf + I, Mark, N) == 0) Found = 1;
                }
            }
            HeapFree(GetProcessHeap(), 0, Buf);
        }
    }
    CloseHandle(H);
    return Found;
}

static void DriversPath(char *Out)
{
    char Dir[MAX_PATH];
    UINT N = GetWindowsDirectoryA(Dir, sizeof(Dir));
    if (N == 0 || N >= sizeof(Dir)) {
        Out[0] = 0;
        return;
    }
    sprintf(Out, "%s\\System32\\drivers\\udfnt.sys", Dir);
}

/* udfnt.sys next to this program, else in the current folder */
static void SourcePath(char *Out)
{
    char *Slash;
    DWORD N = GetModuleFileNameA(NULL, Out, MAX_PATH);
    if (N == 0 || N >= MAX_PATH) {
        strcpy(Out, "udfnt.sys");
        return;
    }
    Slash = strrchr(Out, '\\');
    if (Slash == NULL) {
        strcpy(Out, "udfnt.sys");
        return;
    }
    strcpy(Slash + 1, "udfnt.sys");
    if (GetFileAttributesA(Out) == 0xFFFFFFFF) strcpy(Out, "udfnt.sys");
}

typedef struct {
    int Boot, ReadOnly, NoLetters, Case, HideDot, NoOptical;
    long Uid, Gid;
} OPTIONS;

/* Windows' own UDF driver (2000 and later) in one control set: 1 if changed */
static int UdfsIn(const char *Set, int Enable)
{
    HKEY Key;
    char Path[MAX_PATH], Image[MAX_PATH];
    DWORD Type, Size = sizeof(Image);
    int Changed = 0;
    sprintf(Path, "SYSTEM\\%s%s", Set, SVC_UDFS);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, Path, 0, KEY_ALL_ACCESS, &Key) != ERROR_SUCCESS) {
        return 0;               /* NT 3.x / 4.0: there is none */
    }
    if (!Enable) {
        if (RegQueryValueExA(Key, "UdfntSavedImage", NULL, &Type, (BYTE *)Image, &Size) != ERROR_SUCCESS) {
            Size = sizeof(Image);
            if (RegQueryValueExA(Key, "ImagePath", NULL, &Type, (BYTE *)Image, &Size) != ERROR_SUCCESS) {
                strcpy(Image, "System32\\DRIVERS\\udfs.sys");
            }
            SetSz(Key, "UdfntSavedImage", Image);
        }
        /* the recognizer loads it even when disabled: its load fails, and the
           volume goes to the next file system, UDFNT */
        SetSz(Key, "ImagePath", "System32\\DRIVERS\\udfs.sys.disabled-by-udfnt");
        SetDword(Key, "Start", SERVICE_DISABLED);
        Changed = 1;
    } else {
        if (RegQueryValueExA(Key, "UdfntSavedImage", NULL, &Type, (BYTE *)Image, &Size) == ERROR_SUCCESS) {
            SetSz(Key, "ImagePath", Image);
            RegDeleteValueA(Key, "UdfntSavedImage");
            SetDword(Key, "Start", SERVICE_DISABLED);
            Changed = 1;
        }
    }
    RegFlushKey(Key);
    RegCloseKey(Key);
    return Changed;
}

/* ... in CurrentControlSet and every ControlSetNNN (Last Known Good among them) */
static void WindowsUdfs(int Enable)
{
    HKEY Sys;
    char Name[64];
    DWORD I, Len;
    int Changed = UdfsIn("CurrentControlSet", Enable);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM", 0, KEY_READ, &Sys) == ERROR_SUCCESS) {
        for (I = 0; Len = sizeof(Name), RegEnumKeyExA(Sys, I, Name, &Len, NULL, NULL, NULL, NULL) == ERROR_SUCCESS; I++) {
            if (_strnicmp(Name, "ControlSet", 10) == 0 && Name[10] >= '0' && Name[10] <= '9') {
                Changed |= UdfsIn(Name, Enable);
            }
        }
        RegCloseKey(Sys);
    }
    if (Changed) {
        printf(Enable ? "  Windows' own UDF driver (udfs.sys) is back\n"
                      : "  Windows' own UDF driver (udfs.sys) is turned off\n");
    }
}

static int Install(OPTIONS *O)
{
    char Sys[MAX_PATH], Src[MAX_PATH];
    HKEY Key;
    DWORD Disp, Start;
    LONG R;

    DriversPath(Sys);
    SourcePath(Src);
    if (Sys[0] == 0) {
        printf("  cannot find the system directory.\n");
        return 1;
    }
    if (!IsUdfnt(Src)) {
        printf("  error: %s is not UDFNT's driver.\n", Src);
        printf("  put udfnt.sys next to udfinst.exe and run it again.\n");
        return 1;
    }
    if (!CopyFileA(Src, Sys, FALSE)) {
        printf("  error: could not copy %s to %s (error %lu).\n", Src, Sys, GetLastError());
        return 1;
    }
    printf("  installed %s\n", Sys);

    R = RegCreateKeyExA(HKEY_LOCAL_MACHINE, SVC_UDFNT, 0, NULL, REG_OPTION_NON_VOLATILE,
                        KEY_ALL_ACCESS, NULL, &Key, &Disp);
    if (R != ERROR_SUCCESS) {
        printf("  error: cannot create the Udfnt service key (error %ld).\n", R);
        return 1;
    }

    Start = O->Boot ? SERVICE_BOOT_START : SERVICE_SYSTEM_START;
    SetDword(Key, "Type", SERVICE_FILE_SYSTEM);
    SetDword(Key, "ErrorControl", 1);
    SetSz(Key, "ImagePath", "System32\\DRIVERS\\udfnt.sys");
    SetDword(Key, "Start", Start);
    SetSz(Key, "Group", Start == SERVICE_BOOT_START ? "UDFNT" : "File system");
    SetDword(Key, "EnableWriteSupport", O->ReadOnly ? 0 : 1);
    SetDword(Key, "AssignDriveLetters", O->NoLetters ? 0 : 1);
    SetDword(Key, "CaseSensitive", O->Case ? 1 : 0);
    SetDword(Key, "HideDotFiles", O->HideDot ? 1 : 0);
    SetDword(Key, "MountOptical", O->NoOptical ? 0 : 1);
    if (O->Uid >= 0) SetDword(Key, "DefaultUid", (DWORD)O->Uid);
    if (O->Gid >= 0) SetDword(Key, "DefaultGid", (DWORD)O->Gid);
    RegFlushKey(Key);
    RegCloseKey(Key);

    if (Start == SERVICE_BOOT_START) {
        AddGroupOrder();
    }
    WindowsUdfs(0);

    printf("  Udfnt service: %s start, %s, %s names%s\n",
           Start == SERVICE_BOOT_START ? "boot" : "system",
           O->ReadOnly ? "read-only" : "read/write",
           O->Case ? "case-sensitive" : "case-preserving",
           O->NoLetters ? ", no drive letters" : "");
    printf("Done. Restart to load UDFNT.\n");
    return 0;
}

static int Uninstall(void)
{
    char Sys[MAX_PATH];
    HKEY Key;

    DriversPath(Sys);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, SVC_UDFNT, 0, KEY_ALL_ACCESS, &Key) == ERROR_SUCCESS) {
        SetDword(Key, "Start", SERVICE_DISABLED);
        RegFlushKey(Key);
        RegCloseKey(Key);
        printf("  disabled the Udfnt service\n");
    }
    WindowsUdfs(1);
    /* the running driver keeps its file until the restart; remove it then */
    if (Sys[0] && IsUdfnt(Sys) && !DeleteFileA(Sys)) {
        printf("  %s is in use; it stays until it is removed by hand.\n", Sys);
    }
    printf("Done. Restart to apply.\n");
    return 0;
}

/* A 32-bit installer on 64-bit Windows would write into SysWOW64 */
static int UnderWow64(void)
{
    typedef BOOL (WINAPI *ISWOW64)(HANDLE, PBOOL);
    HMODULE K = GetModuleHandleA("kernel32.dll");
    ISWOW64 F = K ? (ISWOW64)GetProcAddress(K, "IsWow64Process") : NULL;
    BOOL W = FALSE;
    return F != NULL && F(GetCurrentProcess(), &W) && W;
}

static int Usage(void)
{
    printf("usage: udfinst [/BOOT] [/READONLY] [/NOLETTERS] [/NOOPTICAL] [/CASE] [/HIDEDOT] [/UID:n] [/GID:n]\n"
           "       udfinst /U\n");
    return 2;
}

int main(int argc, char **argv)
{
    OPTIONS O;
    int I, Remove = 0;

    memset(&O, 0, sizeof(O));
    O.Uid = O.Gid = -1;
    for (I = 1; I < argc; I++) {
        const char *A = argv[I];
        if (A[0] != '/' && A[0] != '-') return Usage();
        A++;
        if (!_stricmp(A, "U")) Remove = 1;
        else if (!_stricmp(A, "BOOT")) O.Boot = 1;
        else if (!_stricmp(A, "READONLY")) O.ReadOnly = 1;
        else if (!_stricmp(A, "NOLETTERS")) O.NoLetters = 1;
        else if (!_stricmp(A, "CASE")) O.Case = 1;
        else if (!_stricmp(A, "HIDEDOT")) O.HideDot = 1;
        else if (!_stricmp(A, "NOOPTICAL")) O.NoOptical = 1;
        else if (!_strnicmp(A, "UID:", 4)) O.Uid = atol(A + 4);
        else if (!_strnicmp(A, "GID:", 4)) O.Gid = atol(A + 4);
        else return Usage();
    }

    printf("UDFNT installer\n");
    if (UnderWow64()) {
        printf("  this is the 32-bit installer: on 64-bit Windows run amd64\\udfinst.exe\n");
        return 1;
    }
    return Remove ? Uninstall() : Install(&O);
}
