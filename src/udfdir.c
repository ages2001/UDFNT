/*
 * UDFNT - directories (file identifier descriptors) and the operations on
 * names: create, link, unlink, rename, symbolic links.
 *
 * A directory's link count is 1 for its own name plus one for each
 * subdirectory (whose parent entry points back at it); deleted entries are
 * marked deleted, and dropped when they end the directory.
 */
#include "udfeng.h"

#define FID_LEN(liu, lfi)   ((UDF_FID_FIXED + (liu) + (lfi) + 3) & ~3UL)

/* ---- reading -------------------------------------------------------------------------- */
static UDF_STATUS
ReadFid(PUDF_VOL V, PUDF_INODE Dir, ULONGLONG Off, PUCHAR Fid, PULONG FidLen)
{
    ULONG got, liu, lfi, len;
    UDF_STATUS st;
    if (Off + UDF_FID_FIXED > Dir->Size) return UDF_NO_MORE;
    st = UdfIread(V, Dir, Off, UDF_FID_FIXED, Fid, &got);
    if (!UDF_SUCCESS(st)) return st;
    if (got < UDF_FID_FIXED || UdfGet16(Fid) != UDF_TAG_FID) return UDF_FILE_CORRUPT;
    lfi = Fid[UDF_FID_LFI];
    liu = UdfGet16(Fid + UDF_FID_LIU);
    len = FID_LEN(liu, lfi);
    if (len > 38 + 65535 + 255 + 3 || Off + UDF_FID_FIXED + liu + lfi > Dir->Size) return UDF_FILE_CORRUPT;
    if (len > UDF_FID_FIXED) {
        ULONG want = len - UDF_FID_FIXED;
        if (Off + len > Dir->Size) want = (ULONG)(Dir->Size - Off - UDF_FID_FIXED);
        st = UdfIread(V, Dir, Off + UDF_FID_FIXED, want, Fid + UDF_FID_FIXED, &got);
        if (!UDF_SUCCESS(st)) return st;
    }
    if (!UdfTagCheck(Fid, len, UDF_TAG_FID, (ULONG)~0UL)) {
        /* some writers record a CRC over the padding too: accept a tag whose checksum holds */
        ULONG i;
        UCHAR s = 0;
        for (i = 0; i < 16; i++) if (i != 4) s = (UCHAR)(s + Fid[i]);
        if (s != Fid[4]) return UDF_FILE_CORRUPT;
    }
    *FidLen = len;
    return UDF_OK;
}

static VOID
FillEnt(PUDF_VOL V, const UCHAR *Fid, ULONGLONG Off, ULONG Len, PUDF_DIRENT Ent)
{
    ULONG l;
    UCHAR lfi = Fid[UDF_FID_LFI];
    USHORT liu = UdfGet16(Fid + UDF_FID_LIU);
    Ent->Offset = Off;
    Ent->FidLen = Len;
    Ent->Chars = Fid[UDF_FID_CHAR];
    UdfLongAdGet(Fid + UDF_FID_ICB, &l, &Ent->Icb);
    Ent->UniqueId32 = UdfGet32(Fid + UDF_FID_ICB + 12);
    Ent->NameLen = (USHORT)UdfCs0ToUtf8(Fid + UDF_FID_FIXED + liu, lfi, Ent->Name, UDF_MAX_NAME_UTF8);
    Ent->Name[Ent->NameLen] = 0;
    (VOID)V;
}

