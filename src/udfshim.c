/*
 * UDFNT - the FAT16 view that lets Windows NT start from a UDF volume
 *
 * NTLDR reads only FAT and NTFS (and on 3.x HPFS). So the files it needs are
 * also given a FAT16 view, kept inside the UDF volume:
 *
 *   sectors 0-1     the UDFNT boot loader with a FAT16 BPB (UDF leaves the
 *                   first 32 KB of a volume unused)
 *   boot area       the blocks of UDFBOOT.SYS, a hidden system file in the
 *                   root that udffmt /BOOT (or udfboot) places low on the
 *                   disk: the FAT, the root directory, then the clusters of
 *                   the FAT subdirectories
 *   clusters        4 KB (or one block, if blocks are larger; at most 32 KB):
 *                   cluster 2 begins right after the root directory, and a
 *                   FAT file's clusters are the very blocks of the UDF file,
 *                   so the view always shows the current contents
 *
 * The view holds NTLDR, NTDETECT.COM, BOOT.INI (and the other loader files
 * of the root), and from the Windows directory what the loader reads: the
 * kernel and HAL, the SYSTEM hive, the NLS tables and fonts, and every
 * driver in SYSTEM32\DRIVERS. Files lying outside the 65518 clusters the
 * FAT can reach, or not cut on the cluster grid, are moved inside, cluster
 * by cluster (the last cluster's tail stays the file's, recorded as
 * allocated past its end). Clusters the view does not use are marked bad.
 * While a volume has a view, UDFNT allocates in the window last, so the
 * boot files can grow in place.
 */

#include "udfshim.h"

#define FAT_SECTORS     256             /* 65526 entries of 2 bytes */
#define ROOT_SECTORS    16              /* 256 entries */
#define DIR_BYTES       (128 * 1024)    /* the subdirectories' clusters */
#define MAX_CLUSTERS    65524
#define WIN_CLUSTERS    65518

typedef struct _SFILE {
    UCHAR       Name[11];
    UCHAR       Case;               /* NT case flags */
    UCHAR       Attr;
    UDF_LOC     Loc;
    ULONG       Size;
    ULONGLONG   Mtime;
    int         Dir;                /* subdirectory index, or -1 */
    ULONG       First;              /* first cluster */
} SFILE;

typedef struct _SDIR {
    UDF_LOC     Loc;
    int         Parent;
    SFILE      *F;
    ULONG       Count, Cap;
    ULONG       First;              /* first cluster (0 = the root) */
    ULONG       Clusters;
} SDIR;

#define SHIM_MAX_DIRS   16

typedef struct _SHIM {
    PUDF_SHIM_HOST H;
    PUDF_VOL    Vol;
    PUDF_SHIM_RESULT R;
    USHORT      Part;               /* the data partition */
    ULONG       P;                  /* its first block on the volume */
    ULONG       PartLen;
    ULONG       B, Spb;             /* block size, sectors a block */
    ULONG       C, Spc;             /* cluster size, sectors a cluster */
    ULONG       Align;              /* blocks of a piece (a cluster, or one block) */
    ULONG       A, M, K;            /* boot area: first block, blocks, FAT + root blocks */
    ULONG       Fs;                 /* FAT sectors */
    ULONG       Base, WinEnd;       /* the block of cluster 2, the end of the window */
    ULONG       DataSec;            /* the sector of cluster 2 */
    ULONG       Clusters;           /* clusters the FAT describes */
    ULONG       DirClusters;
    UDF_LOC     AreaLoc;
    SDIR        D[SHIM_MAX_DIRS];
    ULONG       NDirs;
    USHORT     *Fat;
} SHIM;

/* ------------------------------------------------------------------ */
/* Small helpers (no C library: this file is built into the driver)    */
/* ------------------------------------------------------------------ */

static PVOID
ShAlloc(ULONG Bytes)
{
    PVOID P = UdfHostAlloc(Bytes ? Bytes : 1);
    if (P) UdfMemZero(P, Bytes);
    return P;
}

static VOID
ShFree(PVOID P)
{
    if (P) UdfHostFree(P);
}

static BOOLEAN
ShGrow(PVOID *Arr, PULONG Cap, ULONG Need, ULONG Elem)
{
    ULONG N;
    PVOID X;
    if (Need <= *Cap) return TRUE;
    N = *Cap ? *Cap : 16;
    while (N < Need) N *= 2;
    X = ShAlloc(N * Elem);
    if (!X) return FALSE;
    if (*Arr) { UdfMemCopy(X, *Arr, *Cap * Elem); UdfHostFree(*Arr); }
    *Arr = X;
    *Cap = N;
    return TRUE;
}

static VOID
ShFill(PVOID P, int C, ULONG N)
{
    PUCHAR B = (PUCHAR)P;
    while (N--) *B++ = (UCHAR)C;
}

static ULONG
ShLen(const char *S)
{
    ULONG N = 0;
    while (S[N]) N++;
    return N;
}

static const char *
ShChr(const char *S, char C)
{
    for (; *S; S++) if (*S == C) return S;
    return 0;
}

static BOOLEAN
SameLoc(const UDF_LOC *A, const UDF_LOC *B)
{
    return (BOOLEAN)(A->Lbn == B->Lbn && A->Part == B->Part);
}

static int
Fail(SHIM *S, const char *Msg)
{
    if (!S->R->Error) S->R->Error = Msg;
    return -1;
}

static VOID
FailName(SHIM *S, const char *Msg, const UCHAR *Name)
{
    if (S->R->Error) return;
    S->R->Error = Msg;
    UdfMemCopy(S->R->ErrorName, Name, 11);
    S->R->ErrorName[11] = 0;
}

/* Volume sectors (512 bytes), in pieces */
static int
ShRead(SHIM *S, ULONGLONG Sector, ULONG Count, PVOID Buf)
{
    PUCHAR P = (PUCHAR)Buf;
    while (Count) {
        ULONG n = Count > 64 ? 64 : Count;
        if (!UDF_SUCCESS(UdfHostRead(S->Vol, Sector << 9, n << 9, P))) return -1;
        Sector += n; Count -= n; P += n * 512;
    }
    return 0;
}

static int
ShWrite(SHIM *S, ULONGLONG Sector, ULONG Count, const VOID *Buf)
{
    const UCHAR *P = (const UCHAR *)Buf;
    while (Count) {
        ULONG n = Count > 64 ? 64 : Count;
        if (!UDF_SUCCESS(UdfHostWrite(S->Vol, Sector << 9, n << 9, P))) return -1;
        Sector += n; Count -= n; P += n * 512;
    }
    return 0;
}

