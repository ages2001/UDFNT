/*
 * UDFNT - the driver layer's engine interface on UDF (see ufx.h).
 *
 * Every changing call stamps the engine with the current time first. The
 * engine keeps its own file entries; the layer's inode copies are filled from
 * them and written back field by field.
 */
#include "udfeng.h"
#include "ufx.h"

#define V2U(V)      (&(V)->Udf)
#define EPOCH_100NS 116444736000000000ULL      /* 1601 -> 1970 */

ULONGLONG UfxHostNowNt(VOID);

/* ---- time ---------------------------------------------------------------- */
VOID
UfxSetTime(ULONG *Field, ULONG *Extra, ULONGLONG Nt100ns)
{
    LONGLONG Unix100 = (LONGLONG)Nt100ns - (LONGLONG)EPOCH_100NS;
    LONGLONG Secs = Unix100 / 10000000LL, Rem = Unix100 % 10000000LL;
    ULONG Nsec;
    if (Rem < 0) { Rem += 10000000LL; Secs--; }
    if (Secs < -0x80000000LL) Secs = -0x80000000LL;
    if (Secs > 0x37FFFFFFFLL) Secs = 0x37FFFFFFFLL;
    Nsec = (ULONG)(Rem * 100);
    *Field = (ULONG)Secs;
    if (Extra) *Extra = ((ULONG)((Secs - (LONGLONG)(LONG)(ULONG)Secs) >> 32) & 3) | (Nsec << 2);
}

ULONGLONG
UfxGetTime(ULONG Field, ULONG Extra)
{
    LONGLONG Secs = (LONGLONG)(LONG)Field;
    ULONG Nsec = Extra >> 2;
    Secs += (LONGLONG)(Extra & 3) << 32;
    if (Nsec > 999999999) Nsec = 0;
    if (Secs * 10000000LL + (LONGLONG)EPOCH_100NS < 0) return 0;
    return (ULONGLONG)((LONGLONG)EPOCH_100NS + Secs * 10000000LL + Nsec / 100);
}

static VOID ToFields(const UDF_TIME *T, ULONG *Field, ULONG *Extra) { UfxSetTime(Field, Extra, T->Nt ? T->Nt : EPOCH_100NS); }
static VOID FromFields(ULONG Field, ULONG Extra, PUDF_TIME T) { T->Nt = UfxGetTime(Field, Extra); T->Tz = 0; }

VOID
UfxOpBegin(PUFX_VOL Vol)
{
    V2U(Vol)->Now = UfxHostNowNt();
}

UFX_STATUS
UfxOpEnd(PUFX_VOL Vol, UFX_STATUS Status)
{
    V2U(Vol)->Now = 0;
    return Status;
}

#define CHANGE_BEGIN(V)     UDF_STATUS _cb; UfxOpBegin(V); if ((V)->ReadOnly) return UFX_READ_ONLY; \
                            _cb = UdfChangeBegin(V2U(V)); if (!UDF_SUCCESS(_cb)) return UfxOpEnd(V, _cb)
#define CHANGE_END(V, st)   UfxOpEnd(V, st)

/* ---- inode numbers ------------------------------------------------------- */
static BOOLEAN
Resolve(PUFX_VOL V, ULONG Ino, PUDF_LOC L)
{
    if (Ino == UFX_ROOT_INO) { *L = V->Udf.Root; return TRUE; }
    if (Ino & UFX_VIRTUAL_INO) {
        ULONG i = Ino & ~UFX_VIRTUAL_INO;
        if (i >= V->VinoCount) return FALSE;
        *L = V->Vino[i].Loc;
        return TRUE;
    }
    if (Ino < UFX_INO_BIAS) return FALSE;
    L->Part = V->Udf.IcbPart;
    L->Lbn = Ino - UFX_INO_BIAS;
    return TRUE;
}

static ULONG
MakeIno(PUFX_VOL V, const UDF_LOC *L)
{
    ULONG i;
    if (L->Lbn == V->Udf.Root.Lbn && L->Part == V->Udf.Root.Part) return UFX_ROOT_INO;
    if (L->Part == V->Udf.IcbPart && L->Lbn < UFX_VIRTUAL_INO - UFX_INO_BIAS) return L->Lbn + UFX_INO_BIAS;
    for (i = 0; i < V->VinoCount; i++)
        if (V->Vino[i].Loc.Lbn == L->Lbn && V->Vino[i].Loc.Part == L->Part) return UFX_VIRTUAL_INO | i;
    if (V->VinoCount == V->VinoCap) {
        ULONG cap = V->VinoCap ? V->VinoCap * 2 : 64;
        UFX_VINO *n = (UFX_VINO *)UfxHostAlloc(cap * sizeof(UFX_VINO));
        if (!n) return 0;
        if (V->Vino) { UdfMemCopy(n, V->Vino, V->VinoCount * sizeof(UFX_VINO)); UfxHostFree(V->Vino); }
        V->Vino = n;
        V->VinoCap = cap;
    }
    V->Vino[V->VinoCount].Loc = *L;
    return UFX_VIRTUAL_INO | V->VinoCount++;
}