UDF_STATUS
UdfDirNext(PUDF_VOL Vol, PUDF_INODE Dir, ULONGLONG Cookie, PUDF_DIRENT Ent)
{
    UCHAR fid[UDF_FID_FIXED + 512];
    PUCHAR big = NULL;
    UDF_STATUS st;
    if (!UdfIsDir(Dir)) return UDF_NOT_A_DIRECTORY;
    while (Cookie < Dir->Size) {
        ULONG len, liu;
        UCHAR hdr[UDF_FID_FIXED];
        ULONG got;
        st = UdfIread(Vol, Dir, Cookie, UDF_FID_FIXED, hdr, &got);
        if (!UDF_SUCCESS(st)) return st;
        if (got < UDF_FID_FIXED) return UDF_NO_MORE;
        liu = UdfGet16(hdr + UDF_FID_LIU);
        if (liu + 255 + UDF_FID_FIXED + 4 > sizeof(fid)) {
            big = (PUCHAR)UdfHostAlloc(UDF_FID_FIXED + liu + 256 + 4);
            if (!big) return UDF_NO_MEMORY;
        }
        st = ReadFid(Vol, Dir, Cookie, big ? big : fid, &len);
        if (st == UDF_NO_MORE) { if (big) UdfHostFree(big); return st; }
        if (!UDF_SUCCESS(st)) { if (big) UdfHostFree(big); return st; }
        if (!((big ? big : fid)[UDF_FID_CHAR] & UDF_FID_DELETED)) {
            FillEnt(Vol, big ? big : fid, Cookie, len, Ent);
            if (big) UdfHostFree(big);
            return UDF_OK;
        }
        if (big) { UdfHostFree(big); big = NULL; }
        Cookie += len;
    }
    return UDF_NO_MORE;
}

UDF_STATUS
UdfDirLookup(PUDF_VOL Vol, PUDF_INODE Dir, const char *Name, ULONG Len, PUDF_DIRENT Ent)
{
    ULONGLONG cookie = 0;
    UDF_STATUS st;
    for (;;) {
        st = UdfDirNext(Vol, Dir, cookie, Ent);
        if (st == UDF_NO_MORE) return UDF_NOT_FOUND;
        if (!UDF_SUCCESS(st)) return st;
        cookie = Ent->Offset + Ent->FidLen;
        if (Ent->Chars & UDF_FID_PARENT) continue;
        if (Ent->NameLen == Len && !UdfMemCmp(Ent->Name, Name, Len)) return UDF_OK;
    }
}

UDF_STATUS
UdfDirParent(PUDF_VOL Vol, PUDF_INODE Dir, PUDF_LOC Parent)
{
    UDF_DIRENT *e = (UDF_DIRENT *)UdfHostAlloc(sizeof(UDF_DIRENT));
    ULONGLONG cookie = 0;
    UDF_STATUS st;
    if (!e) return UDF_NO_MEMORY;
    for (;;) {
        st = UdfDirNext(Vol, Dir, cookie, e);
        if (!UDF_SUCCESS(st)) break;
        cookie = e->Offset + e->FidLen;
        if (e->Chars & UDF_FID_PARENT) { *Parent = e->Icb; break; }
    }
    UdfHostFree(e);
    if (st == UDF_NO_MORE) { *Parent = Dir->Loc; st = UDF_OK; }   /* no parent entry: the root */
    return st;
}

UDF_STATUS
UdfDirIsEmpty(PUDF_VOL Vol, PUDF_INODE Dir, PBOOLEAN Empty)
{
    UDF_DIRENT *e = (UDF_DIRENT *)UdfHostAlloc(sizeof(UDF_DIRENT));
    ULONGLONG cookie = 0;
    UDF_STATUS st;
    if (!e) return UDF_NO_MEMORY;
    *Empty = TRUE;
    for (;;) {
        st = UdfDirNext(Vol, Dir, cookie, e);
        if (!UDF_SUCCESS(st)) break;
        cookie = e->Offset + e->FidLen;
        if (!(e->Chars & UDF_FID_PARENT)) { *Empty = FALSE; break; }
    }
    UdfHostFree(e);
    return st == UDF_NO_MORE || UDF_SUCCESS(st) ? UDF_OK : st;
}

/* ---- writing ----------------------------------------------------------------------------- */
/* the partition block an offset of a directory lies in (for a tag location) */
static ULONG
BlockOf(PUDF_VOL V, PUDF_INODE Dir, ULONGLONG Off)
{
    ULONG i;
    if (Dir->AdType == UDF_ICB_AD_IN_ICB) return Dir->Loc.Lbn;
    for (i = 0; i < Dir->NExt; i++) {
        PUDF_EXT e = &Dir->Ext[i];
        if (Off >= e->Off && Off < e->Off + e->Len) return e->Lbn + (ULONG)((Off - e->Off) >> V->BlockShift);
    }
    return 0;
}

