/*
 * UDFNT - udffmt: formats a volume (or an image file) as UDF.
 *
 *   udffmt E: [options]
 *   udffmt \\.\PhysicalDriveN /P:n [options]    partition n (1-4) of a disk
 *   udffmt IMAGE [options]                       an image file or device
 *
 * The partition type becomes 07h (installable file system), unless /T says
 * otherwise. The new volume is read back through the UDFNT engine before the
 * tool reports success.
 */
#include "udfeng.h"
#include "udfimg.h"
#include "udfmkfs.h"
#include "udfshim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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
    GetSystemTime(&st);                 /* (GetSystemTimeAsFileTime is not in NT 3.1) */
    SystemTimeToFileTime(&st, &ft);
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return 116444736000000000ULL + (ULONGLONG)tv.tv_sec * 10000000ULL + (ULONGLONG)tv.tv_usec * 10;
#endif
}

static int MkWrite(void *Ctx, unsigned long long Off, const void *Buf, unsigned Len) { (void)Ctx; return UimgWrite(&Img, Off, Buf, Len); }

static unsigned
ParseSize(const char *S)
{
    unsigned long v = strtoul(S, NULL, 10);
    const char *p = S;
    while (*p >= '0' && *p <= '9') p++;
    if (*p == 'k' || *p == 'K') v <<= 10;
    return (unsigned)v;
}

static unsigned
ParseRev(const char *S)
{
    static const struct { const char *N; unsigned R; } t[] = {
        { "1.02", 0x102 }, { "1.50", 0x150 }, { "1.5", 0x150 }, { "2.00", 0x200 }, { "2.0", 0x200 }, { "2", 0x200 },
        { "2.01", 0x201 }, { "2.50", 0x250 }, { "2.5", 0x250 }, { "2.60", 0x260 }, { "2.6", 0x260 }
    };
    unsigned i;
    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) if (!strcmp(S, t[i].N)) return t[i].R;
    return 0;
}

static int
Usage(void)
{
    printf("Formats a volume as UDF (Universal Disk Format).\n\n"
           "udffmt drive: [options]\n"
           "udffmt \\\\.\\PhysicalDriveN /P:n [options]\n"
           "udffmt image-file [options]\n\n"
           "  /R:rev         UDF revision: 1.02, 1.50, 2.00, 2.01 (default), 2.50, 2.60\n"
           "  /B:size        block size: 512 (default: the sector size) .. 64K\n"
           "  /L:label       volume label (up to 126 Latin-1 or 63 other characters)\n"
           "  /META          a metadata partition (default for 2.50 and 2.60)\n"
           "  /NOMETA        no metadata partition\n"
           "  /SPARE         a sparable partition (rewritable discs; 1.50 - 2.01)\n"
           "  /FE            file entries instead of extended file entries (2.00+)\n"
           "  /BOOT          a boot area at the start of the volume (see udfboot)\n"
           "  /T:xx          the partition type to set (hex; default 07)\n"
           "  /P:n           partition n (1-4) of a disk or disk image\n"
           "  /Y             do not ask for confirmation\n"
           "  /Q             quiet\n");
    return 2;
}