/* the driver's number for a file entry (the boot view moves files by location) */
ULONG
UfxInoOfLoc(PUFX_VOL Vol, const UDF_LOC *Loc)
{
    return MakeIno(Vol, Loc);
}

static UDF_STATUS
Get(PUFX_VOL V, ULONG Ino, PUDF_INODE *I)
{
    UDF_LOC l;
    if (!Resolve(V, Ino, &l)) return UFX_NOT_FOUND;
    return UdfIget(V2U(V), &l, I);
}

ULONGLONG
UfxInodeNumber(PUFX_VOL Vol, ULONG Ino)
{
    PUDF_INODE I;
    ULONGLONG id;
    if (!UDF_SUCCESS(Get(Vol, Ino, &I))) return Ino;
    id = I->UniqueId;
    UdfIput(V2U(Vol), I);
    return id ? id : Ino;
}

/* where a name was seen: its directory and entry (for the hidden flag) */
static UFX_SEEN *
Seen(PUFX_VOL V, ULONG Ino, BOOLEAN Create)
{
    UFX_SEEN *s = &V->Seen[(Ino * 2654435761u) % UFX_SEEN_SIZE];
    if (s->Ino == Ino) return s;
    if (!Create) return NULL;
    s->Ino = Ino;
    return s;
}

static VOID
NoteName(PUFX_VOL V, ULONG Ino, ULONG DirIno, const UDF_DIRENT *E)
{
    UFX_SEEN *s = Seen(V, Ino, TRUE);
    s->DirIno = DirIno;
    s->Offset = E->Offset;
    s->Hidden = (UCHAR)((E->Chars & UDF_FID_HIDDEN) != 0);
}

/* ---- volume -------------------------------------------------------------- */
BOOLEAN
UfxIsVrs(const UCHAR *Buf, ULONG Length, ULONG Step)
{
    ULONG off;
    BOOLEAN bea = FALSE;
    for (off = 0; off + 7 <= Length; off += Step) {
        const UCHAR *d = Buf + off;
        if (!UdfMemCmp(d + 1, "BEA01", 5)) bea = TRUE;
        else if (!UdfMemCmp(d + 1, "NSR02", 5) || !UdfMemCmp(d + 1, "NSR03", 5)) { if (bea) return TRUE; }
        else if (!UdfMemCmp(d + 1, "TEA01", 5)) return FALSE;
        else if (!UdfMemCmp(d + 1, "CD001", 5) || !UdfMemCmp(d + 1, "CDW02", 5) || !UdfMemCmp(d + 1, "BOOT2", 5)) continue;
        else if (bea) return FALSE;
    }
    return FALSE;
}

VOID
UfxCountFree(PUFX_VOL Vol)
{
    ULONGLONG total, free;
    UdfFreeSpace(V2U(Vol), &total, &free);
    Vol->DataBlocks = total;
    Vol->FreeBlocksCount = free;
}

UFX_STATUS
UfxMount(PUFX_VOL Vol, BOOLEAN ReadOnly)
{
    PUDF_VOL U = V2U(Vol);
    UDF_STATUS st;
    ULONG i;
    UdfMemZero(U, sizeof(*U));
    U->HostContext = Vol->HostContext;
    U->SectorSize = Vol->DeviceSector;
    U->DeviceBytes = Vol->DeviceBytes;
    U->Optical = Vol->Optical;
    U->BufMax = 4096;
    U->InoMax = 2048;
    st = UdfMount(U, ReadOnly);
    if (!UDF_SUCCESS(st)) return st;
    Vol->BlockSize = U->BlockSize;
    Vol->BlockShift = U->BlockShift;
    Vol->SectorsPerBlock = U->BlockSize / (Vol->DeviceSector ? Vol->DeviceSector : 512);
    Vol->BlocksCount = U->Blocks;
    Vol->InodeSize = 256;
    Vol->GroupCount = U->NMaps;
    Vol->NodeSize = U->BlockSize;
    Vol->InodesCount = U->NumFiles + U->NumDirs;
    UdfMemZero(Vol->VolumeName, sizeof(Vol->VolumeName));
    for (i = 0; i < 255 && U->Label[i]; i++) Vol->VolumeName[i] = U->Label[i];
    /* cut a partial UTF-8 character */
    while (i && ((UCHAR)Vol->VolumeName[i - 1] & 0xC0) == 0x80) i--;
    if (i && ((UCHAR)Vol->VolumeName[i - 1] & 0xC0) == 0xC0) Vol->VolumeName[--i] = 0;
    UdfMemCopy(Vol->Uuid, U->VolSetId, 16);
    Vol->SerialNumber = U->Serial;
    Vol->ReadOnly = U->ReadOnly;
    Vol->ReadOnlyForced = (BOOLEAN)(U->ReadOnlyForced && !ReadOnly);
    Vol->DataCopies = FALSE;
    Vol->Dirty = U->WasOpen;
    UfxCountFree(Vol);
    return UFX_OK;
}