/* writes a FID image at Off, with its tag made for where it lands */
static UDF_STATUS
PutFid(PUDF_VOL V, PUDF_INODE Dir, ULONGLONG Off, PUCHAR Fid, ULONG Len)
{
    ULONG liu = UdfGet16(Fid + UDF_FID_LIU), lfi = Fid[UDF_FID_LFI];
    UDF_STATUS st;
    /* first the bytes (the directory may move out of its file entry), then the tag for its block */
    st = UdfIwriteData(V, Dir, Off, Len, Fid);
    if (!UDF_SUCCESS(st)) return st;
    UdfTagMake(Fid, UDF_TAG_FID, V->TagVersion, V->TagSerial, BlockOf(V, Dir, Off), FID_LEN(liu, lfi));
    st = UdfIwriteData(V, Dir, Off, UDF_TAG_SIZE, Fid);
    if (UDF_SUCCESS(st)) Dir->DirVersion++;
    return st;
}

static ULONG
MakeFid(PUDF_VOL V, PUCHAR Fid, UCHAR Chars, const UDF_LOC *Icb, ULONG UniqueId32, const UCHAR *Cs0, ULONG Cs0Len)
{
    ULONG len = FID_LEN(0, Cs0Len);
    UdfMemZero(Fid, len);
    UdfPut16(Fid + UDF_FID_VERSION, 1);
    Fid[UDF_FID_CHAR] = Chars;
    Fid[UDF_FID_LFI] = (UCHAR)Cs0Len;
    if (Icb) {
        UdfLongAdPut(Fid + UDF_FID_ICB, V->BlockSize, Icb);
        UdfPut32(Fid + UDF_FID_ICB + 12, UniqueId32);
    }
    UdfPut16(Fid + UDF_FID_LIU, 0);
    if (Cs0Len) UdfMemCopy(Fid + UDF_FID_FIXED, Cs0, Cs0Len);
    return len;
}

/* adds an entry: in a deleted entry of the same size, else at the end */
static UDF_STATUS
AddFid(PUDF_VOL V, PUDF_INODE Dir, PUCHAR Fid, ULONG Len)
{
    ULONGLONG off = 0, at = Dir->Size;
    UCHAR hdr[UDF_FID_FIXED];
    UDF_STATUS st;
    while (off + UDF_FID_FIXED <= Dir->Size) {
        ULONG got, flen;
        st = UdfIread(V, Dir, off, UDF_FID_FIXED, hdr, &got);
        if (!UDF_SUCCESS(st)) return st;
        if (UdfGet16(hdr) != UDF_TAG_FID) return UDF_FILE_CORRUPT;
        flen = FID_LEN(UdfGet16(hdr + UDF_FID_LIU), hdr[UDF_FID_LFI]);
        if ((hdr[UDF_FID_CHAR] & UDF_FID_DELETED) && flen == Len) { at = off; break; }
        off += flen;
    }
    return PutFid(V, Dir, at, Fid, Len);
}

static UDF_STATUS
CheckName(const char *Name, ULONG Len, PUCHAR Cs0, PULONG Cs0Len)
{
    ULONG i;
    if (Len == 0) return UDF_BAD_NAME;
    if ((Len == 1 && Name[0] == '.') || (Len == 2 && Name[0] == '.' && Name[1] == '.')) return UDF_BAD_NAME;
    for (i = 0; i < Len; i++) if (Name[i] == '/' || Name[i] == 0) return UDF_BAD_NAME;
    *Cs0Len = UdfUtf8ToCs0(Name, Len, Cs0, 255);
    if (*Cs0Len == 0) return Len > 127 ? UDF_NAME_TOO_LONG : UDF_BAD_NAME;
    return UDF_OK;
}

/* drops deleted entries that end the directory */
static UDF_STATUS
TrimDir(PUDF_VOL V, PUDF_INODE Dir)
{
    ULONGLONG off = 0, liveEnd = 0;
    UCHAR hdr[UDF_FID_FIXED];
    UDF_STATUS st;
    while (off + UDF_FID_FIXED <= Dir->Size) {
        ULONG got, flen;
        st = UdfIread(V, Dir, off, UDF_FID_FIXED, hdr, &got);
        if (!UDF_SUCCESS(st)) return st;
        if (UdfGet16(hdr) != UDF_TAG_FID) return UDF_OK;
        flen = FID_LEN(UdfGet16(hdr + UDF_FID_LIU), hdr[UDF_FID_LFI]);
        off += flen;
        if (!(hdr[UDF_FID_CHAR] & UDF_FID_DELETED)) liveEnd = off;
    }
    if (liveEnd < Dir->Size && off >= Dir->Size) return UdfIsetSize(V, Dir, liveEnd);
    return UDF_OK;
}