static VOID
ShCommit(SHIM *S)
{
    if (S->H->Commit) (VOID)S->H->Commit(S->H->Ctx);
    else (VOID)UdfCommit(S->Vol);
}

BOOLEAN
UdfShimPresent(const UCHAR *Sector0)
{
    return (BOOLEAN)(!UdfMemCmp(Sector0 + 3, UDF_SHIM_OEM, 8) && Sector0[510] == 0x55 && Sector0[511] == 0xAA);
}

/* The cluster size for a block size: 4 KB, or one block, at most 32 KB */
static ULONG
ClusterBytes(ULONG B)
{
    return B < 4096 ? 4096 : B > 32768 ? 32768 : B;
}

ULONG
UdfShimAreaBlocks(ULONG BlockSize)
{
    ULONG Spb = BlockSize / 512, K = (FAT_SECTORS + ROOT_SECTORS + Spb - 1) / Spb;
    ULONG Bytes = 16 * ClusterBytes(BlockSize), Dir;
    if (Bytes < DIR_BYTES) Bytes = DIR_BYTES;          /* at least 16 directory clusters */
    Dir = Bytes / BlockSize;
    return K + (Dir ? Dir : 1);
}

BOOLEAN
UdfShimWindow(PUDF_VOL Vol, const UCHAR *S0, PULONG Lo, PULONG Hi)
{
    ULONG Spb, Spc, Res, Spf, Base, P, End;
    if (!UdfShimPresent(S0) || Vol->SectorSize != 512 || Vol->NMaps == 0) return FALSE;
    if (Vol->Map[Vol->DataPart].Kind != UDF_KIND_PHYS) return FALSE;
    Spb = Vol->BlockSize / 512;
    Spc = S0[0x0D];
    Res = S0[0x0E] | (S0[0x0F] << 8);
    Spf = S0[0x16] | (S0[0x17] << 8);
    if (!Spc || !Spb) return FALSE;
    P = Vol->Map[Vol->DataPart].Start;
    Base = (Res + Spf + ROOT_SECTORS) / Spb;
    if (Base < P) return FALSE;
    Base -= P;
    End = Base + (ULONG)(((ULONGLONG)WIN_CLUSTERS * Spc) / Spb);
    if (End > Vol->Map[Vol->DataPart].Length) End = Vol->Map[Vol->DataPart].Length;
    *Lo = Base;
    *Hi = End;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Names                                                               */
/* ------------------------------------------------------------------ */

/* A name as a FAT 8.3 name (upper case, space padded); FALSE if it is not one */
static BOOLEAN
To83(const char *Name, ULONG Len, UCHAR *Out, UCHAR *Case)
{
    static const char Ok[] = "$%'-_@~`!(){}^#&";
    ULONG i, Dot = Len, n;
    BOOLEAN BaseLower = FALSE, BaseUpper = FALSE, ExtLower = FALSE, ExtUpper = FALSE;
    ShFill(Out, ' ', 11);
    *Case = 0;
    if (Len == 0 || Name[0] == '.') return FALSE;
    for (i = 0; i < Len; i++) if (Name[i] == '.') { if (Dot != Len) return FALSE; Dot = i; }
    if (Dot == 0 || Dot > 8 || (Dot < Len && Len - Dot - 1 > 3) || Dot == Len - 1) return FALSE;
    for (i = 0, n = 0; i < Len; i++) {
        char C = Name[i];
        BOOLEAN InExt = (BOOLEAN)(i > Dot);
        if (i == Dot) continue;
        if (C >= 'a' && C <= 'z') { if (InExt) ExtLower = TRUE; else BaseLower = TRUE; C = (char)(C - 32); }
        else if (C >= 'A' && C <= 'Z') { if (InExt) ExtUpper = TRUE; else BaseUpper = TRUE; }
        else if (!(C >= '0' && C <= '9') && !ShChr(Ok, C)) return FALSE;
        Out[InExt ? 8 + (i - Dot - 1) : n++] = (UCHAR)C;
    }
    if (BaseLower && !BaseUpper) *Case |= 0x08;
    if (ExtLower && !ExtUpper) *Case |= 0x10;
    return TRUE;
}

static BOOLEAN
Match(const char *Name, ULONG Len, const char *Want)
{
    ULONG i;
    if (ShLen(Want) != Len) return FALSE;
    for (i = 0; i < Len; i++) {
        char a = Name[i], b = Want[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return FALSE;
    }
    return TRUE;
}

/* Wildcard match of an 8.3 pattern ("HAL*.DLL") against a name, no case */
static BOOLEAN
Pattern(const char *Name, ULONG Len, const char *Pat)
{
    if (*Pat == 0) return (BOOLEAN)(Len == 0);
    if (*Pat == '*') {
        ULONG i;
        for (i = 0; i <= Len; i++) if (Pattern(Name + i, Len - i, Pat + 1)) return TRUE;
        return FALSE;
    }
    if (Len == 0) return FALSE;
    {
        char a = *Name, b = *Pat;
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b && b != '?') return FALSE;
    }
    return Pattern(Name + 1, Len - 1, Pat + 1);
}

/* Finds Name (any case) in directory Dir; FALSE if it is not there (or not of the kind wanted) */
static BOOLEAN
Find(SHIM *S, const UDF_LOC *Dir, const char *Name, BOOLEAN WantDir, PUDF_LOC Out, PUDF_DIRENT EntOut)
{
    PUDF_INODE D;
    PUDF_DIRENT E;
    ULONGLONG Cookie = 0;
    BOOLEAN Found = FALSE;
    if (!UDF_SUCCESS(UdfIget(S->Vol, Dir, &D))) return FALSE;
    E = (PUDF_DIRENT)ShAlloc(sizeof(UDF_DIRENT));
    if (!E) { UdfIput(S->Vol, D); return FALSE; }
    while (UDF_SUCCESS(UdfDirNext(S->Vol, D, Cookie, E))) {
        Cookie = E->Offset + E->FidLen;
        if (E->Chars & UDF_FID_PARENT) continue;
        if (Match(E->Name, E->NameLen, Name)) {
            if (((E->Chars & UDF_FID_DIRECTORY) != 0) == WantDir) {
                *Out = E->Icb;
                if (EntOut) UdfMemCopy(EntOut, E, sizeof(UDF_DIRENT));
                Found = TRUE;
            }
            break;
        }
    }
    ShFree(E);
    UdfIput(S->Vol, D);
    return Found;
}

static int
AddEntry(SHIM *S, int Dir, const char *Name, ULONG NameLen, const UDF_LOC *Loc, int SubDir)
{
    SDIR *D = &S->D[Dir];
    SFILE *F;
    PUDF_INODE I;
    ULONG k;
    UCHAR N83[11], Case;
    if (!To83(Name, NameLen, N83, &Case)) return 0;
    for (k = 0; k < D->Count; k++) if (!UdfMemCmp(D->F[k].Name, N83, 11)) return 0;   /* two names, one 8.3 */
    if (!UDF_SUCCESS(UdfIget(S->Vol, Loc, &I))) return 0;
    if (SubDir < 0 && (I->Size >= 0xFFFFFFFFULL || I->FileType != UDF_FT_FILE)) { UdfIput(S->Vol, I); return 0; }
    if (!ShGrow((PVOID *)&D->F, &D->Cap, D->Count + 1, sizeof(SFILE))) { UdfIput(S->Vol, I); return -1; }
    F = &D->F[D->Count++];
    ShFill(F, 0, sizeof(*F));
    UdfMemCopy(F->Name, N83, 11);
    F->Case = Case;
    F->Loc = *Loc;
    F->Dir = SubDir;
    F->Attr = (UCHAR)(SubDir >= 0 ? 0x10 : 0x20);
    F->Size = SubDir >= 0 ? 0 : (ULONG)I->Size;
    F->Mtime = I->Mtime.Nt;
    UdfIput(S->Vol, I);
    return 1;
}

static int
NewDir(SHIM *S, int Parent, const char *Name, const UDF_LOC *Loc)
{
    int Idx;
    if (S->NDirs == sizeof(S->D) / sizeof(S->D[0])) return -1;
    Idx = (int)S->NDirs++;
    ShFill(&S->D[Idx], 0, sizeof(SDIR));
    S->D[Idx].Loc = *Loc;
    S->D[Idx].Parent = Parent;
    if (Parent >= 0 && AddEntry(S, Parent, Name, ShLen(Name), Loc, Idx) <= 0) return -1;
    return Idx;
}

/* Every file of directory Loc whose name matches one of the patterns (NULL = all) */
static int
AddFiles(SHIM *S, int Dir, const UDF_LOC *Loc, const char *const *Pats)
{
    PUDF_INODE D;
    PUDF_DIRENT E;
    ULONGLONG Cookie = 0;
    int Rc = 0;
    if (!UDF_SUCCESS(UdfIget(S->Vol, Loc, &D))) return -1;
    E = (PUDF_DIRENT)ShAlloc(sizeof(UDF_DIRENT));
    if (!E) { UdfIput(S->Vol, D); return -1; }
    while (Rc == 0 && UDF_SUCCESS(UdfDirNext(S->Vol, D, Cookie, E))) {
        int k, Take = (Pats == NULL);
        Cookie = E->Offset + E->FidLen;
        if (E->Chars & (UDF_FID_PARENT | UDF_FID_DIRECTORY)) continue;
        if (E->Name[0] == '.') continue;
        for (k = 0; Pats && Pats[k] && !Take; k++) if (Pattern(E->Name, E->NameLen, Pats[k])) Take = 1;
        if (!Take) continue;
        if (AddEntry(S, Dir, E->Name, E->NameLen, &E->Icb, -1) < 0) Rc = -1;
    }
    ShFree(E);
    UdfIput(S->Vol, D);
    return Rc;
}

static BOOLEAN
IsBootFile(SHIM *S, const UDF_LOC *Loc)
{
    ULONG d, k;
    if (SameLoc(Loc, &S->AreaLoc)) return TRUE;
    for (d = 0; d < S->NDirs; d++) {
        if (SameLoc(&S->D[d].Loc, Loc)) return TRUE;
        for (k = 0; k < S->D[d].Count; k++) if (SameLoc(&S->D[d].F[k].Loc, Loc)) return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Space in the window                                                 */
/* ------------------------------------------------------------------ */

/* Free aligned pieces (Align blocks from AlignBase) inside [Lo, Hi) */
static ULONG
FreePieces(SHIM *S, ULONG Lo, ULONG Hi, ULONG Align, ULONG AlignBase)
{
    ULONG Lbn = Lo, Len, N = 0;
    while (Lbn < Hi) {
        if (!UDF_SUCCESS(UdfRunOf(S->Vol, S->Part, Lbn, Hi - Lbn, FALSE, &Len))) break;
        Lbn += Len;
        if (Lbn >= Hi) break;
        if (!UDF_SUCCESS(UdfRunOf(S->Vol, S->Part, Lbn, Hi - Lbn, TRUE, &Len)) || Len == 0) break;
        {
            ULONG First = AlignBase + (Lbn - AlignBase + Align - 1) / Align * Align;
            if (First < Lbn + Len) N += (Lbn + Len - First) / Align;
        }
        Lbn += Len;
    }
    return N;
}

/* Pieces a file needs moved to sit in the window */
static ULONG
PiecesToMove(SHIM *S, const UDF_LOC *Loc)
{
    PUDF_INODE I;
    ULONG N = 0;
    if (!UDF_SUCCESS(UdfIget(S->Vol, Loc, &I))) return 0;
    if (!UdfIplaced(S->Vol, I, S->Base, S->WinEnd, S->Align, S->Base, TRUE)) {
        ULONGLONG Blocks = (I->Size + S->B - 1) / S->B;
        N = (ULONG)((Blocks + S->Align - 1) / S->Align);
    }
    UdfIput(S->Vol, I);
    return N;
}

/*
 * Moves files other than the boot files out of [Lo, Hi) until Need pieces
 * (Align blocks from AlignBase) are free in it, walking the whole tree.
 */
static int
MakeRoom(SHIM *S, ULONG Lo, ULONG Hi, ULONG Align, ULONG AlignBase, ULONG Need)
{
    PUDF_LOC Stack = NULL;
    ULONG NStack = 0, CStack = 0, Free;
    PUDF_DIRENT E;
    int Rc = 0;

    Free = FreePieces(S, Lo, Hi, Align, AlignBase);
    if (Free >= Need) return 0;
    E = (PUDF_DIRENT)ShAlloc(sizeof(UDF_DIRENT));
    if (!E || !ShGrow((PVOID *)&Stack, &CStack, 1, sizeof(UDF_LOC))) { ShFree(E); return -1; }
    Stack[NStack++] = S->Vol->Root;
    while (NStack && Free < Need) {
        UDF_LOC DirLoc = Stack[--NStack];
        PUDF_INODE D;
        ULONGLONG Cookie = 0;
        if (!UDF_SUCCESS(UdfIget(S->Vol, &DirLoc, &D))) continue;
        while (Free < Need && UDF_SUCCESS(UdfDirNext(S->Vol, D, Cookie, E))) {
            PUDF_INODE I;
            ULONG Moved = 0;
            Cookie = E->Offset + E->FidLen;
            if (E->Chars & UDF_FID_PARENT) continue;
            if (E->Chars & UDF_FID_DIRECTORY) {
                if (NStack < 4096 && ShGrow((PVOID *)&Stack, &CStack, NStack + 1, sizeof(UDF_LOC))) Stack[NStack++] = E->Icb;
                continue;
            }
            if (IsBootFile(S, &E->Icb)) continue;
            if (S->H->CanMove && !S->H->CanMove(S->H->Ctx, &E->Icb)) continue;
            if (!UDF_SUCCESS(UdfIget(S->Vol, &E->Icb, &I))) continue;
            if (I->FileType == UDF_FT_FILE || I->FileType == UDF_FT_SYMLINK) {
                if (!UdfIplaced(S->Vol, I, Lo, Hi, 1, 0, FALSE) &&
                    UDF_SUCCESS(UdfIrelocate(S->Vol, I, Lo, Hi, 1, 0, FALSE, &Moved)) && Moved) {
                    S->R->MovedOut++;
                }
            }
            UdfIput(S->Vol, I);
            if (Moved) {
                if (S->H->Moved) S->H->Moved(S->H->Ctx, &E->Icb);
                ShCommit(S);
                Free = FreePieces(S, Lo, Hi, Align, AlignBase);
            }
        }
        UdfIput(S->Vol, D);
    }
    ShFree(Stack);
    ShFree(E);
    Free = FreePieces(S, Lo, Hi, Align, AlignBase);
    if (Free < Need) Rc = -1;
    return Rc;
}

/* ------------------------------------------------------------------ */
/* The boot area                                                       */
/* ------------------------------------------------------------------ */

static int
Geometry(SHIM *S)
{
    PUDF_VOL V = S->Vol;
    if (V->NMaps == 0) return Fail(S, "the volume is not mounted");
    if (V->SectorSize != 512) return Fail(S, "a boot view needs 512-byte sectors");
    S->Part = V->DataPart;
    if (V->Map[S->Part].Kind != UDF_KIND_PHYS || !V->Map[S->Part].HasBitmap)
        return Fail(S, "a boot view needs a plain physical partition with a space bitmap");
    S->P = V->Map[S->Part].Start;
    S->PartLen = V->Map[S->Part].Length;
    S->B = V->BlockSize;
    S->Spb = S->B / 512;
    S->C = ClusterBytes(S->B);
    S->Spc = S->C / 512;
    S->Align = S->C >= S->B ? S->C / S->B : 1;
    S->K = (FAT_SECTORS + ROOT_SECTORS + S->Spb - 1) / S->Spb;
    S->Fs = S->K * S->Spb - ROOT_SECTORS;
    S->R->ClusterBytes = S->C;
    return 0;
}

/* The highest block the boot area may start at: its FAT within the BPB's 65535 reserved sectors */
static ULONG
AreaLimit(SHIM *S)
{
    ULONG Max = 65535 / S->Spb;
    return Max > S->P ? Max - S->P : 0;
}

/* Finds UDFBOOT.SYS: one recorded extent, low on the disk */
static int
FindBootArea(SHIM *S)
{
    PUDF_INODE I;
    UDF_DIRENT *E;
    ULONG Blocks;
    E = (UDF_DIRENT *)ShAlloc(sizeof(UDF_DIRENT));
    if (!E) return Fail(S, "out of memory");
    if (!Find(S, &S->Vol->Root, UDF_SHIM_AREA_NAME, FALSE, &S->AreaLoc, E)) {
        ShFree(E);
        return Fail(S, "the volume has no boot area: format it with udffmt /BOOT");
    }
    ShFree(E);
    if (!UDF_SUCCESS(UdfIget(S->Vol, &S->AreaLoc, &I))) return Fail(S, "the boot area cannot be read");
    Blocks = (ULONG)(I->Size / S->B);
    if (I->AdType == UDF_ICB_AD_IN_ICB || I->NExt == 0 || I->Ext[0].Type != UDF_EXT_RECORDED ||
        I->Ext[0].Part != S->Part || I->Ext[0].Off != 0 || I->Ext[0].Len < Blocks * S->B ||
        (I->Size % S->B) != 0) {
        UdfIput(S->Vol, I);
        return Fail(S, "the boot area (" UDF_SHIM_AREA_NAME ") is not one contiguous run: run udfboot again");
    }
    S->A = I->Ext[0].Lbn;
    S->M = Blocks;
    UdfIput(S->Vol, I);
    if (S->A > AreaLimit(S) || S->M < S->K + S->Align ||
        ((S->M - S->K) * (ULONGLONG)S->B) % S->C != 0)
        return Fail(S, "the boot area is not usable: format the volume with udffmt /BOOT");
    S->Base = S->A + S->K;
    S->DataSec = (S->P + S->Base) * S->Spb;
    S->DirClusters = (ULONG)(((ULONGLONG)(S->M - S->K) * S->B) / S->C);
    {
        ULONGLONG Cl = ((ULONGLONG)(S->PartLen - S->Base) * S->B) / S->C;
        if (Cl > MAX_CLUSTERS) Cl = MAX_CLUSTERS;
        S->Clusters = (ULONG)Cl;
    }
    if (S->Clusters < 4085) return Fail(S, "the volume is too small for the boot view");
    S->WinEnd = S->Base + (ULONG)(((ULONGLONG)(S->Clusters < WIN_CLUSTERS ? S->Clusters : WIN_CLUSTERS) * S->C) / S->B);
    if (S->WinEnd > S->PartLen) S->WinEnd = S->PartLen;
    return 0;
}

UDF_STATUS
UdfShimReserve(PUDF_SHIM_HOST Host, PUDF_SHIM_RESULT Result)
{
    SHIM *S;
    PUDF_VOL V = Host->Vol;
    PUDF_INODE Root = NULL, I = NULL;
    UDF_DIRENT *E = NULL;
    UDF_LOC Loc;
    ULONG M, Hi, Lbn = 0, k;
    UDF_STATUS St = UDF_OK;

    UdfMemZero(Result, sizeof(*Result));
    if (V->ReadOnly) { Result->Error = "the volume is read-only"; return UDF_READ_ONLY; }
    S = (SHIM *)ShAlloc(sizeof(SHIM));
    E = (UDF_DIRENT *)ShAlloc(sizeof(UDF_DIRENT));
    if (!S || !E) { ShFree(S); ShFree(E); Result->Error = "out of memory"; return UDF_NO_MEMORY; }
    S->H = Host;
    S->Vol = V;
    S->R = Result;
    if (Geometry(S) != 0) { St = UDF_NOT_SUPPORTED; goto out; }
    if (Find(S, &V->Root, UDF_SHIM_AREA_NAME, FALSE, &Loc, E)) goto out;     /* there is one */

    M = UdfShimAreaBlocks(S->B);
    Hi = AreaLimit(S);
    if (Hi == 0) { Fail(S, "the partition starts too far into the volume for a boot view"); St = UDF_NOT_SUPPORTED; goto out; }
    Hi += M;
    if (Hi > S->PartLen) Hi = S->PartLen;
    St = UdfAllocAligned(V, S->Part, 0, Hi, M, 1, 0, &Lbn);
    if (St == UDF_DISK_FULL) {
        /* other files out of the way: then the start of the partition is free */
        S->NDirs = 0;
        S->AreaLoc.Lbn = 0xFFFFFFFF;
        (VOID)MakeRoom(S, 0, Hi, M, 0, 1);
        St = UdfAllocAligned(V, S->Part, 0, Hi, M, 1, 0, &Lbn);
    }
    if (!UDF_SUCCESS(St)) { Fail(S, "no room for the boot area near the start of the volume"); goto out; }
    /* zeros, then the file */
    for (k = 0; k < M && UDF_SUCCESS(St); k++) {
        ULONGLONG Abs;
        ULONG Run;
        St = UdfTranslate(V, S->Part, Lbn + k, &Abs, &Run, NULL);
        if (UDF_SUCCESS(St)) St = UdfWriteAbs(V, Abs, 1, V->Zero);
    }
    if (UDF_SUCCESS(St)) St = UdfIget(V, &V->Root, &Root);
    if (UDF_SUCCESS(St)) {
        St = UdfCreate(V, Root, UDF_SHIM_AREA_NAME, ShLen(UDF_SHIM_AREA_NAME), UDF_CREATE_FILE, 0444,
                       0xFFFFFFFF, 0xFFFFFFFF, NULL, 0, &I);
    }
    if (UDF_SUCCESS(St) && I->AdType == UDF_ICB_AD_IN_ICB) St = UdfIuninline(V, I);
    if (UDF_SUCCESS(St)) St = UdfIappend(V, I, S->Part, Lbn, M * S->B, UDF_EXT_RECORDED);
    if (UDF_SUCCESS(St)) {
        I->IcbFlags |= UDF_ICB_SYSTEM;
        UdfIdirty(V, I);
        Lbn = 0xFFFFFFFF;               /* the file's now */
        if (Find(S, &V->Root, UDF_SHIM_AREA_NAME, FALSE, &Loc, E))
            St = UdfDirSetChars(V, Root, E->Offset, UDF_FID_HIDDEN, 0);
    }
    if (I) UdfIput(V, I);
    if (Root) UdfIput(V, Root);
    if (!UDF_SUCCESS(St)) {
        if (Lbn != 0xFFFFFFFF) (VOID)UdfFree(V, S->Part, Lbn, M);
        Fail(S, "the boot area could not be created");
    }
    ShCommit(S);
out:
    ShFree(E);
    ShFree(S);
    return St;
}

/* ------------------------------------------------------------------ */
/* The files of the view                                               */
/* ------------------------------------------------------------------ */

static VOID
CopyName(char *To, const char *From, ULONG Max)
{
    ULONG n = 0;
    if (*From == '\\' || *From == '/') From++;
    while (From[n] && n + 1 < Max) { To[n] = From[n]; n++; }
    To[n] = 0;
}

/* The Windows directory: given, recorded by udfboot, BOOT.INI's default entry, or found */
static VOID
FindWinDir(SHIM *S, const char *Given)
{
    char *Root = S->R->WinDir;
    UCHAR Sec[1024];
    UDF_LOC Loc, W, X;
    ULONG k;

    Root[0] = 0;
    if (Given && *Given) { CopyName(Root, Given, sizeof(S->R->WinDir)); return; }
    if (ShRead(S, 0, 2, Sec) == 0 && UdfShimPresent(Sec) && Sec[UDF_SHIM_WINDIR] > ' ') {
        UdfMemCopy(Root, Sec + UDF_SHIM_WINDIR, 15);
        Root[15] = 0;
        return;
    }
    if (Find(S, &S->Vol->Root, "BOOT.INI", FALSE, &Loc, NULL)) {
        PUDF_INODE I;
        char *Txt = (char *)ShAlloc(4096);
        ULONG Got = 0;
        if (Txt && UDF_SUCCESS(UdfIget(S->Vol, &Loc, &I))) {
            if (UDF_SUCCESS(UdfIread(S->Vol, I, 0, 4095, Txt, &Got))) {
                char *P;
                Txt[Got] = 0;
                for (P = Txt; *P; P++) {
                    if ((P == Txt || P[-1] == '\n') && (P[0] | 0x20) == 'd' && !UdfMemCmp(P + 1, "efault", 6)) {
                        const char *Q = ShChr(P, '\\');
                        if (Q) {
                            ULONG n = 0;
                            Q++;
                            while (*Q && *Q != '\r' && *Q != '\n' && *Q != ' ' && *Q != '\t' && n < 15) Root[n++] = *Q++;
                            Root[n] = 0;
                        }
                        break;
                    }
                }
            }
            UdfIput(S->Vol, I);
        }
        ShFree(Txt);
        if (Root[0]) return;
    }
    {
        static const char *const Guess[] = { "WINNT", "WINDOWS", "WINNT40", "WINNT35", "WINNT4", "WINNT351", "WINNT31", 0 };
        for (k = 0; Guess[k]; k++) {
            if (Find(S, &S->Vol->Root, Guess[k], TRUE, &W, NULL) && Find(S, &W, "SYSTEM32", TRUE, &X, NULL)) {
                CopyName(Root, Guess[k], 16);
                return;
            }
        }
    }
}

static int
BuildTree(SHIM *S)
{
    static const char *const RootFiles[] = { "NTLDR", "NTDETECT.COM", "BOOT.INI", "BOOTSECT.DOS", "NTBOOTDD.SYS",
                                             "BOOTFONT.BIN", 0 };
    static const char *const Sys32Files[] = { "NT*.EXE", "HAL*.DLL", "KD*.DLL", "BOOTVID.DLL", "*.NLS", "*.FON",
                                              "C_*.*", "L_*.*", 0 };
    static const char *const ConfigFiles[] = { "SYSTEM", "SYSTEM.*", 0 };
    static const char *const FontFiles[] = { "*.FON", 0 };
    static const char *const SysRootFiles[] = { "BOOTSTAT.DAT", 0 };
    static const char *const AppPatchFiles[] = { "DRVMAIN.SDB", 0 };
    static const char *const InfFiles[] = { "BIOSINFO.INF", 0 };
    UDF_LOC SysRoot, Sys32, Loc, *RootLoc = &S->Vol->Root;
    int Wd, S32, Sr;

    if (!S->R->WinDir[0]) return Fail(S, "cannot tell the Windows directory: give it with /S");
    if (ShChr(S->R->WinDir, '\\')) return Fail(S, "the Windows directory must be in the root of the volume");
    if (!Find(S, RootLoc, S->R->WinDir, TRUE, &SysRoot, NULL) || !Find(S, &SysRoot, "SYSTEM32", TRUE, &Sys32, NULL))
        return Fail(S, "the Windows directory (with SYSTEM32) was not found");

    S->NDirs = 0;
    NewDir(S, -1, "", RootLoc);
    if (AddFiles(S, 0, RootLoc, RootFiles) < 0) return Fail(S, "out of memory");
    if (!Find(S, RootLoc, "NTLDR", FALSE, &Loc, NULL)) return Fail(S, "NTLDR is not in the root of the volume");
    Wd = NewDir(S, 0, S->R->WinDir, &SysRoot);
    if (Wd < 0) return Fail(S, "the Windows directory has no 8.3 name");
    if (AddFiles(S, Wd, &SysRoot, SysRootFiles) < 0) return Fail(S, "out of memory");
    S32 = NewDir(S, Wd, "SYSTEM32", &Sys32);
    if (S32 < 0 || AddFiles(S, S32, &Sys32, Sys32Files) < 0) return Fail(S, "out of memory");
    if (Find(S, &Sys32, "DRIVERS", TRUE, &Loc, NULL)) {
        Sr = NewDir(S, S32, "DRIVERS", &Loc);
        if (Sr < 0 || AddFiles(S, Sr, &Loc, 0) < 0) return Fail(S, "out of memory");
    }
    if (Find(S, &Sys32, "CONFIG", TRUE, &Loc, NULL)) {
        Sr = NewDir(S, S32, "CONFIG", &Loc);
        if (Sr < 0 || AddFiles(S, Sr, &Loc, ConfigFiles) < 0) return Fail(S, "out of memory");
    }
    if (Find(S, &SysRoot, "FONTS", TRUE, &Loc, NULL)) {
        Sr = NewDir(S, Wd, "FONTS", &Loc);
        if (Sr < 0 || AddFiles(S, Sr, &Loc, FontFiles) < 0) return Fail(S, "out of memory");
    }
    if (Find(S, &SysRoot, "SYSTEM", TRUE, &Loc, NULL)) {
        Sr = NewDir(S, Wd, "SYSTEM", &Loc);
        if (Sr < 0 || AddFiles(S, Sr, &Loc, FontFiles) < 0) return Fail(S, "out of memory");
    }
    if (Find(S, &SysRoot, "INF", TRUE, &Loc, NULL)) {
        Sr = NewDir(S, Wd, "INF", &Loc);
        if (Sr < 0 || AddFiles(S, Sr, &Loc, InfFiles) < 0) return Fail(S, "out of memory");
    }
    if (Find(S, &SysRoot, "APPPATCH", TRUE, &Loc, NULL)) {
        Sr = NewDir(S, Wd, "APPPATCH", &Loc);
        if (Sr < 0 || AddFiles(S, Sr, &Loc, AppPatchFiles) < 0) return Fail(S, "out of memory");
    }
    return 0;
}

/* Every file of the view inside the window, on the cluster grid (other files moved out of the way first) */
static int
PlaceFiles(SHIM *S, BOOLEAN Initial)
{
    ULONG Need = 0, d, k;
    for (d = 0; d < S->NDirs; d++)
        for (k = 0; k < S->D[d].Count; k++)
            if (S->D[d].F[k].Dir < 0 && S->D[d].F[k].Size) Need += PiecesToMove(S, &S->D[d].F[k].Loc);
    if (Need) {
        Need += 16 + Need / 32;
        if (Initial) {
            /* room for the files to grow (the registry), so that later updates need not move others */
            ULONG Extra = (ULONG)((4UL << 20) / ((ULONG)S->Align * S->B));
            if (Extra > S->Clusters / 16) Extra = S->Clusters / 16;
            Need += Extra;
        }
        if (MakeRoom(S, S->Base, S->WinEnd, S->Align, S->Base, Need) != 0)
            return Fail(S, "not enough free space near the start of the volume");
    }
    for (d = 0; d < S->NDirs; d++) {
        for (k = 0; k < S->D[d].Count; k++) {
            SFILE *F = &S->D[d].F[k];
            PUDF_INODE I;
            ULONG Moved = 0;
            UDF_STATUS St;
            BOOLEAN Ok;
            if (F->Dir >= 0 || F->Size == 0) continue;
            if (!UDF_SUCCESS(UdfIget(S->Vol, &F->Loc, &I))) { FailName(S, "a file of the view cannot be read", F->Name); return -1; }
            if (UdfIplaced(S->Vol, I, S->Base, S->WinEnd, S->Align, S->Base, TRUE)) { UdfIput(S->Vol, I); continue; }
            if (S->H->CanMove && !S->H->CanMove(S->H->Ctx, &F->Loc)) {
                UdfIput(S->Vol, I);
                FailName(S, "a file of the view is in use and cannot be moved", F->Name);
                return -1;
            }
            St = UdfIrelocate(S->Vol, I, S->Base, S->WinEnd, S->Align, S->Base, TRUE, &Moved);
            Ok = (BOOLEAN)(UDF_SUCCESS(St) && UdfIplaced(S->Vol, I, S->Base, S->WinEnd, S->Align, S->Base, TRUE));
            F->Size = (ULONG)I->Size;
            UdfIput(S->Vol, I);
            if (S->H->Moved) S->H->Moved(S->H->Ctx, &F->Loc);
            ShCommit(S);
            if (!Ok) {
                FailName(S, "no room to move a file near the start of the volume", F->Name);
                return -1;
            }
            S->R->MovedIn++;
        }
    }
    return 0;
}

/* Chains a file's clusters in the FAT; returns its first cluster (0: not on the grid) */
static ULONG
Chain(SHIM *S, const UDF_LOC *Loc, ULONG Size)
{
    PUDF_INODE I;
    ULONG N = (ULONG)(((ULONGLONG)Size + S->C - 1) / S->C), i, Prev = 0, First = 0;
    if (!UDF_SUCCESS(UdfIget(S->Vol, Loc, &I))) return 0;
    for (i = 0; i < N; i++) {
        ULONGLONG Off = (ULONGLONG)i * S->C, Abs, Sec;
        ULONG Run, C;
        UCHAR Kind;
        if (!UDF_SUCCESS(UdfImap(S->Vol, I, Off / S->B, &Abs, &Run, &Kind)) || Kind != UDF_MAP_RECORDED) { First = 0; break; }
        Sec = Abs * S->Spb + (ULONG)(Off % S->B) / 512;
        if (Sec < S->DataSec || ((Sec - S->DataSec) % S->Spc) != 0) { First = 0; break; }
        C = (ULONG)((Sec - S->DataSec) / S->Spc) + 2;
        if (C >= S->Clusters + 2) { First = 0; break; }
        if (Prev) S->Fat[Prev] = (USHORT)C; else First = C;
        Prev = C;
    }
    UdfIput(S->Vol, I);
    if (Prev && First) S->Fat[Prev] = 0xFFFF;
    return First;
}

/* ------------------------------------------------------------------ */
/* Writing the view                                                    */
/* ------------------------------------------------------------------ */

static void
DosTime(ULONGLONG Nt, PUCHAR Time, PUCHAR Date)
{
    ULONGLONG Secs1601 = Nt / 10000000ULL;
    ULONG Unix = Secs1601 > 11644473600ULL ? (ULONG)(Secs1601 - 11644473600ULL) : 0;
    ULONG Days = Unix / 86400, Secs = Unix % 86400, Y = 1970, M, D;
    static const UCHAR Ml[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    for (;;) {
        ULONG Yd = ((Y % 4 == 0 && Y % 100 != 0) || Y % 400 == 0) ? 366 : 365;
        if (Days < Yd) break;
        Days -= Yd; Y++;
    }
    for (M = 0; M < 12; M++) {
        ULONG L = Ml[M] + (M == 1 && ((Y % 4 == 0 && Y % 100 != 0) || Y % 400 == 0));
        if (Days < L) break;
        Days -= L;
    }
    D = Days + 1;
    if (Y < 1980) { Y = 1980; M = 0; D = 1; }
    if (Y > 2107) { Y = 2107; M = 11; D = 31; }
    {
        ULONG T = ((Secs / 3600) << 11) | (((Secs / 60) % 60) << 5) | ((Secs % 60) / 2);
        ULONG Dt = ((Y - 1980) << 9) | ((M + 1) << 5) | D;
        Time[0] = (UCHAR)T; Time[1] = (UCHAR)(T >> 8);
        Date[0] = (UCHAR)Dt; Date[1] = (UCHAR)(Dt >> 8);
    }
}

static void
PutEntry(PUCHAR E, const UCHAR *Name, UCHAR Attr, UCHAR Case, ULONG Icb, ULONGLONG Mtime, ULONG Cluster, ULONG Size)
{
    ShFill(E, 0, 32);
    UdfMemCopy(E, Name, 11);
    E[11] = Attr;
    E[12] = Case;
    /* the file entry's block, where a FAT keeps creation time (the view is read-only to NTLDR) */
    E[14] = (UCHAR)Icb; E[15] = (UCHAR)(Icb >> 8); E[16] = (UCHAR)(Icb >> 16); E[17] = (UCHAR)(Icb >> 24);
    DosTime(Mtime, E + 22, E + 24);
    E[18] = E[24]; E[19] = E[25];
    E[26] = (UCHAR)Cluster; E[27] = (UCHAR)(Cluster >> 8);
    E[28] = (UCHAR)Size; E[29] = (UCHAR)(Size >> 8); E[30] = (UCHAR)(Size >> 16); E[31] = (UCHAR)(Size >> 24);
}

/* The entries of directory d (with "." and ".." for subdirectories) */
static PUCHAR
DirImage(SHIM *S, ULONG d, PULONG Bytes)
{
    SDIR *D = &S->D[d];
    ULONG n = D->Count + (d ? 2 : 0), Size, k, Pos = 0;
    PUCHAR Img;
    static const UCHAR Dot[11] = { '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
    static const UCHAR DotDot[11] = { '.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
    Size = d ? ((n * 32 + S->C - 1) / S->C) * S->C : ROOT_SECTORS * 512;
    Img = (PUCHAR)ShAlloc(Size);
    if (!Img) return NULL;
    if (d) {
        PutEntry(Img, Dot, 0x10, 0, D->Loc.Lbn, 0, D->First, 0);
        PutEntry(Img + 32, DotDot, 0x10, 0, S->D[D->Parent].Loc.Lbn, 0, S->D[D->Parent].First, 0);
        Pos = 64;
    }
    for (k = 0; k < D->Count; k++, Pos += 32) {
        SFILE *F = &D->F[k];
        PutEntry(Img + Pos, F->Name, F->Attr, F->Case, F->Loc.Lbn, F->Mtime,
                 F->Dir >= 0 ? S->D[F->Dir].First : F->First, F->Size);
    }
    *Bytes = Size;
    return Img;
}

/* The FAT, the root directory and the subdirectories */
static int
WriteView(SHIM *S)
{
    ULONG d, k, FatBytes, Bytes, Next = 0;
    ULONG AreaSec = (S->P + S->A) * S->Spb;
    PUCHAR FatImg, Dir;
    int Rc;

    S->Fat = (USHORT *)ShAlloc((S->Clusters + 2) * sizeof(USHORT));
    if (!S->Fat) return Fail(S, "out of memory");
    for (k = 0; k < S->Clusters + 2; k++) S->Fat[k] = 0xFFF7;        /* bad: nothing may go there */
    S->Fat[0] = 0xFFF8;
    S->Fat[1] = 0xFFFF;

    /* subdirectory clusters: the boot area after the root directory */
    for (d = 1; d < S->NDirs; d++) {
        ULONG Need = ((S->D[d].Count + 2) * 32 + S->C - 1) / S->C, c;
        if (Next + Need > S->DirClusters) return Fail(S, "the boot area is too small for the directories");
        S->D[d].First = Next + 2;
        S->D[d].Clusters = Need;
        for (c = 0; c < Need; c++) {
            ULONG Cl = S->D[d].First + c;
            S->Fat[Cl] = (USHORT)(c + 1 < Need ? Cl + 1 : 0xFFFF);
        }
        Next += Need;
    }
    for (d = 0; d < S->NDirs; d++) {
        for (k = 0; k < S->D[d].Count; k++) {
            SFILE *F = &S->D[d].F[k];
            if (F->Dir >= 0 || F->Size == 0) continue;
            F->First = Chain(S, &F->Loc, F->Size);
            if (!F->First) { FailName(S, "a file of the view is not on the cluster grid", F->Name); return -1; }
        }
    }

    FatBytes = S->Fs * 512;
    FatImg = (PUCHAR)ShAlloc(FatBytes);
    if (!FatImg) return Fail(S, "out of memory");
    for (k = 0; k < S->Clusters + 2 && k * 2 + 1 < FatBytes; k++) {
        FatImg[k * 2] = (UCHAR)S->Fat[k];
        FatImg[k * 2 + 1] = (UCHAR)(S->Fat[k] >> 8);
    }
    Rc = ShWrite(S, AreaSec, S->Fs, FatImg);
    ShFree(FatImg);
    if (Rc != 0) return Fail(S, "write error (FAT)");
    for (d = 0; d < S->NDirs; d++) {
        if (d == 0 && S->D[0].Count > 256) return Fail(S, "too many files in the root");
        Dir = DirImage(S, d, &Bytes);
        if (!Dir) return Fail(S, "out of memory");
        if (d == 0) Rc = ShWrite(S, AreaSec + S->Fs, ROOT_SECTORS, Dir);
        else Rc = ShWrite(S, S->DataSec + (S->D[d].First - 2) * S->Spc, Bytes / 512, Dir);
        ShFree(Dir);
        if (Rc != 0) return Fail(S, "write error (directory)");
    }
    return 0;
}

static int
WriteBootSector(SHIM *S, const UCHAR *Code)
{
    UCHAR B[1024];
    ULONG Reserved = (S->P + S->A) * S->Spb;
    ULONG Total = S->DataSec + S->Clusters * S->Spc;
    ULONG Spt = S->H->SectorsPerTrack ? S->H->SectorsPerTrack : 63;
    ULONG Heads = S->H->Heads ? S->H->Heads : 255;
    ULONG Hidden = S->H->PartitionStart;
    ULONG Serial = S->Vol->Serial;
    UdfMemCopy(B, Code, 1024);
    UdfMemCopy(B + 3, UDF_SHIM_OEM, 8);
    B[0x0B] = 0x00; B[0x0C] = 0x02;          /* 512 bytes a sector */
    B[0x0D] = (UCHAR)S->Spc;
    B[0x0E] = (UCHAR)Reserved; B[0x0F] = (UCHAR)(Reserved >> 8);
    B[0x10] = 1;                             /* one FAT */
    B[0x11] = (UCHAR)(ROOT_SECTORS * 16); B[0x12] = (UCHAR)((ROOT_SECTORS * 16) >> 8);
    B[0x13] = 0; B[0x14] = 0;
    B[0x15] = 0xF8;
    B[0x16] = (UCHAR)S->Fs; B[0x17] = (UCHAR)(S->Fs >> 8);
    B[0x18] = (UCHAR)Spt; B[0x19] = 0;
    B[0x1A] = (UCHAR)Heads; B[0x1B] = (UCHAR)(Heads >> 8);
    B[0x1C] = (UCHAR)Hidden; B[0x1D] = (UCHAR)(Hidden >> 8);
    B[0x1E] = (UCHAR)(Hidden >> 16); B[0x1F] = (UCHAR)(Hidden >> 24);
    B[0x20] = (UCHAR)Total; B[0x21] = (UCHAR)(Total >> 8); B[0x22] = (UCHAR)(Total >> 16); B[0x23] = (UCHAR)(Total >> 24);
    B[0x24] = 0x80; B[0x25] = 0; B[0x26] = 0x29;
    B[0x27] = (UCHAR)Serial; B[0x28] = (UCHAR)(Serial >> 8); B[0x29] = (UCHAR)(Serial >> 16); B[0x2A] = (UCHAR)(Serial >> 24);
    UdfMemCopy(B + 0x2B, "UDFNT BOOT ", 11);
    UdfMemCopy(B + 0x36, "FAT16   ", 8);
    ShFill(B + UDF_SHIM_WINDIR, 0, 16);
    UdfMemCopy(B + UDF_SHIM_WINDIR, S->R->WinDir, ShLen(S->R->WinDir));
    return ShWrite(S, 0, 2, B);
}

UDF_STATUS
UdfShimUpdate(PUDF_SHIM_HOST Host, const char *WinDir, const UCHAR *Code, PUDF_SHIM_RESULT Result)
{
    SHIM *S;
    ULONG d, k;
    USHORT SavedPart;
    ULONG SavedLo, SavedHi;
    int Rc;

    UdfMemZero(Result, sizeof(*Result));
    if (Host->Vol->ReadOnly) { Result->Error = "the volume is read-only"; return UDF_READ_ONLY; }
    S = (SHIM *)ShAlloc(sizeof(SHIM));
    if (!S) { Result->Error = "out of memory"; return UDF_NO_MEMORY; }
    S->H = Host;
    S->Vol = Host->Vol;
    S->R = Result;
    SavedPart = S->Vol->AvoidPart; SavedLo = S->Vol->AvoidLo; SavedHi = S->Vol->AvoidHi;

    Rc = Geometry(S);
    if (Rc == 0) Rc = FindBootArea(S);
    if (Rc == 0) {
        /* while files move, nothing new goes into the window */
        S->Vol->AvoidPart = S->Part; S->Vol->AvoidLo = S->Base; S->Vol->AvoidHi = S->WinEnd;
        FindWinDir(S, WinDir);
        Rc = BuildTree(S);
    }
    if (Rc == 0) Rc = PlaceFiles(S, (BOOLEAN)(Code != 0));
    if (Rc == 0) Rc = WriteView(S);
    if (Rc == 0 && Code && WriteBootSector(S, Code) != 0) Rc = Fail(S, "write error (boot sector)");
    ShCommit(S);
    if (Rc == 0) {
        /* from now on the window is allocated last */
        S->Vol->AvoidPart = S->Part; S->Vol->AvoidLo = S->Base; S->Vol->AvoidHi = S->WinEnd;
    } else {
        S->Vol->AvoidPart = SavedPart; S->Vol->AvoidLo = SavedLo; S->Vol->AvoidHi = SavedHi;
    }

    for (d = 0; d < S->NDirs; d++) {
        for (k = 0; k < S->D[d].Count; k++) if (S->D[d].F[k].Dir < 0) Result->Files++;
        ShFree(S->D[d].F);
    }
    Result->Dirs = S->NDirs;
    ShFree(S->Fat);
    ShFree(S);
    return Rc == 0 ? UDF_OK : UDF_CORRUPT;
}