VOID
UfxUnmountFree(PUFX_VOL Vol)
{
    if (V2U(Vol)->NMaps) UdfUnmount(V2U(Vol));
    if (Vol->Vino) UfxHostFree(Vol->Vino);
    Vol->Vino = NULL;
    Vol->VinoCount = Vol->VinoCap = 0;
    UdfMemZero(Vol->Seen, sizeof(Vol->Seen));
}

/* the volume on the disk is (still) this one: its first volume descriptors */
BOOLEAN
UfxSameVolume(PUFX_VOL Vol)
{
    PUDF_VOL U = V2U(Vol);
    PUCHAR b;
    BOOLEAN same = FALSE;
    if (!U->NMaps || !U->PvdBlock[0]) return TRUE;
    b = (PUCHAR)UfxHostAlloc(U->BlockSize);
    if (!b) return TRUE;
    if (UDF_SUCCESS(UdfHostRead(U, (ULONGLONG)U->PvdBlock[0] << U->BlockShift, U->BlockSize, b)) &&
        UdfTagCheck(b, U->BlockSize, UDF_TAG_PVD, U->PvdBlock[0]) &&
        !UdfMemCmp(b + UDF_PVD_VOLSETID, U->VolSetId, 128)) {
        same = TRUE;
    }
    UfxHostFree(b);
    return same;
}

UFX_STATUS UfxStartWriting(PUFX_VOL Vol) { return Vol->ReadOnly ? UFX_READ_ONLY : UFX_OK; }

UFX_STATUS
UfxTxnCommit(PUFX_VOL Vol)
{
    UDF_STATUS st;
    if (Vol->ReadOnly || !UdfDirty(V2U(Vol))) return UFX_OK;
    UfxOpBegin(Vol);
    st = UdfCommit(V2U(Vol));
    UfxOpEnd(Vol, st);
    UfxCountFree(Vol);
    return st;
}

UFX_STATUS UfxStopWriting(PUFX_VOL Vol) { return UfxTxnCommit(Vol); }
BOOLEAN    UfxTxnPending(PUFX_VOL Vol) { return (BOOLEAN)(!Vol->ReadOnly && UdfDirty(V2U(Vol))); }
ULONG      UfxDirtyBytes(PUFX_VOL Vol) { return UdfDirtyBytes(V2U(Vol)); }

UFX_STATUS
UfxSetLabel(PUFX_VOL Vol, const char *Label)
{
    ULONG n = 0;
    UDF_STATUS st;
    CHANGE_BEGIN(Vol);
    while (Label[n] && n < 255) n++;
    st = UdfSetLabel(V2U(Vol), Label, n);
    if (UDF_SUCCESS(st)) { UdfMemZero(Vol->VolumeName, 256); UdfMemCopy(Vol->VolumeName, Label, n); }
    return CHANGE_END(Vol, st);
}

UFX_STATUS UfxProcessOrphans(PUFX_VOL Vol) { (VOID)Vol; return UFX_OK; }

/* ---- inodes -------------------------------------------------------------- */
static VOID
Fill(PUFX_VOL V, PUDF_INODE I, PUFX_INODE X)
{
    ULONG mode = UdfIMode(I);
    ULONGLONG blocks512 = UdfIblocks(V2U(V), I) << (V->BlockShift - 9);
    UdfMemZero(X, sizeof(*X));
    X->Mode = (USHORT)mode;
    X->UidLo = (USHORT)(I->Uid == 0xFFFFFFFF ? 0 : I->Uid);
    X->UidHi = (USHORT)(I->Uid == 0xFFFFFFFF ? 0 : I->Uid >> 16);
    X->GidLo = (USHORT)(I->Gid == 0xFFFFFFFF ? 0 : I->Gid);
    X->GidHi = (USHORT)(I->Gid == 0xFFFFFFFF ? 0 : I->Gid >> 16);
    X->SizeLo = (ULONG)I->Size;
    X->SizeHi = (ULONG)(I->Size >> 32);
    ToFields(&I->Atime, &X->Atime, &X->AtimeExtra);
    ToFields(&I->Mtime, &X->Mtime, &X->MtimeExtra);
    ToFields(&I->Ctime, &X->Ctime, &X->CtimeExtra);
    ToFields(&I->Btime, &X->Crtime, &X->CrtimeExtra);
    /* a directory: its own name and its subdirectories on the disk, "." as Unix counts it */
    X->LinksCount = (USHORT)(UdfIsDir(I) ? (I->Links < 0xFFFF ? I->Links + 1 : I->Links) : I->Links);
    X->BlocksLo = (ULONG)blocks512;
    X->BlocksHi = (USHORT)(blocks512 >> 32);
    X->ExtraIsize = 32;
    X->UniqueId = I->UniqueId;
    X->Generation = (ULONG)I->UniqueId;
    if (I->AdType == UDF_ICB_AD_IN_ICB && !UdfIsDir(I)) X->Flags |= UFX_IF_INLINE_DATA;
    if (I->FileType == UDF_FT_SYMLINK) X->Flags |= UFX_IF_INLINE_DATA;
}