static UDF_STATUS
DeleteFid(PUDF_VOL V, PUDF_INODE Dir, const UDF_DIRENT *E)
{
    PUCHAR fid = (PUCHAR)UdfHostAlloc(E->FidLen + 4);
    ULONG len, liu, lfi;
    UDF_STATUS st;
    if (!fid) return UDF_NO_MEMORY;
    st = ReadFid(V, Dir, E->Offset, fid, &len);
    if (UDF_SUCCESS(st)) {
        liu = UdfGet16(fid + UDF_FID_LIU);
        lfi = fid[UDF_FID_LFI];
        fid[UDF_FID_CHAR] |= UDF_FID_DELETED;
        UdfMemZero(fid + UDF_FID_ICB, UDF_LONG_AD);
        UdfTagMake(fid, UDF_TAG_FID, V->TagVersion, V->TagSerial, BlockOf(V, Dir, E->Offset), FID_LEN(liu, lfi));
        st = UdfIwriteData(V, Dir, E->Offset, len, fid);
        Dir->DirVersion++;
    }
    UdfHostFree(fid);
    if (UDF_SUCCESS(st)) st = TrimDir(V, Dir);
    return st;
}

UDF_STATUS
UdfDirSetChars(PUDF_VOL Vol, PUDF_INODE Dir, ULONGLONG Offset, UCHAR Set, UCHAR Clear)
{
    UCHAR fid[UDF_FID_FIXED + 300];
    ULONG len, liu, lfi;
    UDF_STATUS st;
    if (Vol->ReadOnly) return UDF_READ_ONLY;
    st = ReadFid(Vol, Dir, Offset, fid, &len);
    if (!UDF_SUCCESS(st)) return st;
    liu = UdfGet16(fid + UDF_FID_LIU);
    lfi = fid[UDF_FID_LFI];
    if (liu + lfi + UDF_FID_FIXED > sizeof(fid)) return UDF_NOT_SUPPORTED;
    if (((fid[UDF_FID_CHAR] | Set) & ~Clear) == fid[UDF_FID_CHAR]) return UDF_OK;
    st = UdfChangeBegin(Vol);
    if (!UDF_SUCCESS(st)) return st;
    fid[UDF_FID_CHAR] = (UCHAR)((fid[UDF_FID_CHAR] | Set) & ~Clear);
    UdfTagMake(fid, UDF_TAG_FID, Vol->TagVersion, Vol->TagSerial, BlockOf(Vol, Dir, Offset), FID_LEN(liu, lfi));
    return UdfIwriteData(Vol, Dir, Offset, len, fid);
}

/* ---- symbolic links ------------------------------------------------------------------------------ */
static UDF_STATUS
BuildPath(const char *T, ULONG TLen, PUCHAR Out, ULONG Max, PULONG OutLen)
{
    ULONG i = 0, n = 0;
    if (TLen && T[0] == '/') {
        if (n + UDF_PC_SIZE > Max) return UDF_NAME_TOO_LONG;
        UdfMemZero(Out + n, UDF_PC_SIZE);
        Out[n] = UDF_PC_ROOT;
        n += UDF_PC_SIZE;
        while (i < TLen && T[i] == '/') i++;
    }
    while (i < TLen) {
        ULONG s = i, len, cl;
        while (i < TLen && T[i] != '/') i++;
        len = i - s;
        while (i < TLen && T[i] == '/') i++;
        if (len == 0) continue;
        if (n + UDF_PC_SIZE > Max) return UDF_NAME_TOO_LONG;
        UdfMemZero(Out + n, UDF_PC_SIZE);
        if (len == 1 && T[s] == '.') { Out[n] = UDF_PC_CURRENT; n += UDF_PC_SIZE; continue; }
        if (len == 2 && T[s] == '.' && T[s + 1] == '.') { Out[n] = UDF_PC_PARENT; n += UDF_PC_SIZE; continue; }
        Out[n] = UDF_PC_NAME;
        cl = UdfUtf8ToCs0(T + s, len, Out + n + UDF_PC_SIZE, Max - n - UDF_PC_SIZE < 255 ? Max - n - UDF_PC_SIZE : 255);
        if (cl == 0) return UDF_NAME_TOO_LONG;
        Out[n + 1] = (UCHAR)cl;
        n += UDF_PC_SIZE + cl;
    }
    *OutLen = n;
    return UDF_OK;
}