int
main(int argc, char **argv)
{
    UDF_MKFS M;
    const char *Target = NULL;
    int Yes = 0, Quiet = 0, Slot = -1, I, R, Meta = -1, Boot = 0;
    unsigned Type = 0x07;
    UDF_VOL *V;
    UDF_STATUS st;

    memset(&M, 0, sizeof(M));
    M.Revision = 0x201;
    strcpy(M.Label, "UDF Volume");
    M.Mode = 0755;
    M.Uid = M.Gid = 0xFFFFFFFF;
    for (I = 1; I < argc; I++) {
        const char *A = argv[I];
        if ((A[0] == '/' || A[0] == '-') && A[1] && (A[2] == 0 || A[2] == ':' || !_stricmp(A + 1, "META") || !_stricmp(A + 1, "NOMETA") ||
                                                  !_stricmp(A + 1, "SPARE") || !_stricmp(A + 1, "FE") || !_stricmp(A + 1, "BOOT"))) {
            const char *V2 = strchr(A, ':');
            char C = (char)((A[1] >= 'a' && A[1] <= 'z') ? A[1] - 32 : A[1]);
            if (V2) V2++;
            if (!_stricmp(A + 1, "META")) Meta = 1;
            else if (!_stricmp(A + 1, "NOMETA")) Meta = 0;
            else if (!_stricmp(A + 1, "SPARE")) M.Sparable = 1;
            else if (!_stricmp(A + 1, "FE")) M.UseFe = 1;
            else if (!_stricmp(A + 1, "BOOT")) Boot = 1;
            else if (!V2 && C == 'Y' && !A[2]) Yes = 1;
            else if (!V2 && C == 'Q' && !A[2]) Quiet = 1;
            else if (V2 && C == 'L' && A[2] == ':') {
                if (strlen(V2) > 126) { printf("The label is too long.\n"); return 2; }
                strcpy(M.Label, V2);
            }
            else if (V2 && C == 'R' && A[2] == ':') { M.Revision = ParseRev(V2); if (!M.Revision) return Usage(); }
            else if (V2 && C == 'B' && A[2] == ':') M.BlockSize = ParseSize(V2);
            else if (V2 && C == 'T' && A[2] == ':') Type = (unsigned)strtoul(V2, NULL, 16);
            else if (V2 && C == 'P' && A[2] == ':' && atoi(V2) >= 1 && atoi(V2) <= 4) Slot = atoi(V2) - 1;
            else return Usage();
        } else if (Target == NULL) {
            Target = A;
        } else {
            return Usage();
        }
    }
    if (Target == NULL) return Usage();
    M.Metadata = Meta >= 0 ? Meta : (M.Revision >= 0x250 && !M.Sparable);
#ifndef _WIN32
    if (Target[0] && Target[1] == ':' && Target[2] == 0) { printf("Drives can be formatted on Windows only.\n"); return 1; }
#endif
    if (!UimgOpen(&Img, Target, Slot, 0)) {
        if (Slot >= 0) printf("Cannot open partition %d of %s.\n", Slot + 1, Target);
        else printf("Cannot open %s.\n", Target);
        return 1;
    }
    if (!M.BlockSize) M.BlockSize = Img.Sector;
    if (M.BlockSize < Img.Sector) { printf("The block size cannot be smaller than the sector size (%u).\n", Img.Sector); return 1; }
    if (Boot) {
        if (Img.Sector != 512 || M.Sparable) { printf("/BOOT needs 512-byte sectors and no sparable partition.\n"); return 1; }
        M.BootBlocks = UdfShimAreaBlocks(M.BlockSize);
    }
    M.Bytes = Img.Size;
    M.NowNt = UdfHostNowNt();
    M.Ctx = &Img;
    M.Write = MkWrite;

    if (!Yes) {
        char Answer[8];
        printf("WARNING: ALL DATA ON %s%s WILL BE LOST!\nProceed with format (Y/N)? ",
               Target, Img.Slot >= 0 ? " (the partition)" : "");
        fflush(stdout);
#ifdef _WIN32
        { DWORD N; if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), Answer, sizeof(Answer), &N, NULL) || N == 0) Answer[0] = 'n'; }
#else
        if (!fgets(Answer, sizeof(Answer), stdin)) Answer[0] = 'n';
#endif
        if (Answer[0] != 'y' && Answer[0] != 'Y') { printf("\nFormat cancelled.\n"); UimgClose(&Img); return 1; }
    }
    if (!UimgLock(&Img)) {
        printf("Cannot lock drive %s: it is in use (open files, a running program,\n"
               "the system or paging file). Close them and try again.\n", Target);
        UimgClose(&Img);
        return 1;
    }
    if (!Quiet) {
        printf("Formatting %llu MB as UDF %x.%02x (block %u%s%s)...\n", (unsigned long long)(M.Bytes >> 20),
               M.Revision >> 8, M.Revision & 0xFF, M.BlockSize, M.Metadata ? ", metadata partition" : "",
               M.Sparable ? ", sparable partition" : "");
    }
    if (!UdfMakeFs(&M)) {
        printf("Format failed: %s.\n", M.Error ? M.Error : "?");
        UimgUnlock(&Img); UimgClose(&Img);
        return 1;
    }

    /* read it back through the engine */
    V = (UDF_VOL *)calloc(1, sizeof(UDF_VOL));
    R = 0;
    if (V) {
        V->DeviceBytes = Img.Size;
        V->SectorSize = Img.Sector;
        st = UdfMount(V, !Boot);
        R = UDF_SUCCESS(st) && V->BlockSize == M.BlockSize && V->Revision == M.Revision;
        if (R && Boot) {
            /* the boot area, in the blocks kept free for it */
            UDF_SHIM_HOST H;
            UDF_SHIM_RESULT Res;
            memset(&H, 0, sizeof(H));
            H.Vol = V;
            st = UdfShimReserve(&H, &Res);
            if (UDF_SUCCESS(st)) st = UdfCommit(V);
            if (!UDF_SUCCESS(st)) {
                printf("Format failed: the boot area could not be made (%s).\n", Res.Error ? Res.Error : "error");
                R = 0;
            }
        }
        UdfUnmount(V);
        free(V);
    }
    if (!R) {
        printf("Format failed: the new file system does not read back.\n");
        UimgUnlock(&Img); UimgClose(&Img);
        return 1;
    }
    if (Type) {
        R = UimgSetType(&Img, (unsigned char)Type);
        if (R == 0) printf("Note: the partition type could not be set to %02Xh.\n", Type);
        else if (R == 1 && !Quiet) printf("Partition type set to %02Xh.\n", Type);
    }
    UimgUnlock(&Img);
    UimgClose(&Img);
    if (!Quiet) printf("Format complete.\n");
    return 0;
}