UFX_STATUS
UfxReadInode(PUFX_VOL Vol, ULONG Ino, PUFX_INODE Inode)
{
    PUDF_INODE I;
    UDF_STATUS st = Get(Vol, Ino, &I);
    if (!UDF_SUCCESS(st)) return st;
    Fill(Vol, I, Inode);
    UdfIput(V2U(Vol), I);
    return UFX_OK;
}

/* the fields the driver owns: mode bits, owner, size (files), times */
UFX_STATUS
UfxWriteInode(PUFX_VOL Vol, ULONG Ino, PUFX_INODE X)
{
    PUDF_INODE I;
    UDF_STATUS st;
    ULONG uid, gid;
    CHANGE_BEGIN(Vol);
    st = Get(Vol, Ino, &I);
    if (!UDF_SUCCESS(st)) return CHANGE_END(Vol, st);
    UdfISetMode(I, X->Mode & 07777);
    uid = X->UidLo | ((ULONG)X->UidHi << 16);
    gid = X->GidLo | ((ULONG)X->GidHi << 16);
    if (uid || I->Uid != 0xFFFFFFFF) I->Uid = uid;
    if (gid || I->Gid != 0xFFFFFFFF) I->Gid = gid;
    FromFields(X->Atime, X->AtimeExtra, &I->Atime);
    FromFields(X->Mtime, X->MtimeExtra, &I->Mtime);
    FromFields(X->Ctime, X->CtimeExtra, &I->Ctime);
    if (X->Crtime) FromFields(X->Crtime, X->CrtimeExtra, &I->Btime);
    UdfIdirty(V2U(Vol), I);
    if (!UdfIsDir(I) && I->FileType != UDF_FT_SYMLINK) {
        ULONGLONG size = X->SizeLo | ((ULONGLONG)X->SizeHi << 32);
        if (size != I->Size) st = UdfIsetSize(V2U(Vol), I, size);
    }
    UdfIput(V2U(Vol), I);
    return CHANGE_END(Vol, st);
}

ULONGLONG UfxInodeSize(PUFX_VOL Vol, PUFX_INODE I) { (VOID)Vol; return I->SizeLo | ((ULONGLONG)I->SizeHi << 32); }
VOID      UfxInodeSetSize(PUFX_VOL Vol, PUFX_INODE I, ULONGLONG Size) { (VOID)Vol; I->SizeLo = (ULONG)Size; I->SizeHi = (ULONG)(Size >> 32); }
ULONGLONG UfxInodeBlocks(PUFX_VOL Vol, PUFX_INODE I) { (VOID)Vol; return I->BlocksLo | ((ULONGLONG)I->BlocksHi << 32); }

/* the engine-kept fields of a copy the caller holds, after a change */
static VOID
Refresh(PUFX_VOL V, ULONG Ino, PUFX_INODE X)
{
    UFX_INODE N;
    if (!X || !UDF_SUCCESS(UfxReadInode(V, Ino, &N))) return;
    X->LinksCount = N.LinksCount;
    X->BlocksLo = N.BlocksLo;
    X->BlocksHi = N.BlocksHi;
    X->Flags = (X->Flags & ~UFX_IF_INLINE_DATA) | (N.Flags & UFX_IF_INLINE_DATA);
    if ((N.Mode & UFX_S_IFMT) == UFX_S_IFDIR) {
        X->SizeLo = N.SizeLo; X->SizeHi = N.SizeHi;
        X->Mtime = N.Mtime; X->MtimeExtra = N.MtimeExtra;
        X->Ctime = N.Ctime; X->CtimeExtra = N.CtimeExtra;
    }
}

/* ---- directories --------------------------------------------------------- */
static VOID
CopyName(PUFX_ENTRY Entry, const char *Name, ULONG Len)
{
    if (Len > UFX_MAX_NAME) Len = UFX_MAX_NAME;
    UdfMemCopy(Entry->Name, Name, Len);
    Entry->Name[Len] = 0;
    Entry->NameLength = (USHORT)Len;
}

static VOID
FromEnt(PUFX_VOL Vol, ULONG DirIno, const UDF_DIRENT *E, PUFX_ENTRY Entry)
{
    Entry->Inode = MakeIno(Vol, &E->Icb);
    Entry->FileType = (E->Chars & UDF_FID_DIRECTORY) ? UFX_FT_DIR : UFX_FT_UNKNOWN;
    Entry->Cookie = E->Offset + E->FidLen;
    CopyName(Entry, E->Name, E->NameLen);
    if (Entry->Inode) NoteName(Vol, Entry->Inode, DirIno, E);
}