UDF_STATUS
UdfReadLink(PUDF_VOL Vol, PUDF_INODE I, char *Out, ULONG OutMax, PULONG Len)
{
    PUCHAR d;
    ULONG got, off = 0, n = 0, size;
    UDF_STATUS st;
    if (I->FileType != UDF_FT_SYMLINK) return UDF_NOT_SUPPORTED;
    if (I->Size > 65536) return UDF_FILE_CORRUPT;
    size = (ULONG)I->Size;
    d = (PUCHAR)UdfHostAlloc(size + 1);
    if (!d) return UDF_NO_MEMORY;
    st = UdfIread(Vol, I, 0, size, d, &got);
    if (!UDF_SUCCESS(st)) { UdfHostFree(d); return st; }
    while (off + UDF_PC_SIZE <= got) {
        UCHAR type = d[off], cl = d[off + 1];
        const char *add = NULL;
        ULONG addLen = 0;
        char name[UDF_MAX_NAME_UTF8 + 1];
        if (off + UDF_PC_SIZE + cl > got) break;
        switch (type) {
        case UDF_PC_ROOT_AGREED:
            if (cl) break;
            /* fall through */
        case UDF_PC_ROOT: n = 0; if (n < OutMax) Out[n++] = '/'; off += UDF_PC_SIZE + cl; continue;
        case UDF_PC_PARENT: add = ".."; addLen = 2; break;
        case UDF_PC_CURRENT: add = "."; addLen = 1; break;
        case UDF_PC_NAME:
            addLen = UdfCs0ToUtf8(d + off + UDF_PC_SIZE, cl, name, sizeof(name) - 1);
            add = name;
            break;
        }
        if (add) {
            if (n && Out[n - 1] != '/' && n < OutMax) Out[n++] = '/';
            if (n + addLen > OutMax) addLen = OutMax - n;
            UdfMemCopy(Out + n, add, addLen);
            n += addLen;
        }
        off += UDF_PC_SIZE + cl;
    }
    UdfHostFree(d);
    if (n < OutMax) Out[n] = 0;
    *Len = n;
    return UDF_OK;
}

