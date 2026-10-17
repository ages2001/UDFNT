/* UDFNT - the FAT16 boot view of a UDF volume (see udfshim.c) */
#ifndef _UDFSHIM_H_
#define _UDFSHIM_H_

#include "udfeng.h"

#define UDF_SHIM_OEM            "UDFNTBT1"      /* bytes 3-10 of sector 0 */
#define UDF_SHIM_WINDIR         0x3F0           /* the Windows directory, 16 bytes, in sector 1 */
#define UDF_SHIM_CODE_SIZE      1024
#define UDF_SHIM_AREA_NAME      "UDFBOOT.SYS"   /* the boot area: a hidden system file in the root */

/* What the view is built through: the mounted, writable volume */
typedef struct _UDF_SHIM_HOST {
    PUDF_VOL    Vol;
    PVOID       Ctx;
    /* optional: a commit between files (default UdfCommit); may a file's data move; it moved */
    UDF_STATUS  (*Commit)(PVOID Ctx);
    BOOLEAN     (*CanMove)(PVOID Ctx, const UDF_LOC *Loc);
    VOID        (*Moved)(PVOID Ctx, const UDF_LOC *Loc);
    /* for a new boot sector: the BPB's geometry */
    ULONG       PartitionStart, SectorsPerTrack, Heads;
} UDF_SHIM_HOST, *PUDF_SHIM_HOST;

typedef struct _UDF_SHIM_RESULT {
    const char *Error;                  /* why it failed (NULL: it did not) */
    UCHAR       ErrorName[12];          /* the file it is about, if any */
    ULONG       Files, Dirs;
    ULONG       MovedIn;                /* boot files moved into reach */
    ULONG       MovedOut;               /* other files moved out of the way */
    ULONG       ClusterBytes;
    char        WinDir[16];
} UDF_SHIM_RESULT, *PUDF_SHIM_RESULT;

/* The blocks of the boot area for a block size (udffmt /BOOT keeps them free) */
ULONG UdfShimAreaBlocks(ULONG BlockSize);

/* Creates the boot area file (UDFBOOT.SYS) low on the volume, if there is none */
UDF_STATUS UdfShimReserve(PUDF_SHIM_HOST Host, PUDF_SHIM_RESULT Result);

/*
 * Builds the boot view (Code: the 1024-byte boot loader, written with its
 * BPB) or brings it up to date (Code NULL: the boot sector stays). WinDir:
 * the Windows directory, or NULL: the one recorded in the boot sector, else
 * BOOT.INI's default entry.
 */
UDF_STATUS UdfShimUpdate(PUDF_SHIM_HOST Host, const char *WinDir, const UCHAR *Code, PUDF_SHIM_RESULT Result);

/* TRUE if sector 0 (512 bytes) is the boot sector of a view */
BOOLEAN UdfShimPresent(const UCHAR *Sector0);

/* The window of a view (from its boot sector): data partition blocks [Lo, Hi) kept for the boot files */
BOOLEAN UdfShimWindow(PUDF_VOL Vol, const UCHAR *Sector0, PULONG Lo, PULONG Hi);

#endif