UFX_STATUS
UfxLookup(PUFX_VOL Vol, ULONG DirIno, PUFX_INODE Dir, const char *Name, ULONG NameLen, PUFX_ENTRY Entry)
{
    PUDF_INODE D;
    UDF_DIRENT *e;
    UDF_STATUS st;
    (VOID)Dir;
    st = Get(Vol, DirIno, &D);
    if (!UDF_SUCCESS(st)) return st;
    e = (UDF_DIRENT *)UfxHostAlloc(sizeof(UDF_DIRENT));
    if (!e) { UdfIput(V2U(Vol), D); return UFX_NO_MEMORY; }
    st = UdfDirLookup(V2U(Vol), D, Name, NameLen, e);
    if (UDF_SUCCESS(st)) {
        FromEnt(Vol, DirIno, e, Entry);
        Entry->Cookie = 0;
        if (!Entry->Inode) st = UFX_NO_MEMORY;
    }
    UfxHostFree(e);
    UdfIput(V2U(Vol), D);
    return st;
}

UFX_STATUS
UfxEnum(PUFX_VOL Vol, ULONG DirIno, PUFX_INODE Dir, ULONGLONG Cookie, PUFX_ENTRY Entry)
{
    PUDF_INODE D;
    UDF_DIRENT *e;
    UDF_STATUS st;
    (VOID)Dir;
    st = Get(Vol, DirIno, &D);
    if (!UDF_SUCCESS(st)) return st;
    e = (UDF_DIRENT *)UfxHostAlloc(sizeof(UDF_DIRENT));
    if (!e) { UdfIput(V2U(Vol), D); return UFX_NO_MEMORY; }
    for (;;) {
        st = UdfDirNext(V2U(Vol), D, Cookie, e);
        if (st == UDF_NO_MORE) { st = UFX_NO_MORE_ENTRIES; break; }
        if (!UDF_SUCCESS(st)) break;
        Cookie = e->Offset + e->FidLen;
        if (e->Chars & UDF_FID_PARENT) continue;
        if (e->NameLen == 0) continue;
        FromEnt(Vol, DirIno, e, Entry);
        if (!Entry->Inode) st = UFX_NO_MEMORY;
        break;
    }
    UfxHostFree(e);
    UdfIput(V2U(Vol), D);
    return st;
}

UFX_STATUS
UfxDirIsEmpty(PUFX_VOL Vol, ULONG DirIno, PUFX_INODE Dir, PBOOLEAN Empty)
{
    PUDF_INODE D;
    UDF_STATUS st;
    (VOID)Dir;
    st = Get(Vol, DirIno, &D);
    if (!UDF_SUCCESS(st)) return st;
    st = UdfDirIsEmpty(V2U(Vol), D, Empty);
    UdfIput(V2U(Vol), D);
    return st;
}

UFX_STATUS
UfxGetParent(PUFX_VOL Vol, ULONG DirIno, PUFX_INODE Dir, PULONG Parent)
{
    PUDF_INODE D;
    UDF_LOC p;
    UDF_STATUS st;
    (VOID)Dir;
    if (DirIno == UFX_ROOT_INO) { *Parent = UFX_ROOT_INO; return UFX_OK; }
    st = Get(Vol, DirIno, &D);
    if (!UDF_SUCCESS(st)) return st;
    st = UdfDirParent(V2U(Vol), D, &p);
    UdfIput(V2U(Vol), D);
    if (!UDF_SUCCESS(st)) return st;
    *Parent = MakeIno(Vol, &p);
    return *Parent ? UFX_OK : UFX_NO_MEMORY;
}

UFX_STATUS
UfxCreate(PUFX_VOL Vol, ULONG DirIno, PUFX_INODE Dir, const char *Name, ULONG NameLen,
          USHORT Mode, const char *Target, PULONG NewIno, PUFX_INODE NewInode)
{
    PUDF_INODE D, I;
    UDF_STATUS st;
    ULONG kind, tlen = 0;
    UDF_DIRENT *e;
    CHANGE_BEGIN(Vol);
    st = Get(Vol, DirIno, &D);
    if (!UDF_SUCCESS(st)) return CHANGE_END(Vol, st);
    switch (Mode & UFX_S_IFMT) {
    case UFX_S_IFDIR: kind = UDF_CREATE_DIR; break;
    case UFX_S_IFLNK: kind = UDF_CREATE_SYMLINK; break;
    case UFX_S_IFREG: kind = UDF_CREATE_FILE; break;
    default: UdfIput(V2U(Vol), D); return CHANGE_END(Vol, UFX_NOT_SUPPORTED);
    }
    if (Target) while (Target[tlen]) tlen++;
    st = UdfCreate(V2U(Vol), D, Name, NameLen, kind, Mode & 07777, 0xFFFFFFFF, 0xFFFFFFFF, Target, tlen, &I);
    if (UDF_SUCCESS(st)) {
        *NewIno = MakeIno(Vol, &I->Loc);
        if (!*NewIno) st = UFX_NO_MEMORY;
        else if (NewInode) Fill(Vol, I, NewInode);
        UdfIput(V2U(Vol), I);
        /* its entry, for the hidden flag */
        e = (UDF_DIRENT *)UfxHostAlloc(sizeof(UDF_DIRENT));
        if (e && UDF_SUCCESS(st) && UDF_SUCCESS(UdfDirLookup(V2U(Vol), D, Name, NameLen, e))) NoteName(Vol, *NewIno, DirIno, e);
        if (e) UfxHostFree(e);
    }
    UdfIput(V2U(Vol), D);
    if (UDF_SUCCESS(st)) Refresh(Vol, DirIno, Dir);
    return CHANGE_END(Vol, st);
}