/* ---- operations -------------------------------------------------------------------------------------- */
UDF_STATUS
UdfCreate(PUDF_VOL Vol, PUDF_INODE Dir, const char *Name, ULONG Len, ULONG Kind, ULONG Mode,
          ULONG Uid, ULONG Gid, const char *Target, ULONG TargetLen, PUDF_INODE *New)
{
    UCHAR cs0[256];
    UCHAR fid[UDF_FID_FIXED + 260];
    ULONG cl, flen;
    UDF_DIRENT *e;
    PUDF_INODE I;
    UDF_STATUS st;
    UCHAR type = Kind == UDF_CREATE_DIR ? UDF_FT_DIRECTORY : Kind == UDF_CREATE_SYMLINK ? UDF_FT_SYMLINK : UDF_FT_FILE;
    if (Vol->ReadOnly) return UDF_READ_ONLY;
    if (!UdfIsDir(Dir)) return UDF_NOT_A_DIRECTORY;
    st = CheckName(Name, Len, cs0, &cl);
    if (!UDF_SUCCESS(st)) return st;
    e = (UDF_DIRENT *)UdfHostAlloc(sizeof(UDF_DIRENT));
    if (!e) return UDF_NO_MEMORY;
    st = UdfDirLookup(Vol, Dir, Name, Len, e);
    UdfHostFree(e);
    if (UDF_SUCCESS(st)) return UDF_EXISTS;
    if (st != UDF_NOT_FOUND) return st;
    if (Kind == UDF_CREATE_DIR && Dir->Links >= 0xFFFF) return UDF_TOO_MANY_LINKS;
    st = UdfChangeBegin(Vol);
    if (!UDF_SUCCESS(st)) return st;
    st = UdfInew(Vol, type, &Dir->Loc, &I);
    if (!UDF_SUCCESS(st)) return st;
    UdfISetMode(I, Mode);
    I->Uid = Uid;
    I->Gid = Gid;
    I->Links = 1;
    if (Kind == UDF_CREATE_DIR) {
        /* its parent entry */
        flen = MakeFid(Vol, fid, UDF_FID_DIRECTORY | UDF_FID_PARENT, &Dir->Loc, (ULONG)Dir->UniqueId, NULL, 0);
        st = PutFid(Vol, I, 0, fid, flen);
    } else if (Kind == UDF_CREATE_SYMLINK) {
        PUCHAR path = (PUCHAR)UdfHostAlloc(4096);
        ULONG plen;
        if (!path) st = UDF_NO_MEMORY;
        else {
            st = BuildPath(Target, TargetLen, path, 4096, &plen);
            if (UDF_SUCCESS(st)) st = UdfIwriteData(Vol, I, 0, plen, path);
            UdfHostFree(path);
        }
    }
    if (UDF_SUCCESS(st)) {
        flen = MakeFid(Vol, fid, (UCHAR)(Kind == UDF_CREATE_DIR ? UDF_FID_DIRECTORY : 0), &I->Loc, (ULONG)I->UniqueId, cs0, cl);
        st = AddFid(Vol, Dir, fid, flen);
    }
    if (!UDF_SUCCESS(st)) {
        (VOID)UdfIdelete(Vol, I);
        UdfIput(Vol, I);
        return st;
    }
    if (Kind == UDF_CREATE_DIR) {
        Dir->Links++;
        Vol->NumDirs++;
    } else {
        Vol->NumFiles++;
    }
    UdfTimeNow(Vol, &Dir->Mtime);
    Dir->Ctime = Dir->Mtime;
    UdfIdirty(Vol, Dir);
    UdfIdirty(Vol, I);
    Vol->ModCount++;
    *New = I;
    return UDF_OK;
}

/* the inode of a removed name: one link less, gone at none (unless kept for open handles) */
static UDF_STATUS
DropLink(PUDF_VOL Vol, PUDF_INODE I, BOOLEAN Keep)
{
    BOOLEAN dir = UdfIsDir(I);
    if (dir) {
        I->Links = 0;
        if (Vol->NumDirs) Vol->NumDirs--;
    } else {
        if (I->Links) I->Links--;
        if (Vol->NumFiles) Vol->NumFiles--;
    }
    UdfTimeNow(Vol, &I->Ctime);
    UdfIdirty(Vol, I);
    if (I->Links == 0 && !Keep) return UdfIdelete(Vol, I);
    return UDF_OK;
}

UDF_STATUS
UdfUnlink(PUDF_VOL Vol, PUDF_INODE Dir, const char *Name, ULONG Len, BOOLEAN KeepInode)
{
    UDF_DIRENT *e;
    PUDF_INODE I;
    UDF_STATUS st;
    if (Vol->ReadOnly) return UDF_READ_ONLY;
    e = (UDF_DIRENT *)UdfHostAlloc(sizeof(UDF_DIRENT));
    if (!e) return UDF_NO_MEMORY;
    st = UdfDirLookup(Vol, Dir, Name, Len, e);
    if (!UDF_SUCCESS(st)) { UdfHostFree(e); return st; }
    st = UdfIget(Vol, &e->Icb, &I);
    if (!UDF_SUCCESS(st)) { UdfHostFree(e); return st; }
    if (UdfIsDir(I)) {
        BOOLEAN empty;
        st = UdfDirIsEmpty(Vol, I, &empty);
        if (UDF_SUCCESS(st) && !empty) st = UDF_NOT_EMPTY;
    }
    if (UDF_SUCCESS(st)) st = UdfChangeBegin(Vol);
    if (UDF_SUCCESS(st)) st = DeleteFid(Vol, Dir, e);
    if (UDF_SUCCESS(st)) {
        if (UdfIsDir(I) && Dir->Links > 1) Dir->Links--;
        UdfTimeNow(Vol, &Dir->Mtime);
        Dir->Ctime = Dir->Mtime;
        UdfIdirty(Vol, Dir);
        st = DropLink(Vol, I, KeepInode);
        Vol->ModCount++;
    }
    UdfIput(Vol, I);
    UdfHostFree(e);
    return st;
}

