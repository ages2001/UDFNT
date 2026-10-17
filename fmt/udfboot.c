/*
 * UDFNT - udfboot: makes a UDF volume start Windows NT.
 *
 *   udfboot E: [/S:dir] [/Q]
 *   udfboot \\.\PhysicalDriveN /P:n [/S:dir] [/Q]
 *   udfboot IMAGE [/P:n] [/S:dir] [/Q]
 *
 * The volume should have been formatted with udffmt /BOOT (else udfboot
 * makes room for the boot area at the start of the partition), and hold
 * NTLDR, NTDETECT.COM and BOOT.INI in its root and the Windows directory
 * (with UDFNT installed to start at boot: udfinst /BOOT). udfboot writes the
 * boot loader and the FAT16 view NTLDR reads (see src/udfshim.c); on a disk
 * image it also marks the partition active.
 *
 * Run it again after changing NTLDR, the kernel, the HAL or a boot driver
 * from a system that has no UDFNT, or after Linux changed them; UDFNT
 * itself keeps the view current.
 */
#include "udfeng.h"
#include "udfimg.h"
#include "udfshim.h"
#include "udfbootbin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#include <sys/time.h>
#define _stricmp strcasecmp
#endif

static UDF_IMAGE Img;

UDF_STATUS UdfHostRead(PUDF_VOL V, ULONGLONG Off, ULONG Len, PVOID Buf) { (void)V; return UimgRead(&Img, Off, Buf, Len) ? UDF_OK : UDF_IO_ERROR; }
UDF_STATUS UdfHostWrite(PUDF_VOL V, ULONGLONG Off, ULONG Len, const VOID *Buf) { (void)V; return UimgWrite(&Img, Off, Buf, Len) ? UDF_OK : UDF_IO_ERROR; }
UDF_STATUS UdfHostFlush(PUDF_VOL V) { (void)V; return UDF_OK; }
PVOID UdfHostAlloc(ULONG Bytes) { return malloc(Bytes ? Bytes : 1); }
VOID UdfHostFree(PVOID P) { free(P); }
ULONGLONG
UdfHostNowNt(VOID)
{
#ifdef _WIN32
    FILETIME ft; SYSTEMTIME st;
    GetSystemTime(&st);
    SystemTimeToFileTime(&st, &ft);
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return 116444736000000000ULL + (ULONGLONG)tv.tv_sec * 10000000ULL + (ULONGLONG)tv.tv_usec * 10;
#endif
}

static int
Usage(void)
{
    printf("Makes a UDF volume start Windows NT.\n\n"
           "udfboot drive: [/S:dir] [/Q]\n"
           "udfboot \\\\.\\PhysicalDriveN /P:n [/S:dir] [/Q]\n"
           "udfboot image-file [/P:n] [/S:dir] [/Q]\n\n"
           "  /S:dir   the Windows directory (default: from BOOT.INI)\n"
           "  /P:n     partition n (1-4) of a disk or disk image\n"
           "  /T:xx    also set the partition type (hex)\n"
           "  /Q       quiet\n\n"
           "The volume should be formatted with udffmt /BOOT and hold NTLDR, NTDETECT.COM,\n"
           "BOOT.INI and Windows NT with UDFNT installed by udfinst /BOOT.\n");
    return 2;
}