static UFX_STATUS
DoUnlink(PUFX_VOL Vol, ULONG DirIno, PUFX_INODE Dir, const char *Name, ULONG NameLen, BOOLEAN Keep)
{
    PUDF_INODE D;
    UDF_STATUS st;
    CHANGE_BEGIN(Vol);
    st = Get(Vol, DirIno, &D);
    if (!UDF_SUCCESS(st)) return CHANGE_END(Vol, st);
    st = UdfUnlink(V2U(Vol), D, Name, NameLen, Keep);
    UdfIput(V2U(Vol), D);
    if (UDF_SUCCESS(st)) Refresh(Vol, DirIno, Dir);
    return CHANGE_END(Vol, st);
}

UFX_STATUS UfxUnlink(PUFX_VOL Vol, ULONG DirIno, PUFX_INODE Dir, const char *Name, ULONG NameLen)
{ return DoUnlink(Vol, DirIno, Dir, Name, NameLen, FALSE); }
UFX_STATUS UfxRemoveEntry(PUFX_VOL Vol, ULONG DirIno, PUFX_INODE Dir, const char *Name, ULONG NameLen)
{ return DoUnlink(Vol, DirIno, Dir, Name, NameLen, TRUE); }

UFX_STATUS
UfxLink(PUFX_VOL Vol, ULONG DirIno, PUFX_INODE Dir, const char *Name, ULONG NameLen, ULONG TargetIno, PUFX_INODE Target)
{
    PUDF_INODE D, T;
    UDF_STATUS st;
    CHANGE_BEGIN(Vol);
    st = Get(Vol, DirIno, &D);
    if (!UDF_SUCCESS(st)) return CHANGE_END(Vol, st);
    st = Get(Vol, TargetIno, &T);
    if (!UDF_SUCCESS(st)) { UdfIput(V2U(Vol), D); return CHANGE_END(Vol, st); }
    st = UdfLink(V2U(Vol), D, Name, NameLen, T);
    UdfIput(V2U(Vol), T);
    UdfIput(V2U(Vol), D);
    if (UDF_SUCCESS(st)) { Refresh(Vol, DirIno, Dir); Refresh(Vol, TargetIno, Target); }
    return CHANGE_END(Vol, st);
}

UFX_STATUS
UfxRename(PUFX_VOL Vol, ULONG OldDir, PUFX_INODE OldDirInode, const char *OldName, ULONG OldLen,
          ULONG NewDir, PUFX_INODE NewDirInode, const char *NewName, ULONG NewLen)
{
    PUDF_INODE O, N;
    UDF_STATUS st;
    CHANGE_BEGIN(Vol);
    st = Get(Vol, OldDir, &O);
    if (!UDF_SUCCESS(st)) return CHANGE_END(Vol, st);
    if (NewDir == OldDir) { N = O; O->Refs++; }
    else {
        st = Get(Vol, NewDir, &N);
        if (!UDF_SUCCESS(st)) { UdfIput(V2U(Vol), O); return CHANGE_END(Vol, st); }
    }
    st = UdfRename(V2U(Vol), O, OldName, OldLen, N, NewName, NewLen);
    if (UDF_SUCCESS(st)) {
        UDF_DIRENT *e = (UDF_DIRENT *)UfxHostAlloc(sizeof(UDF_DIRENT));
        if (e && UDF_SUCCESS(UdfDirLookup(V2U(Vol), N, NewName, NewLen, e))) {
            ULONG ino = MakeIno(Vol, &e->Icb);
            if (ino) NoteName(Vol, ino, NewDir, e);
        }
        if (e) UfxHostFree(e);
    }
    UdfIput(V2U(Vol), N);
    UdfIput(V2U(Vol), O);
    if (UDF_SUCCESS(st)) { Refresh(Vol, OldDir, OldDirInode); if (NewDirInode != OldDirInode) Refresh(Vol, NewDir, NewDirInode); }
    return CHANGE_END(Vol, st);
}

UFX_STATUS
UfxReadLink(PUFX_VOL Vol, ULONG Ino, PUFX_INODE Inode, char *Buf, ULONG BufSize, PULONG Len)
{
    PUDF_INODE I;
    UDF_STATUS st;
    (VOID)Inode;
    st = Get(Vol, Ino, &I);
    if (!UDF_SUCCESS(st)) return st;
    st = UdfReadLink(V2U(Vol), I, Buf, BufSize - 1, Len);
    if (UDF_SUCCESS(st)) Buf[*Len] = 0;
    UdfIput(V2U(Vol), I);
    return st;
}