UDF_STATUS
UdfReleaseOrphan(PUDF_VOL Vol, PUDF_INODE I)
{
    if (I->Links || I->Gone) return UDF_OK;
    if (Vol->ReadOnly) return UDF_READ_ONLY;
    return UdfIdelete(Vol, I);
}

UDF_STATUS
UdfLink(PUDF_VOL Vol, PUDF_INODE Dir, const char *Name, ULONG Len, PUDF_INODE Target)
{
    UCHAR cs0[256], fid[UDF_FID_FIXED + 260];
    ULONG cl, flen;
    UDF_DIRENT *e;
    UDF_STATUS st;
    if (Vol->ReadOnly) return UDF_READ_ONLY;
    if (UdfIsDir(Target)) return UDF_IS_A_DIRECTORY;
    if (Target->Links >= 0xFFFF) return UDF_TOO_MANY_LINKS;
    st = CheckName(Name, Len, cs0, &cl);
    if (!UDF_SUCCESS(st)) return st;
    e = (UDF_DIRENT *)UdfHostAlloc(sizeof(UDF_DIRENT));
    if (!e) return UDF_NO_MEMORY;
    st = UdfDirLookup(Vol, Dir, Name, Len, e);
    UdfHostFree(e);
    if (UDF_SUCCESS(st)) return UDF_EXISTS;
    if (st != UDF_NOT_FOUND) return st;
    st = UdfChangeBegin(Vol);
    if (!UDF_SUCCESS(st)) return st;
    flen = MakeFid(Vol, fid, 0, &Target->Loc, (ULONG)Target->UniqueId, cs0, cl);
    st = AddFid(Vol, Dir, fid, flen);
    if (!UDF_SUCCESS(st)) return st;
    Target->Links++;
    UdfTimeNow(Vol, &Target->Ctime);
    UdfIdirty(Vol, Target);
    Dir->Mtime = Target->Ctime;
    Dir->Ctime = Target->Ctime;
    UdfIdirty(Vol, Dir);
    Vol->NumFiles++;
    Vol->ModCount++;
    return UDF_OK;
}

/* points a directory's parent entry at a new parent */
static UDF_STATUS
SetParentEntry(PUDF_VOL Vol, PUDF_INODE Dir, PUDF_INODE NewParent)
{
    UDF_DIRENT *e = (UDF_DIRENT *)UdfHostAlloc(sizeof(UDF_DIRENT));
    ULONGLONG cookie = 0;
    UDF_STATUS st;
    if (!e) return UDF_NO_MEMORY;
    for (;;) {
        st = UdfDirNext(Vol, Dir, cookie, e);
        if (!UDF_SUCCESS(st)) break;
        cookie = e->Offset + e->FidLen;
        if (e->Chars & UDF_FID_PARENT) {
            UCHAR fid[UDF_FID_FIXED + 300];
            ULONG len;
            st = ReadFid(Vol, Dir, e->Offset, fid, &len);
            if (UDF_SUCCESS(st) && len <= sizeof(fid)) {
                UdfLongAdPut(fid + UDF_FID_ICB, Vol->BlockSize, &NewParent->Loc);
                UdfPut32(fid + UDF_FID_ICB + 12, (ULONG)NewParent->UniqueId);
                st = PutFid(Vol, Dir, e->Offset, fid, len);
            }
            break;
        }
    }
    UdfHostFree(e);
    return st == UDF_NO_MORE ? UDF_OK : st;
}

/* is Dir (a directory) inside Tree? (a directory cannot move into itself) */
static UDF_STATUS
IsInside(PUDF_VOL Vol, PUDF_INODE Dir, PUDF_INODE Tree, PBOOLEAN Inside)
{
    UDF_LOC cur = Dir->Loc, p;
    ULONG depth;
    UDF_STATUS st;
    *Inside = FALSE;
    for (depth = 0; depth < 4096; depth++) {
        PUDF_INODE I;
        if (cur.Lbn == Tree->Loc.Lbn && cur.Part == Tree->Loc.Part) { *Inside = TRUE; return UDF_OK; }
        if (cur.Lbn == Vol->Root.Lbn && cur.Part == Vol->Root.Part) return UDF_OK;
        st = UdfIget(Vol, &cur, &I);
        if (!UDF_SUCCESS(st)) return st;
        st = UdfDirParent(Vol, I, &p);
        UdfIput(Vol, I);
        if (!UDF_SUCCESS(st)) return st;
        if (p.Lbn == cur.Lbn && p.Part == cur.Part) return UDF_OK;
        cur = p;
    }
    return UDF_FILE_CORRUPT;
}