int
main(int argc, char **argv)
{
    const char *Target = NULL, *SysRoot = NULL;
    int I, Slot = -1, Quiet = 0, Rc = 1;
    unsigned Type = 0;
    UDF_VOL *V = NULL;
    UDF_SHIM_HOST H;
    UDF_SHIM_RESULT R;
    UDF_STATUS St;
    unsigned long Start, Spt, Heads;

    for (I = 1; I < argc; I++) {
        const char *A = argv[I];
        if ((A[0] == '/' || A[0] == '-') && A[1] && (!A[2] || A[2] == ':')) {
            char C = (char)((A[1] >= 'a' && A[1] <= 'z') ? A[1] - 32 : A[1]);
            if (!_stricmp(A + 1, "Q")) Quiet = 1;
            else if (C == 'S' && A[2] == ':') SysRoot = A + 3;
            else if (C == 'P' && A[2] == ':' && atoi(A + 3) >= 1 && atoi(A + 3) <= 4) Slot = atoi(A + 3) - 1;
            else if (C == 'T' && A[2] == ':') Type = (unsigned)strtoul(A + 3, NULL, 16);
            else return Usage();
        } else if (Target == NULL) {
            Target = A;
        } else {
            return Usage();
        }
    }
    if (Target == NULL) return Usage();
#ifndef _WIN32
    if (Target[0] && Target[1] == ':' && Target[2] == 0) { printf("Drives can be prepared on Windows only.\n"); return 1; }
#endif
    if (!UimgOpen(&Img, Target, Slot, 0)) {
        if (Slot >= 0) printf("Cannot open partition %d of %s.\n", Slot + 1, Target);
        else printf("Cannot open %s.\n", Target);
        return 1;
    }
    if (!UimgLock(&Img)) {
        printf("Cannot lock drive %s: it is in use (open files, a running program,\n"
               "the system or paging file). Close them and try again.\n", Target);
        UimgClose(&Img);
        return 1;
    }
    if (!Quiet) printf("Preparing %s to start Windows NT...\n", Target);
    V = (UDF_VOL *)calloc(1, sizeof(UDF_VOL));
    if (!V) { printf("Out of memory.\n"); goto done; }
    V->DeviceBytes = Img.Size;
    V->SectorSize = Img.Sector;
    St = UdfMount(V, FALSE);
    if (!UDF_SUCCESS(St)) {
        printf("udfboot failed: this is not a UDF volume (%08lX).\n", (unsigned long)St);
        goto done;
    }
    if (V->ReadOnly) {
        printf("udfboot failed: UDFNT cannot write this volume (%s).\n", V->ReadOnlyWhy ? V->ReadOnlyWhy : "read-only");
        UdfUnmount(V);
        goto done;
    }
    memset(&H, 0, sizeof(H));
    H.Vol = V;
    UimgGeometry(&Img, &Start, &Spt, &Heads);
    H.PartitionStart = Start;
    H.SectorsPerTrack = Spt;
    H.Heads = Heads;
    St = UdfShimReserve(&H, &R);
    if (UDF_SUCCESS(St)) St = UdfShimUpdate(&H, SysRoot, UdfBootCode, &R);
    if (!UDF_SUCCESS(UdfCommit(V)) && UDF_SUCCESS(St)) { St = UDF_IO_ERROR; R.Error = "write error"; }
    UdfUnmount(V);
    if (!UDF_SUCCESS(St)) {
        printf("udfboot failed: %s%s%.11s%s\n", R.Error ? R.Error : "error",
               R.ErrorName[0] ? " (" : "", (const char *)R.ErrorName, R.ErrorName[0] ? ")" : "");
        goto done;
    }
    if (!Quiet) {
        if (R.MovedOut) printf("  %lu files moved further out to make room\n", (unsigned long)R.MovedOut);
        printf("  boot view: %lu files in %lu directories (Windows in \\%s), %lu moved near the start, %lu KB clusters\n",
               (unsigned long)R.Files, (unsigned long)R.Dirs, R.WinDir, (unsigned long)R.MovedIn,
               (unsigned long)(R.ClusterBytes >> 10));
    }
    Rc = 0;
    if (Type) {
        int T = UimgSetType(&Img, (unsigned char)Type);
        if (T == 0) printf("Note: the partition type could not be set to %02Xh.\n", Type);
    }
    if (UimgSetActive(&Img) == 0) printf("Note: the partition could not be marked active.\n");
#ifdef _WIN32
    if (Img.IsDrive && !Quiet) printf("Mark the partition active (Disk Administrator) if it is to start the computer.\n");
#endif
    if (!Quiet) printf("Done. Linux mounts the volume with: mount -t udf\n");
done:
    free(V);
    UimgUnlock(&Img);
    UimgClose(&Img);
    return Rc;
}