/* ---- file data ----------------------------------------------------------- */
UFX_STATUS
UfxReadFile(PUFX_VOL Vol, ULONG Ino, PUFX_INODE Inode, ULONGLONG Offset, ULONG Length, PVOID Buffer, PULONG Read)
{
    PUDF_INODE I;
    UDF_STATUS st;
    (VOID)Inode;
    st = Get(Vol, Ino, &I);
    if (!UDF_SUCCESS(st)) return st;
    if (I->FileType == UDF_FT_SYMLINK) {
        /* the link's target text, as the layer reads a link's "data" */
        char *t = (char *)UfxHostAlloc(4096);
        ULONG n = 0;
        if (!t) st = UFX_NO_MEMORY;
        else {
            st = UdfReadLink(V2U(Vol), I, t, 4095, &n);
            *Read = 0;
            if (UDF_SUCCESS(st) && Offset < n) {
                ULONG take = n - (ULONG)Offset < Length ? n - (ULONG)Offset : Length;
                UdfMemCopy(Buffer, t + Offset, take);
                *Read = take;
            }
            UfxHostFree(t);
        }
    } else {
        st = UdfIread(V2U(Vol), I, Offset, Length, Buffer, Read);
    }
    UdfIput(V2U(Vol), I);
    return st;
}

UFX_STATUS
UfxMapFileBlock(PUFX_VOL Vol, ULONG Ino, PUFX_INODE Inode, ULONGLONG FileBlk, PULONGLONG Disk, PULONG RunLen)
{
    PUDF_INODE I;
    UDF_STATUS st;
    UCHAR kind;
    ULONG run;
    (VOID)Inode;
    st = Get(Vol, Ino, &I);
    if (!UDF_SUCCESS(st)) return st;
    st = UdfImap(V2U(Vol), I, FileBlk, Disk, &run, &kind);
    UdfIput(V2U(Vol), I);
    if (!UDF_SUCCESS(st)) return st;
    if (kind == UDF_MAP_INLINE) return UFX_NOT_SUPPORTED;
    if (kind != UDF_MAP_RECORDED) *Disk = 0;
    if (run > 0xFFFFFFF) run = 0xFFFFFFF;
    *RunLen = run ? run : 1;
    return UFX_OK;
}

UFX_STATUS
UfxAllocateRange(PUFX_VOL Vol, ULONG Ino, PUFX_INODE Inode, ULONGLONG FirstBlk, ULONGLONG Count)
{
    PUDF_INODE I;
    UDF_STATUS st;
    CHANGE_BEGIN(Vol);
    st = Get(Vol, Ino, &I);
    if (!UDF_SUCCESS(st)) return CHANGE_END(Vol, st);
    /* blocks larger than a page: the cache writes them a page at a time, the rest must be zeros */
    st = UdfIallocRange(V2U(Vol), I, FirstBlk, Count, (BOOLEAN)(Vol->BlockSize > 4096));
    UdfIput(V2U(Vol), I);
    if (UDF_SUCCESS(st)) Refresh(Vol, Ino, Inode);
    return CHANGE_END(Vol, st);
}

UFX_STATUS
UfxGetFileBlock(PUFX_VOL Vol, ULONG Ino, PUFX_INODE Inode, ULONGLONG FileBlk, PULONGLONG Disk, PBOOLEAN Fresh)
{
    ULONG Run;
    UFX_STATUS st = UfxMapFileBlock(Vol, Ino, Inode, FileBlk, Disk, &Run);
    if (!NT_SUCCESS(st)) return st;
    *Fresh = FALSE;
    if (*Disk) return UFX_OK;
    st = UfxAllocateRange(Vol, Ino, Inode, FileBlk, 1);
    if (!NT_SUCCESS(st)) return st;
    *Fresh = TRUE;
    st = UfxMapFileBlock(Vol, Ino, Inode, FileBlk, Disk, &Run);
    if (NT_SUCCESS(st) && *Disk == 0) st = UFX_CORRUPT;
    return st;
}

UFX_STATUS
UfxUninline(PUFX_VOL Vol, ULONG Ino, PUFX_INODE Inode)
{
    PUDF_INODE I;
    UDF_STATUS st;
    CHANGE_BEGIN(Vol);
    st = Get(Vol, Ino, &I);
    if (!UDF_SUCCESS(st)) return CHANGE_END(Vol, st);
    st = UdfIuninline(V2U(Vol), I);
    UdfIput(V2U(Vol), I);
    if (UDF_SUCCESS(st)) Refresh(Vol, Ino, Inode);
    return CHANGE_END(Vol, st);
}

UFX_STATUS
UfxTruncateBlocks(PUFX_VOL Vol, ULONG Ino, PUFX_INODE Inode, ULONGLONG NewBlocks)
{
    PUDF_INODE I;
    UDF_STATUS st;
    CHANGE_BEGIN(Vol);
    st = Get(Vol, Ino, &I);
    if (!UDF_SUCCESS(st)) return CHANGE_END(Vol, st);
    st = UdfIfreeFrom(V2U(Vol), I, NewBlocks);
    UdfIput(V2U(Vol), I);
    if (UDF_SUCCESS(st)) Refresh(Vol, Ino, Inode);
    return CHANGE_END(Vol, st);
}