UDF_STATUS
UdfRename(PUDF_VOL Vol, PUDF_INODE OldDir, const char *OldName, ULONG OldLen,
          PUDF_INODE NewDir, const char *NewName, ULONG NewLen)
{
    UCHAR cs0[256], fid[UDF_FID_FIXED + 260];
    ULONG cl, flen;
    UDF_DIRENT *e, *t;
    PUDF_INODE I;
    UDF_STATUS st;
    BOOLEAN isDir, moved = (BOOLEAN)(OldDir != NewDir);
    if (Vol->ReadOnly) return UDF_READ_ONLY;
    st = CheckName(NewName, NewLen, cs0, &cl);
    if (!UDF_SUCCESS(st)) return st;
    e = (UDF_DIRENT *)UdfHostAlloc(2 * sizeof(UDF_DIRENT));
    if (!e) return UDF_NO_MEMORY;
    t = e + 1;
    st = UdfDirLookup(Vol, OldDir, OldName, OldLen, e);
    if (!UDF_SUCCESS(st)) { UdfHostFree(e); return st; }
    if (!moved && OldLen == NewLen && !UdfMemCmp(OldName, NewName, NewLen)) { UdfHostFree(e); return UDF_OK; }
    st = UdfDirLookup(Vol, NewDir, NewName, NewLen, t);
    if (UDF_SUCCESS(st)) {
        /* only a change of case of the same entry may land on an existing name */
        if (moved || t->Offset != e->Offset) { UdfHostFree(e); return UDF_EXISTS; }
    } else if (st != UDF_NOT_FOUND) {
        UdfHostFree(e);
        return st;
    }
    st = UdfIget(Vol, &e->Icb, &I);
    if (!UDF_SUCCESS(st)) { UdfHostFree(e); return st; }
    isDir = UdfIsDir(I);
    if (isDir && moved) {
        BOOLEAN inside;
        st = IsInside(Vol, NewDir, I, &inside);
        if (UDF_SUCCESS(st) && inside) st = UDF_NOT_SUPPORTED;
        if (UDF_SUCCESS(st) && NewDir->Links >= 0xFFFF) st = UDF_TOO_MANY_LINKS;
    }
    if (UDF_SUCCESS(st)) st = UdfChangeBegin(Vol);
    if (UDF_SUCCESS(st)) {
        flen = MakeFid(Vol, fid, (UCHAR)(e->Chars & (UDF_FID_DIRECTORY | UDF_FID_HIDDEN)), &I->Loc, (ULONG)I->UniqueId, cs0, cl);
        st = AddFid(Vol, NewDir, fid, flen);
    }
    if (UDF_SUCCESS(st)) {
        /* the old entry (the directory may have changed; look it up again) */
        st = UdfDirLookup(Vol, OldDir, OldName, OldLen, e);
        if (UDF_SUCCESS(st) && !moved && e->Offset == t->Offset && t->NameLen) {
            /* never happens: a new entry was added elsewhere */
        }
        if (UDF_SUCCESS(st)) st = DeleteFid(Vol, OldDir, e);
    }
    if (UDF_SUCCESS(st) && isDir && moved) {
        st = SetParentEntry(Vol, I, NewDir);
        if (UDF_SUCCESS(st)) {
            if (OldDir->Links > 1) OldDir->Links--;
            NewDir->Links++;
        }
    }
    if (UDF_SUCCESS(st)) {
        UDF_TIME now;
        UdfTimeNow(Vol, &now);
        OldDir->Mtime = OldDir->Ctime = now;
        NewDir->Mtime = NewDir->Ctime = now;
        I->Ctime = now;
        UdfIdirty(Vol, OldDir);
        UdfIdirty(Vol, NewDir);
        UdfIdirty(Vol, I);
        Vol->ModCount++;
    }
    UdfIput(Vol, I);
    UdfHostFree(e);
    return st;
}