ULONG
UfxDataMirrors(PUFX_VOL Vol, ULONGLONG DiskBlock, PULONGLONG Other, ULONG Max)
{
    (VOID)Vol; (VOID)DiskBlock; (VOID)Other; (VOID)Max;
    return 0;
}

/* ---- extended attributes: user.DOSATTRIB only ---------------------------- */
#define DOS_HIDDEN  0x02
#define DOS_SYSTEM  0x04
#define DOS_ARCHIVE 0x20

static BOOLEAN
IsDosAttrib(UCHAR Index, const char *Name, ULONG Len)
{
    return (BOOLEAN)(Index == UFX_XATTR_INDEX_USER && Len == 9 && !UdfMemCmp(Name, "DOSATTRIB", 9));
}

UFX_STATUS
UfxGetXattr(PUFX_VOL Vol, ULONG Ino, PUFX_INODE Inode, UCHAR NameIndex, const char *Name,
            ULONG NameLen, PUCHAR Value, ULONG ValueSize, PULONG ValueLen)
{
    PUDF_INODE I;
    UDF_STATUS st;
    ULONG a = 0;
    UFX_SEEN *s;
    static const char hex[] = "0123456789abcdef";
    (VOID)Inode;
    if (!IsDosAttrib(NameIndex, Name, NameLen)) return UFX_NO_SUCH_EA;
    st = Get(Vol, Ino, &I);
    if (!UDF_SUCCESS(st)) return st;
    if (I->IcbFlags & UDF_ICB_SYSTEM) a |= DOS_SYSTEM;
    if (I->IcbFlags & UDF_ICB_ARCHIVE) a |= DOS_ARCHIVE;
    UdfIput(V2U(Vol), I);
    s = Seen(Vol, Ino, FALSE);
    if (s && s->Hidden) a |= DOS_HIDDEN;
    if (!a) return UFX_NO_SUCH_EA;
    *ValueLen = 4;
    if (ValueSize < 4) return UFX_BUFFER_TOO_SMALL;
    Value[0] = '0'; Value[1] = 'x';
    Value[2] = (UCHAR)hex[(a >> 4) & 15];
    Value[3] = (UCHAR)hex[a & 15];
    return UFX_OK;
}

UFX_STATUS
UfxSetXattr(PUFX_VOL Vol, ULONG Ino, PUFX_INODE Inode, UCHAR NameIndex, const char *Name,
            ULONG NameLen, const UCHAR *Value, ULONG ValueLen)
{
    PUDF_INODE I;
    UDF_STATUS st;
    ULONG a = 0, i;
    UFX_SEEN *s;
    (VOID)Inode;
    if (!IsDosAttrib(NameIndex, Name, NameLen)) return UFX_NOT_SUPPORTED;
    if (Value && ValueLen >= 3 && Value[0] == '0' && (Value[1] == 'x' || Value[1] == 'X')) {
        for (i = 2; i < ValueLen; i++) {
            UCHAR c = Value[i];
            ULONG d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            a = (a << 4) | d;
        }
    }
    {
        CHANGE_BEGIN(Vol);
        st = Get(Vol, Ino, &I);
        if (!UDF_SUCCESS(st)) return CHANGE_END(Vol, st);
        I->IcbFlags &= ~(UDF_ICB_SYSTEM | UDF_ICB_ARCHIVE);
        if (a & DOS_SYSTEM) I->IcbFlags |= UDF_ICB_SYSTEM;
        if (a & DOS_ARCHIVE) I->IcbFlags |= UDF_ICB_ARCHIVE;
        UdfIdirty(V2U(Vol), I);
        UdfIput(V2U(Vol), I);
        s = Seen(Vol, Ino, FALSE);
        if (s && Ino != UFX_ROOT_INO && (BOOLEAN)((a & DOS_HIDDEN) != 0) != (BOOLEAN)s->Hidden) {
            PUDF_INODE D;
            st = Get(Vol, s->DirIno, &D);
            if (UDF_SUCCESS(st)) {
                st = UdfDirSetChars(V2U(Vol), D, s->Offset, (UCHAR)((a & DOS_HIDDEN) ? UDF_FID_HIDDEN : 0),
                                    (UCHAR)((a & DOS_HIDDEN) ? 0 : UDF_FID_HIDDEN));
                UdfIput(V2U(Vol), D);
                if (UDF_SUCCESS(st)) s->Hidden = (UCHAR)((a & DOS_HIDDEN) != 0);
            }
        }
        return CHANGE_END(Vol, st);
    }
}

UFX_STATUS
UfxListXattr(PUFX_VOL Vol, ULONG Ino, PUFX_INODE Inode,
             VOID (*Report)(PVOID Ctx, UCHAR NameIndex, const char *Name, ULONG NameLen,
                            const UCHAR *Value, ULONG ValueLen), PVOID Ctx)
{
    (VOID)Vol; (VOID)Ino; (VOID)Inode; (VOID)Report; (VOID)Ctx;
    return UFX_OK;
}
