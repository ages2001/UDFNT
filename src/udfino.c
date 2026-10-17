/*
 * UDFNT - file entries: reading (file entries, extended file entries,
 * indirect entries), the allocation descriptors (short, long, extended,
 * embedded, chains of allocation extent descriptors), file data, allocation
 * and writing them back.
 *
 * In memory a file's extents cover it from offset 0 without gaps (holes are
 * sparse extents) in whole blocks; they are written back with the last one
 * cut to the information length, and anything past the end of the file as
 * "allocated, not recorded".
 */
#include "udfeng.h"

#define IHASH(l)    ((ULONG)((l).Lbn ^ ((ULONG)(l).Part << 20) ^ ((l).Lbn >> 9)) & (UDF_INODE_HASH - 1))

static ULONG HdrSize(PUDF_INODE I) { return I->Extended ? UDF_EFE_EA : UDF_FE_EA; }
static ULONG AdSize(UCHAR T) { return T == UDF_ICB_AD_SHORT ? UDF_SHORT_AD : T == UDF_ICB_AD_LONG ? UDF_LONG_AD : UDF_EXT_AD; }

BOOLEAN
UdfIsDir(PUDF_INODE I)
{
    return (BOOLEAN)(I->FileType == UDF_FT_DIRECTORY || I->FileType == UDF_FT_STREAMDIR);
}

/* metadata-like files go through the block cache; file data goes straight to the disk */
static BOOLEAN
Cached(PUDF_INODE I)
{
    return (BOOLEAN)(UdfIsDir(I) || I->FileType == UDF_FT_METABITMAP || I->FileType == UDF_FT_VAT20 ||
                     I->FileType == UDF_FT_METADATA || I->FileType == UDF_FT_METAMIRROR);
}

/* ---- the cache ------------------------------------------------------------------- */
static VOID
LruRemove(PUDF_VOL V, PUDF_INODE I)
{
    if (!I->LruPrev && V->InoLruHead != I) return;
    if (I->LruPrev) I->LruPrev->LruNext = I->LruNext; else V->InoLruHead = I->LruNext;
    if (I->LruNext) I->LruNext->LruPrev = I->LruPrev; else V->InoLruTail = I->LruPrev;
    I->LruPrev = I->LruNext = NULL;
}

static VOID
LruAdd(PUDF_VOL V, PUDF_INODE I)
{
    I->LruPrev = NULL;
    I->LruNext = V->InoLruHead;
    if (V->InoLruHead) V->InoLruHead->LruPrev = I; else V->InoLruTail = I;
    V->InoLruHead = I;
}

static VOID
FreeInode(PUDF_VOL V, PUDF_INODE I)
{
    PUDF_INODE *pp = &V->InoHash[IHASH(I->Loc)];
    while (*pp && *pp != I) pp = &(*pp)->HashNext;
    if (*pp) *pp = I->HashNext;
    LruRemove(V, I);
    if (I->Ea) UdfHostFree(I->Ea);
    if (I->Inline) UdfHostFree(I->Inline);
    if (I->Ext) UdfHostFree(I->Ext);
    if (I->Aed) UdfHostFree(I->Aed);
    UdfHostFree(I);
    V->InoCount--;
}

static VOID
TrimInodes(PUDF_VOL V)
{
    while (V->InoCount > V->InoMax && V->InoLruTail) FreeInode(V, V->InoLruTail);
}

/* ---- extents in memory ------------------------------------------------------------- */
static BOOLEAN
ExtReserve(PUDF_INODE I, ULONG N)
{
    PUDF_EXT n;
    ULONG cap;
    if (I->CapExt >= N) return TRUE;
    cap = I->CapExt ? I->CapExt * 2 : 8;
    while (cap < N) cap *= 2;
    n = (PUDF_EXT)UdfHostAlloc(cap * sizeof(UDF_EXT));
    if (!n) return FALSE;
    if (I->Ext) { UdfMemCopy(n, I->Ext, I->NExt * sizeof(UDF_EXT)); UdfHostFree(I->Ext); }
    I->Ext = n;
    I->CapExt = cap;
    return TRUE;
}

static ULONGLONG
ExtEnd(PUDF_INODE I)
{
    return I->NExt ? I->Ext[I->NExt - 1].Off + I->Ext[I->NExt - 1].Len : 0;
}

/* appends an extent (merging with the last one when it continues it) */
static BOOLEAN
ExtPush(PUDF_VOL V, PUDF_INODE I, UCHAR Type, USHORT Part, ULONG Lbn, ULONG Len)
{
    PUDF_EXT e;
    if (Len == 0) return TRUE;
    if (I->NExt) {
        e = &I->Ext[I->NExt - 1];
        if (e->Type == Type && (ULONGLONG)e->Len + Len <= V->ExtMax && !(e->Len & (V->BlockSize - 1)) &&
            (Type == UDF_EXT_SPARSE || (e->Part == Part && e->Lbn + (e->Len >> V->BlockShift) == Lbn))) {
            e->Len += Len;
            return TRUE;
        }
    }
    if (!ExtReserve(I, I->NExt + 1)) return FALSE;
    e = &I->Ext[I->NExt++];
    e->Off = I->NExt > 1 ? I->Ext[I->NExt - 2].Off + I->Ext[I->NExt - 2].Len : 0;
    e->Len = Len;
    e->Lbn = Type == UDF_EXT_SPARSE ? 0 : Lbn;
    e->Part = Part;
    e->Type = Type;
    e->Pad = 0;
    return TRUE;
}

/* the extent holding file offset Off (binary search), or NExt */
static ULONG
ExtFind(PUDF_INODE I, ULONGLONG Off)
{
    ULONG lo = 0, hi = I->NExt;
    while (lo < hi) {
        ULONG mid = (lo + hi) / 2;
        if (Off < I->Ext[mid].Off) hi = mid;
        else if (Off >= I->Ext[mid].Off + I->Ext[mid].Len) lo = mid + 1;
        else return mid;
    }
    return I->NExt;
}

/* splits so that an extent starts at block Blk (if inside one); returns its index */
static ULONG
ExtSplit(PUDF_VOL V, PUDF_INODE I, ULONGLONG Blk)
{
    ULONGLONG off = Blk << V->BlockShift;
    ULONG i = ExtFind(I, off), cut;
    PUDF_EXT e;
    if (i == I->NExt) return i;
    e = &I->Ext[i];
    if (e->Off == off) return i;
    if (!ExtReserve(I, I->NExt + 1)) return (ULONG)-1;
    e = &I->Ext[i];
    UdfMemMove(&I->Ext[i + 2], &I->Ext[i + 1], (I->NExt - i - 1) * sizeof(UDF_EXT));
    I->NExt++;
    cut = (ULONG)(off - e->Off);
    I->Ext[i + 1] = *e;
    I->Ext[i + 1].Off = off;
    I->Ext[i + 1].Len = e->Len - cut;
    if (e->Type != UDF_EXT_SPARSE) I->Ext[i + 1].Lbn = e->Lbn + (cut >> V->BlockShift);
    e->Len = cut;
    return i + 1;
}

/* merges neighbours that continue each other */
static VOID
ExtTidy(PUDF_VOL V, PUDF_INODE I)
{
    ULONG r, w = 0;
    for (r = 0; r < I->NExt; r++) {
        PUDF_EXT e = &I->Ext[r];
        if (e->Len == 0) continue;
        if (w) {
            PUDF_EXT p = &I->Ext[w - 1];
            if (p->Type == e->Type && (ULONGLONG)p->Len + e->Len <= V->ExtMax && !(p->Len & (V->BlockSize - 1)) &&
                (e->Type == UDF_EXT_SPARSE || (p->Part == e->Part && p->Lbn + (p->Len >> V->BlockShift) == e->Lbn))) {
                p->Len += e->Len;
                continue;
            }
        }
        I->Ext[w] = *e;
        I->Ext[w].Off = w ? I->Ext[w - 1].Off + I->Ext[w - 1].Len : 0;
        w++;
    }
    I->NExt = w;
}

/* ---- reading a file entry ---------------------------------------------------------- */
static UDF_STATUS
AddAed(PUDF_INODE I, const UDF_LOC *L)
{
    if (I->NAed == I->CapAed) {
        ULONG cap = I->CapAed ? I->CapAed * 2 : 4;
        PUDF_LOC n = (PUDF_LOC)UdfHostAlloc(cap * sizeof(UDF_LOC));
        if (!n) return UDF_NO_MEMORY;
        if (I->Aed) { UdfMemCopy(n, I->Aed, I->NAed * sizeof(UDF_LOC)); UdfHostFree(I->Aed); }
        I->Aed = n;
        I->CapAed = cap;
    }
    I->Aed[I->NAed++] = *L;
    return UDF_OK;
}

/* the allocation descriptors in Ads[0..Len) (of the file entry or an extent of them) */
static UDF_STATUS
ParseAds(PUDF_VOL V, PUDF_INODE I, const UCHAR *Ads, ULONG Len, USHORT DefPart, PUDF_LOC Next, PBOOLEAN More)
{
    ULONG sz = AdSize(I->AdType), off;
    *More = FALSE;
    for (off = 0; off + sz <= Len; off += sz) {
        const UCHAR *a = Ads + off;
        ULONG el = UdfGet32(a), len = el & UDF_EXT_LEN_MASK;
        UCHAR type = (UCHAR)(el >> 30);
        UDF_LOC loc;
        if (len == 0) break;
        loc.Part = DefPart;
        if (I->AdType == UDF_ICB_AD_SHORT) {
            loc.Lbn = UdfGet32(a + 4);
        } else if (I->AdType == UDF_ICB_AD_LONG) {
            loc.Lbn = UdfGet32(a + 4);
            loc.Part = UdfGet16(a + 8);
        } else {
            loc.Lbn = UdfGet32(a + 12);
            loc.Part = UdfGet16(a + 16);
        }
        if (type == UDF_EXT_NEXT) {
            *Next = loc;
            *More = TRUE;
            return UDF_OK;
        }
        if (type != UDF_EXT_SPARSE && loc.Part >= V->NMaps) return UDF_FILE_CORRUPT;
        /* round each extent to whole blocks (only the last can be short on the disk) */
        {
            ULONG rounded = (len + V->BlockSize - 1) & ~(V->BlockSize - 1);
            ULONGLONG at = ExtEnd(I);
            if (!ExtPush(V, I, type, loc.Part, loc.Lbn, rounded)) return UDF_NO_MEMORY;
            (VOID)at;
        }
    }
    return UDF_OK;
}

static UDF_STATUS
Parse(PUDF_VOL V, PUDF_INODE I, const UCHAR *D)
{
    const UCHAR *icb = D + UDF_ICBTAG;
    ULONG hdr, lea, lad, B = V->BlockSize;
    UDF_STATUS st;
    I->Extended = (BOOLEAN)(UdfGet16(D) == UDF_TAG_EFE);
    UdfMemCopy(I->IcbTag, icb, UDF_ICBTAG_SIZE);
    I->FileType = icb[UDF_ICBTAG_FILETYPE];
    I->IcbFlags = UdfGet16(icb + UDF_ICBTAG_FLAGS);
    I->AdType = (UCHAR)(I->IcbFlags & UDF_ICB_AD_MASK);
    I->Uid = UdfGet32(D + UDF_FE_UID);
    I->Gid = UdfGet32(D + UDF_FE_GID);
    I->Perm = UdfGet32(D + UDF_FE_PERM);
    I->Links = UdfGet16(D + UDF_FE_LINKS);
    I->RecFmt = D[UDF_FE_RECFMT];
    I->RecAttr = D[UDF_FE_RECATTR];
    I->RecLen = UdfGet32(D + UDF_FE_RECLEN);
    I->Size = UdfGet64(D + UDF_FE_INFOLEN);
    if (I->Extended) {
        I->ObjSize = UdfGet64(D + UDF_EFE_OBJSIZE);
        UdfTimeGet(D + UDF_EFE_ATIME, &I->Atime);
        UdfTimeGet(D + UDF_EFE_MTIME, &I->Mtime);
        UdfTimeGet(D + UDF_EFE_CTIME, &I->Btime);
        UdfTimeGet(D + UDF_EFE_ATTRTIME, &I->Ctime);
        I->Checkpoint = UdfGet32(D + UDF_EFE_CHECKPOINT);
        UdfMemCopy(I->EaIcb, D + UDF_EFE_EAICB, UDF_LONG_AD);
        UdfMemCopy(I->StreamIcb, D + UDF_EFE_STREAMICB, UDF_LONG_AD);
        UdfMemCopy(I->Impl, D + UDF_EFE_IMPL, 32);
        I->UniqueId = UdfGet64(D + UDF_EFE_UNIQUEID);
        lea = UdfGet32(D + UDF_EFE_LEA);
        lad = UdfGet32(D + UDF_EFE_LAD);
        hdr = UDF_EFE_EA;
    } else {
        I->ObjSize = I->Size;
        UdfTimeGet(D + UDF_FE_ATIME, &I->Atime);
        UdfTimeGet(D + UDF_FE_MTIME, &I->Mtime);
        UdfTimeGet(D + UDF_FE_ATTRTIME, &I->Ctime);
        I->Btime = I->Mtime;
        if (I->Ctime.Nt && I->Ctime.Nt < I->Btime.Nt) I->Btime = I->Ctime;
        I->Checkpoint = UdfGet32(D + UDF_FE_CHECKPOINT);
        UdfMemCopy(I->EaIcb, D + UDF_FE_EAICB, UDF_LONG_AD);
        UdfMemZero(I->StreamIcb, UDF_LONG_AD);
        UdfMemCopy(I->Impl, D + UDF_FE_IMPL, 32);
        I->UniqueId = UdfGet64(D + UDF_FE_UNIQUEID);
        lea = UdfGet32(D + UDF_FE_LEA);
        lad = UdfGet32(D + UDF_FE_LAD);
        hdr = UDF_FE_EA;
    }
    if (lea > B - hdr || lad > B - hdr - lea) return UDF_FILE_CORRUPT;
    if (lea) {
        I->Ea = (PUCHAR)UdfHostAlloc(lea);
        if (!I->Ea) return UDF_NO_MEMORY;
        UdfMemCopy(I->Ea, D + hdr, lea);
        I->EaLen = lea;
    }
    if (I->AdType == UDF_ICB_AD_IN_ICB) {
        I->InlineCap = B;
        I->Inline = (PUCHAR)UdfHostAlloc(B);
        if (!I->Inline) return UDF_NO_MEMORY;
        UdfMemZero(I->Inline, B);
        UdfMemCopy(I->Inline, D + hdr + lea, lad);
        if (I->Size > lad) I->Size = lad;
        return UDF_OK;
    }
    if (I->AdType > UDF_ICB_AD_EXT) return UDF_FILE_CORRUPT;
    {
        UDF_LOC next;
        BOOLEAN more;
        ULONG hops = 0;
        st = ParseAds(V, I, D + hdr + lea, lad, I->Loc.Part, &next, &more);
        while (UDF_SUCCESS(st) && more) {
            PUDF_BUF b;
            ULONG alen;
            if (++hops > 65536) return UDF_FILE_CORRUPT;
            st = AddAed(I, &next);
            if (!UDF_SUCCESS(st)) return st;
            st = UdfBufGet(V, next.Part, next.Lbn, TRUE, &b);
            if (!UDF_SUCCESS(st)) return st;
            if (!UdfTagCheck(b->Data, B, UDF_TAG_AED, next.Lbn)) {
                UdfBufPut(V, b);
                return UDF_FILE_CORRUPT;
            }
            alen = UdfGet32(b->Data + UDF_AED_LAD);
            if (alen > B - UDF_AED_ADS) alen = B - UDF_AED_ADS;
            {
                USHORT p = next.Part;
                st = ParseAds(V, I, b->Data + UDF_AED_ADS, alen, p, &next, &more);
            }
            UdfBufPut(V, b);
        }
    }
    return st;
}

UDF_STATUS
UdfIget(PUDF_VOL Vol, const UDF_LOC *Loc, PUDF_INODE *Out)
{
    PUDF_INODE I;
    PUDF_BUF b;
    UDF_STATUS st;
    UDF_LOC loc = *Loc;
    ULONG depth;
    for (I = Vol->InoHash[IHASH(loc)]; I; I = I->HashNext) {
        if (I->Loc.Lbn == loc.Lbn && I->Loc.Part == loc.Part && !I->Gone) {
            if (I->Refs++ == 0) LruRemove(Vol, I);
            *Out = I;
            return UDF_OK;
        }
    }
    I = (PUDF_INODE)UdfHostAlloc(sizeof(UDF_INODE));
    if (!I) return UDF_NO_MEMORY;
    UdfMemZero(I, sizeof(*I));
    /* indirect entries (strategy 4096 ICBs) lead to the file entry in use */
    for (depth = 0; ; depth++) {
        USHORT id;
        st = UdfBufGet(Vol, loc.Part, loc.Lbn, TRUE, &b);
        if (!UDF_SUCCESS(st)) { UdfHostFree(I); return st; }
        id = UdfGet16(b->Data);
        if (id == UDF_TAG_IE && depth < 32 && UdfTagCheck(b->Data, Vol->BlockSize, UDF_TAG_IE, loc.Lbn)) {
            ULONG len;
            UdfLongAdGet(b->Data + UDF_ICBTAG + UDF_ICBTAG_SIZE, &len, &loc);
            UdfBufPut(Vol, b);
            continue;
        }
        if ((id != UDF_TAG_FE && id != UDF_TAG_EFE) || !UdfTagCheck(b->Data, Vol->BlockSize, id, loc.Lbn)) {
            UdfBufPut(Vol, b);
            UdfHostFree(I);
            return UDF_FILE_CORRUPT;
        }
        /* strategy 4096: a newer entry may follow in the next block */
        if (UdfGet16(b->Data + UDF_ICBTAG + UDF_ICBTAG_STRATEGY) == 4096 && depth < 32) {
            PUDF_BUF n;
            if (UDF_SUCCESS(UdfBufGet(Vol, loc.Part, loc.Lbn + 1, TRUE, &n))) {
                if (UdfTagCheck(n->Data, Vol->BlockSize, UDF_TAG_IE, loc.Lbn + 1)) {
                    ULONG len;
                    UDF_LOC nl;
                    UdfLongAdGet(n->Data + UDF_ICBTAG + UDF_ICBTAG_SIZE, &len, &nl);
                    if ((len & UDF_EXT_LEN_MASK) && (nl.Lbn != loc.Lbn || nl.Part != loc.Part)) {
                        UdfBufPut(Vol, n);
                        UdfBufPut(Vol, b);
                        loc = nl;
                        continue;
                    }
                }
                UdfBufPut(Vol, n);
            }
        }
        break;
    }
    I->Loc = loc;
    st = Parse(Vol, I, b->Data);
    UdfBufPut(Vol, b);
    if (!UDF_SUCCESS(st)) {
        if (I->Ea) UdfHostFree(I->Ea);
        if (I->Inline) UdfHostFree(I->Inline);
        if (I->Ext) UdfHostFree(I->Ext);
        if (I->Aed) UdfHostFree(I->Aed);
        UdfHostFree(I);
        return st;
    }
    /* the location asked for is the inode's name, even when an indirect entry led elsewhere */
    I->Loc = loc;
    I->Refs = 1;
    I->HashNext = Vol->InoHash[IHASH(I->Loc)];
    Vol->InoHash[IHASH(I->Loc)] = I;
    Vol->InoCount++;
    TrimInodes(Vol);
    *Out = I;
    return UDF_OK;
}

VOID
UdfIput(PUDF_VOL Vol, PUDF_INODE I)
{
    if (!I || !I->Refs) return;
    if (--I->Refs) return;
    if (I->Gone) { FreeInode(Vol, I); return; }
    if (!I->Dirty) {
        LruAdd(Vol, I);
        TrimInodes(Vol);
    }
}

VOID
UdfIdirty(PUDF_VOL Vol, PUDF_INODE I)
{
    I->Dirty = TRUE;
    Vol->Changed = TRUE;
}

ULONGLONG
UdfIblocks(PUDF_VOL Vol, PUDF_INODE I)
{
    ULONGLONG n = 0;
    ULONG i;
    for (i = 0; i < I->NExt; i++)
        if (I->Ext[i].Type != UDF_EXT_SPARSE) n += I->Ext[i].Len >> Vol->BlockShift;
    return n;
}

/* ---- mode bits ------------------------------------------------------------------------- */
ULONG
UdfIMode(PUDF_INODE I)
{
    ULONG p = I->Perm, m;
    m = (p & 7) | ((p >> 2) & 0070) | ((p >> 4) & 0700);
    if (I->IcbFlags & UDF_ICB_SETUID) m |= 04000;
    if (I->IcbFlags & UDF_ICB_SETGID) m |= 02000;
    if (I->IcbFlags & UDF_ICB_STICKY) m |= 01000;
    switch (I->FileType) {
    case UDF_FT_DIRECTORY: case UDF_FT_STREAMDIR: m |= 0040000; break;
    case UDF_FT_SYMLINK: m |= 0120000; break;
    case UDF_FT_BLOCKDEV: m |= 0060000; break;
    case UDF_FT_CHARDEV: m |= 0020000; break;
    case UDF_FT_FIFO: m |= 0010000; break;
    case UDF_FT_SOCKET: m |= 0140000; break;
    default: m |= 0100000; break;
    }
    return m;
}

VOID
UdfISetMode(PUDF_INODE I, ULONG Mode)
{
    ULONG keep = I->Perm & ((UDF_PERM_CHATTR | UDF_PERM_DELETE) * 0x421);
    I->Perm = keep | (Mode & 7) | ((Mode & 0070) << 2) | ((Mode & 0700) << 4);
    I->IcbFlags &= ~(UDF_ICB_SETUID | UDF_ICB_SETGID | UDF_ICB_STICKY);
    if (Mode & 04000) I->IcbFlags |= UDF_ICB_SETUID;
    if (Mode & 02000) I->IcbFlags |= UDF_ICB_SETGID;
    if (Mode & 01000) I->IcbFlags |= UDF_ICB_STICKY;
}

/* ---- mapping and reading ------------------------------------------------------------- */
UDF_STATUS
UdfImap(PUDF_VOL Vol, PUDF_INODE I, ULONGLONG FileBlk, PULONGLONG Abs, PULONG Run, PUCHAR Kind)
{
    ULONGLONG off = FileBlk << Vol->BlockShift;
    ULONG i, run, trun;
    PUDF_EXT e;
    UDF_STATUS st;
    *Abs = 0;
    if (I->AdType == UDF_ICB_AD_IN_ICB) { *Kind = UDF_MAP_INLINE; *Run = 1; return UDF_OK; }
    i = ExtFind(I, off);
    if (i == I->NExt) {
        *Kind = UDF_MAP_HOLE;
        *Run = 0x10000000;
        return UDF_OK;
    }
    e = &I->Ext[i];
    run = (ULONG)((e->Off + e->Len - off + Vol->BlockSize - 1) >> Vol->BlockShift);
    if (e->Type == UDF_EXT_SPARSE) {
        *Kind = UDF_MAP_HOLE;
        *Run = run;
        return UDF_OK;
    }
    st = UdfTranslate(Vol, e->Part, e->Lbn + (ULONG)((off - e->Off) >> Vol->BlockShift), Abs, &trun, NULL);
    if (!UDF_SUCCESS(st)) return st;
    if (trun < run) run = trun;
    *Run = run ? run : 1;
    *Kind = e->Type == UDF_EXT_RECORDED ? UDF_MAP_RECORDED : UDF_MAP_UNRECORDED;
    return UDF_OK;
}

UDF_STATUS
UdfIread(PUDF_VOL Vol, PUDF_INODE I, ULONGLONG Off, ULONG Len, PVOID Buf, PULONG Got)
{
    PUCHAR out = (PUCHAR)Buf;
    ULONG done = 0;
    UDF_STATUS st;
    *Got = 0;
    if (Off >= I->Size) return UDF_OK;
    if (Len > I->Size - Off) Len = (ULONG)(I->Size - Off);
    if (I->AdType == UDF_ICB_AD_IN_ICB) {
        UdfMemCopy(out, I->Inline + Off, Len);
        *Got = Len;
        return UDF_OK;
    }
    while (done < Len) {
        ULONGLONG pos = Off + done, blk = pos >> Vol->BlockShift, abs;
        ULONG within = (ULONG)(pos & (Vol->BlockSize - 1)), run, take;
        UCHAR kind;
        st = UdfImap(Vol, I, blk, &abs, &run, &kind);
        if (!UDF_SUCCESS(st)) return st;
        if (kind != UDF_MAP_RECORDED) {
            ULONGLONG span = ((ULONGLONG)run << Vol->BlockShift) - within;
            take = (span > Len - done) ? Len - done : (ULONG)span;
            UdfMemZero(out + done, take);
        } else if (Cached(I) || within || Len - done < Vol->BlockSize) {
            PUDF_BUF b;
            take = Vol->BlockSize - within;
            if (take > Len - done) take = Len - done;
            if (Cached(I)) {
                st = UdfBufGetAbs(Vol, abs, TRUE, &b);
                if (!UDF_SUCCESS(st)) return st;
                UdfMemCopy(out + done, b->Data + within, take);
                UdfBufPut(Vol, b);
            } else {
                st = UdfReadAbs(Vol, abs, 1, Vol->Scratch);
                if (!UDF_SUCCESS(st)) return st;
                UdfMemCopy(out + done, Vol->Scratch + within, take);
            }
        } else {
            ULONG blocks = (Len - done) >> Vol->BlockShift;
            if (blocks > run) blocks = run;
            if (blocks > 256) blocks = 256;
            st = UdfReadAbs(Vol, abs, blocks, out + done);
            if (!UDF_SUCCESS(st)) return st;
            take = blocks << Vol->BlockShift;
        }
        done += take;
    }
    *Got = done;
    return UDF_OK;
}

/* ---- allocation ----------------------------------------------------------------------------- */
static USHORT
PartFor(PUDF_VOL V, PUDF_INODE I)
{
    /* the metadata partition's own files live in the physical partition below it */
    if (I->FileType == UDF_FT_METABITMAP || I->FileType == UDF_FT_METADATA ||
        I->FileType == UDF_FT_METAMIRROR || I->FileType == UDF_FT_VAT20) return I->Loc.Part;
    if (UdfIsDir(I)) return V->IcbPart;
    return V->DataPart;
}

/* the AD type an inode must use for data in partition Part */
static VOID
NeedAdTypeFor(PUDF_INODE I, USHORT Part)
{
    if (I->AdType == UDF_ICB_AD_EXT) I->AdType = UDF_ICB_AD_LONG;
    if (I->AdType == UDF_ICB_AD_SHORT && Part != I->Loc.Part) I->AdType = UDF_ICB_AD_LONG;
}

static UDF_STATUS
ZeroBlocks(PUDF_VOL V, USHORT Part, ULONG Lbn, ULONG Count, BOOLEAN ViaCache)
{
    UDF_STATUS st;
    ULONG i;
    for (i = 0; i < Count; i++) {
        if (ViaCache) {
            PUDF_BUF b;
            st = UdfBufGet(V, Part, Lbn + i, FALSE, &b);
            if (!UDF_SUCCESS(st)) return st;
            UdfMemZero(b->Data, V->BlockSize);
            UdfBufDirty(V, b);
            UdfBufPut(V, b);
        } else {
            ULONGLONG abs;
            ULONG run;
            st = UdfTranslate(V, Part, Lbn + i, &abs, &run, NULL);
            if (!UDF_SUCCESS(st)) return st;
            st = UdfWriteAbs(V, abs, 1, V->Zero);
            if (!UDF_SUCCESS(st)) return st;
        }
    }
    return UDF_OK;
}

/* the goal for new blocks of a file: after its blocks before FileBlk */
static ULONG
Goal(PUDF_VOL V, PUDF_INODE I, ULONGLONG FileBlk, USHORT Part)
{
    ULONG i = I->NExt;
    while (i--) {
        PUDF_EXT e = &I->Ext[i];
        if (e->Type != UDF_EXT_SPARSE && e->Part == Part && (e->Off >> V->BlockShift) < FileBlk)
            return e->Lbn + (ULONG)((e->Len) >> V->BlockShift);
    }
    if (I->Loc.Part == Part) return I->Loc.Lbn + 1;
    return 0;
}

UDF_STATUS
UdfIallocRange(PUDF_VOL Vol, PUDF_INODE I, ULONGLONG FirstBlk, ULONGLONG Count, BOOLEAN Zero)
{
    ULONGLONG blk = FirstBlk, end = FirstBlk + Count;
    USHORT part = PartFor(Vol, I);
    UDF_STATUS st;
    BOOLEAN viaCache = Cached(I);
    if (Vol->ReadOnly) return UDF_READ_ONLY;
    if (I->AdType == UDF_ICB_AD_IN_ICB) {
        st = UdfIuninline(Vol, I);
        if (!UDF_SUCCESS(st)) return st;
    }
    NeedAdTypeFor(I, part);
    /* the extents reach the range's end (a sparse tail) */
    if (ExtEnd(I) < (end << Vol->BlockShift)) {
        ULONGLONG gap = (end << Vol->BlockShift) - ExtEnd(I);
        while (gap) {
            ULONG piece = gap > Vol->ExtMax ? Vol->ExtMax : (ULONG)gap;
            if (!ExtPush(Vol, I, UDF_EXT_SPARSE, 0, 0, piece)) return UDF_NO_MEMORY;
            gap -= piece;
        }
    }
    while (blk < end) {
        ULONG i = ExtSplit(Vol, I, blk);
        PUDF_EXT e;
        ULONGLONG eblocks;
        if (i == (ULONG)-1) return UDF_NO_MEMORY;
        if (i >= I->NExt) break;
        e = &I->Ext[i];
        eblocks = e->Len >> Vol->BlockShift;
        if (e->Type == UDF_EXT_RECORDED) { blk += eblocks; continue; }
        /* the part of this extent inside the range */
        if (blk + eblocks > end) {
            if (ExtSplit(Vol, I, end) == (ULONG)-1) return UDF_NO_MEMORY;
            e = &I->Ext[i];
            eblocks = e->Len >> Vol->BlockShift;
        }
        if (e->Type == UDF_EXT_ALLOCATED) {
            /* allocated, never written: it becomes recorded, its contents zeroed */
            st = ZeroBlocks(Vol, e->Part, e->Lbn, (ULONG)eblocks, viaCache);
            if (!UDF_SUCCESS(st)) return st;
            e->Type = UDF_EXT_RECORDED;
            blk += eblocks;
            continue;
        }
        /* a hole: new blocks, as contiguous as the free space allows */
        {
            ULONG want = (ULONG)eblocks, lbn, got;
            ULONG maxRun = Vol->ExtMax >> Vol->BlockShift;
            ULONG g = Goal(Vol, I, blk, part);
            if (want > maxRun) want = maxRun;
            if (part == Vol->AvoidPart && Vol->AvoidHi > Vol->AvoidLo && g > Vol->AvoidLo && g <= Vol->AvoidHi &&
                !(I->Loc.Part == part && g == I->Loc.Lbn + 1)) {
                /* a file with its data in the boot view's window (a boot file): it grows in place */
                ULONG lo = Vol->AvoidLo;
                Vol->AvoidLo = Vol->AvoidHi;
                st = UdfAlloc(Vol, part, g, want, &lbn, &got);
                Vol->AvoidLo = lo;
            } else {
                st = UdfAlloc(Vol, part, g, want, &lbn, &got);
            }
            if (!UDF_SUCCESS(st)) return st;
            if (Zero || viaCache || Vol->BlockSize > 4096) {
                st = ZeroBlocks(Vol, part, lbn, got, viaCache);
                if (!UDF_SUCCESS(st)) return st;
            }
            if (got < eblocks) {
                if (ExtSplit(Vol, I, blk + got) == (ULONG)-1) return UDF_NO_MEMORY;
                e = &I->Ext[i];
            }
            e->Type = UDF_EXT_RECORDED;
            e->Part = part;
            e->Lbn = lbn;
            blk += got;
        }
    }
    ExtTidy(Vol, I);
    UdfIdirty(Vol, I);
    return UDF_OK;
}

UDF_STATUS
UdfIappend(PUDF_VOL Vol, PUDF_INODE I, USHORT Part, ULONG Lbn, ULONG Bytes, UCHAR Type)
{
    ULONGLONG end = (I->Size + Vol->BlockSize - 1) & ~(ULONGLONG)(Vol->BlockSize - 1);
    if (I->AdType == UDF_ICB_AD_IN_ICB) return UDF_NOT_SUPPORTED;
    NeedAdTypeFor(I, Part);
    /* the extents end at the (block-rounded) size: drop what lies past it */
    if (ExtEnd(I) > end) {
        ULONG i = ExtSplit(Vol, I, end >> Vol->BlockShift);
        if (i == (ULONG)-1) return UDF_NO_MEMORY;
        I->NExt = i;
    }
    if (ExtEnd(I) < end && !ExtPush(Vol, I, UDF_EXT_SPARSE, 0, 0, (ULONG)(end - ExtEnd(I)))) return UDF_NO_MEMORY;
    if (!ExtPush(Vol, I, Type, Part, Lbn, Bytes)) return UDF_NO_MEMORY;
    I->Size = end + Bytes;
    if (!I->Extended || !(I->StreamIcb[0] | I->StreamIcb[1] | I->StreamIcb[2])) I->ObjSize = I->Size;
    UdfIdirty(Vol, I);
    return UDF_OK;
}

/* frees everything from file block Blocks on */
UDF_STATUS
UdfIfreeFrom(PUDF_VOL Vol, PUDF_INODE I, ULONGLONG Blocks)
{
    ULONG i;
    UDF_STATUS st;
    if (I->AdType == UDF_ICB_AD_IN_ICB) return UDF_OK;
    i = ExtSplit(Vol, I, Blocks);
    if (i == (ULONG)-1) return UDF_NO_MEMORY;
    if (i >= I->NExt) return UDF_OK;
    if (Vol->ReadOnly) return UDF_READ_ONLY;
    {
        ULONG j;
        for (j = i; j < I->NExt; j++) {
            PUDF_EXT e = &I->Ext[j];
            if (e->Type == UDF_EXT_SPARSE) continue;
            st = UdfFree(Vol, e->Part, e->Lbn, e->Len >> Vol->BlockShift);
            if (!UDF_SUCCESS(st)) return st;
        }
    }
    I->NExt = i;
    UdfIdirty(Vol, I);
    return UDF_OK;
}

UDF_STATUS
UdfIuninline(PUDF_VOL Vol, PUDF_INODE I)
{
    ULONGLONG size = I->Size;
    PUCHAR data = I->Inline;
    UDF_STATUS st;
    USHORT part;
    if (I->AdType != UDF_ICB_AD_IN_ICB) return UDF_OK;
    if (Vol->ReadOnly) return UDF_READ_ONLY;
    part = PartFor(Vol, I);
    I->AdType = part == I->Loc.Part ? UDF_ICB_AD_SHORT : UDF_ICB_AD_LONG;
    I->Inline = NULL;
    I->InlineCap = 0;
    I->NExt = 0;
    if (size) {
        st = UdfIallocRange(Vol, I, 0, (size + Vol->BlockSize - 1) >> Vol->BlockShift, FALSE);
        if (UDF_SUCCESS(st)) st = UdfIwriteData(Vol, I, 0, (ULONG)size, data);
        if (!UDF_SUCCESS(st)) {
            /* back to embedded data */
            (VOID)UdfIfreeFrom(Vol, I, 0);
            I->AdType = UDF_ICB_AD_IN_ICB;
            I->Inline = data;
            I->InlineCap = Vol->BlockSize;
            I->Size = size;
            return st;
        }
    }
    if (data) UdfHostFree(data);
    I->Size = size;
    UdfIdirty(Vol, I);
    return UDF_OK;
}

static ULONG
InlineRoom(PUDF_VOL V, PUDF_INODE I)
{
    return V->BlockSize - HdrSize(I) - I->EaLen;
}

UDF_STATUS
UdfIsetSize(PUDF_VOL Vol, PUDF_INODE I, ULONGLONG Size)
{
    UDF_STATUS st;
    if (Vol->ReadOnly) return UDF_READ_ONLY;
    if (I->AdType == UDF_ICB_AD_IN_ICB) {
        if (Size > InlineRoom(Vol, I)) {
            st = UdfIuninline(Vol, I);
            if (!UDF_SUCCESS(st)) return st;
        } else {
            if (Size > I->Size) UdfMemZero(I->Inline + I->Size, (ULONG)(Size - I->Size));
            I->Size = Size;
            if (!I->Extended || !(I->StreamIcb[0] | I->StreamIcb[1] | I->StreamIcb[2])) I->ObjSize = Size;
            UdfIdirty(Vol, I);
            return UDF_OK;
        }
    }
    if (Size < I->Size) {
        ULONGLONG keep = (Size + Vol->BlockSize - 1) >> Vol->BlockShift;
        st = UdfIfreeFrom(Vol, I, keep);
        if (!UDF_SUCCESS(st)) return st;
        /* the tail of the last block: zeros, so a later extension reads them */
        if (Size & (Vol->BlockSize - 1)) {
            ULONGLONG abs;
            ULONG run, within = (ULONG)(Size & (Vol->BlockSize - 1));
            UCHAR kind;
            st = UdfImap(Vol, I, Size >> Vol->BlockShift, &abs, &run, &kind);
            if (UDF_SUCCESS(st) && kind == UDF_MAP_RECORDED) {
                if (Cached(I)) {
                    PUDF_BUF b;
                    st = UdfBufGetAbs(Vol, abs, TRUE, &b);
                    if (UDF_SUCCESS(st)) {
                        UdfMemZero(b->Data + within, Vol->BlockSize - within);
                        UdfBufDirty(Vol, b);
                        UdfBufPut(Vol, b);
                    }
                } else {
                    st = UdfReadAbs(Vol, abs, 1, Vol->Scratch);
                    if (UDF_SUCCESS(st)) {
                        UdfMemZero(Vol->Scratch + within, Vol->BlockSize - within);
                        st = UdfWriteAbs(Vol, abs, 1, Vol->Scratch);
                    }
                }
            }
            if (!UDF_SUCCESS(st)) return st;
        }
    }
    I->Size = Size;
    if (!I->Extended || !(I->StreamIcb[0] | I->StreamIcb[1] | I->StreamIcb[2])) I->ObjSize = Size;
    UdfIdirty(Vol, I);
    return UDF_OK;
}

UDF_STATUS
UdfIwriteData(PUDF_VOL Vol, PUDF_INODE I, ULONGLONG Off, ULONG Len, const VOID *Buf)
{
    const UCHAR *in = (const UCHAR *)Buf;
    ULONG done = 0;
    UDF_STATUS st;
    if (Vol->ReadOnly) return UDF_READ_ONLY;
    if (Len == 0) return UDF_OK;
    if (I->AdType == UDF_ICB_AD_IN_ICB) {
        if (Off + Len <= InlineRoom(Vol, I)) {
            if (Off > I->Size) UdfMemZero(I->Inline + I->Size, (ULONG)(Off - I->Size));
            UdfMemCopy(I->Inline + Off, in, Len);
            if (Off + Len > I->Size) {
                I->Size = Off + Len;
                if (!I->Extended || !(I->StreamIcb[0] | I->StreamIcb[1] | I->StreamIcb[2])) I->ObjSize = I->Size;
            }
            UdfIdirty(Vol, I);
            return UDF_OK;
        }
        st = UdfIuninline(Vol, I);
        if (!UDF_SUCCESS(st)) return st;
    }
    {
        ULONGLONG first = Off >> Vol->BlockShift, last = (Off + Len - 1) >> Vol->BlockShift;
        st = UdfIallocRange(Vol, I, first, last - first + 1, FALSE);
        if (!UDF_SUCCESS(st)) return st;
    }
    while (done < Len) {
        ULONGLONG pos = Off + done, abs;
        ULONG within = (ULONG)(pos & (Vol->BlockSize - 1)), run, take;
        UCHAR kind;
        st = UdfImap(Vol, I, pos >> Vol->BlockShift, &abs, &run, &kind);
        if (!UDF_SUCCESS(st)) return st;
        if (kind != UDF_MAP_RECORDED) return UDF_FILE_CORRUPT;
        if (Cached(I)) {
            PUDF_BUF b;
            take = Vol->BlockSize - within;
            if (take > Len - done) take = Len - done;
            st = UdfBufGetAbs(Vol, abs, take != Vol->BlockSize, &b);
            if (!UDF_SUCCESS(st)) return st;
            /* a metadata partition block: the mirror copy too */
            if (b->Mirror == UDF_NO_MIRROR) {
                ULONGLONG a2, m;
                ULONG r2;
                ULONG i = ExtFind(I, pos);
                if (i < I->NExt && UDF_SUCCESS(UdfTranslate(Vol, I->Ext[i].Part,
                        I->Ext[i].Lbn + (ULONG)((pos - I->Ext[i].Off) >> Vol->BlockShift), &a2, &r2, &m)))
                    b->Mirror = m;
            }
            UdfMemCopy(b->Data + within, in + done, take);
            UdfBufDirty(Vol, b);
            UdfBufPut(Vol, b);
        } else if (within || Len - done < Vol->BlockSize) {
            take = Vol->BlockSize - within;
            if (take > Len - done) take = Len - done;
            st = UdfReadAbs(Vol, abs, 1, Vol->Scratch);
            if (!UDF_SUCCESS(st)) return st;
            UdfMemCopy(Vol->Scratch + within, in + done, take);
            st = UdfWriteAbs(Vol, abs, 1, Vol->Scratch);
            if (!UDF_SUCCESS(st)) return st;
        } else {
            ULONG blocks = (Len - done) >> Vol->BlockShift;
            if (blocks > run) blocks = run;
            if (blocks > 256) blocks = 256;
            st = UdfWriteAbs(Vol, abs, blocks, in + done);
            if (!UDF_SUCCESS(st)) return st;
            take = blocks << Vol->BlockShift;
        }
        done += take;
    }
    if (Off + Len > I->Size) {
        I->Size = Off + Len;
        if (!I->Extended || !(I->StreamIcb[0] | I->StreamIcb[1] | I->StreamIcb[2])) I->ObjSize = I->Size;
    }
    UdfIdirty(Vol, I);
    return UDF_OK;
}

/* ---- writing a file entry ---------------------------------------------------------------- */
/* the allocation descriptors to record: Out (NULL: count), returns how many */
typedef struct {
    PUDF_VOL V;
    PUCHAR Out;
    ULONG Max, N, Sz;
    ULONGLONG Rec;
} ADW;

static VOID
Emit(ADW *W, UCHAR Type, ULONG Len, USHORT Part, ULONG Lbn)
{
    if (W->Out && W->N < W->Max) {
        PUCHAR a = W->Out + W->N * W->Sz;
        UdfMemZero(a, W->Sz);
        UdfPut32(a, ((ULONG)Type << 30) | Len);
        if (Type != UDF_EXT_SPARSE) {
            UdfPut32(a + 4, Lbn);
            if (W->Sz != UDF_SHORT_AD) UdfPut16(a + 8, Part);
        }
    }
    if (Type != UDF_EXT_SPARSE) W->Rec += (Len + W->V->BlockSize - 1) >> W->V->BlockShift;
    W->N++;
}

static ULONG
BuildAds(PUDF_VOL V, PUDF_INODE I, PUCHAR Out, ULONG Max, PULONGLONG Recorded)
{
    ULONGLONG size = I->Size, roundSize = (size + V->BlockSize - 1) & ~(ULONGLONG)(V->BlockSize - 1);
    ULONG i;
    BOOLEAN pastEnd = FALSE;
    ADW W;
    W.V = V; W.Out = Out; W.Max = Max; W.N = 0; W.Sz = AdSize(I->AdType); W.Rec = 0;
    for (i = 0; i < I->NExt; i++)
        if (I->Ext[i].Type != UDF_EXT_SPARSE && I->Ext[i].Off + I->Ext[i].Len > roundSize) pastEnd = TRUE;
    for (i = 0; i < I->NExt; i++) {
        PUDF_EXT e = &I->Ext[i];
        ULONGLONG end = e->Off + e->Len;
        if (e->Off >= roundSize) {
            if (e->Type != UDF_EXT_SPARSE) Emit(&W, UDF_EXT_ALLOCATED, e->Len, e->Part, e->Lbn);
            continue;
        }
        if (end > roundSize) {
            ULONG inside = (ULONG)(roundSize - e->Off);
            if (e->Type == UDF_EXT_SPARSE) {
                Emit(&W, UDF_EXT_SPARSE, pastEnd ? inside : (ULONG)(size - e->Off), 0, 0);
            } else {
                Emit(&W, e->Type, inside, e->Part, e->Lbn);
                Emit(&W, UDF_EXT_ALLOCATED, e->Len - inside, e->Part, e->Lbn + (inside >> V->BlockShift));
            }
            continue;
        }
        if (end == roundSize && !pastEnd) Emit(&W, e->Type, (ULONG)(size - e->Off), e->Part, e->Lbn);
        else Emit(&W, e->Type, e->Len, e->Part, e->Lbn);
    }
    /* a file longer than its extents: a hole to its end */
    if (ExtEnd(I) < size) {
        ULONGLONG at = ExtEnd(I);
        while (at < size) {
            ULONGLONG left = size - at;
            ULONG piece = left > V->ExtMax ? V->ExtMax : (ULONG)left;
            Emit(&W, UDF_EXT_SPARSE, piece, 0, 0);
            at += piece;
        }
    }
    if (Recorded) *Recorded = W.Rec;
    return W.N;
}

static VOID
PutNextAd(PUDF_VOL V, PUCHAR A, UCHAR AdType, const UDF_LOC *L)
{
    ULONG sz = AdSize(AdType);
    UdfMemZero(A, sz);
    UdfPut32(A, ((ULONG)UDF_EXT_NEXT << 30) | V->BlockSize);
    UdfPut32(A + 4, L->Lbn);
    if (sz != UDF_SHORT_AD) UdfPut16(A + 8, L->Part);
}

UDF_STATUS
UdfIwrite(PUDF_VOL Vol, PUDF_INODE I)
{
    ULONG B = Vol->BlockSize, hdr = HdrSize(I), room, sz, n, inFe, perAed, needAed, i, k;
    PUCHAR ads = NULL;
    PUDF_BUF fb;
    PUCHAR d;
    UDF_STATUS st;
    ULONGLONG rec = 0;
    ULONG lad;

    if (I->Gone || Vol->ReadOnly) { I->Dirty = FALSE; return UDF_OK; }
    room = B - hdr - I->EaLen;
    if (I->AdType == UDF_ICB_AD_IN_ICB) {
        if (I->Size > room) {
            st = UdfIuninline(Vol, I);
            if (!UDF_SUCCESS(st)) return st;
        }
    }
    if (I->AdType == UDF_ICB_AD_EXT) I->AdType = UDF_ICB_AD_LONG;
    if (I->AdType == UDF_ICB_AD_IN_ICB) {
        lad = (ULONG)I->Size;
        rec = 0;
        needAed = 0;
        n = 0;
    } else {
        ExtTidy(Vol, I);
        sz = AdSize(I->AdType);
        n = BuildAds(Vol, I, NULL, 0, &rec);
        ads = (PUCHAR)UdfHostAlloc((n + 1) * sz);
        if (!ads) return UDF_NO_MEMORY;
        BuildAds(Vol, I, ads, n, &rec);
        inFe = room / sz;
        perAed = (B - UDF_AED_ADS) / sz;
        if (n <= inFe) needAed = 0;
        else {
            ULONG left = n - (inFe - 1);
            needAed = 0;
            while (left) {
                needAed++;
                if (left <= perAed) break;
                left -= perAed - 1;
            }
        }
        /* the blocks for the extents of descriptors: reuse, add, give back */
        while (I->NAed < needAed) {
            UDF_LOC l;
            ULONG got;
            USHORT part = I->Loc.Part;
            st = UdfAlloc(Vol, part, I->NAed ? I->Aed[I->NAed - 1].Lbn + 1 : I->Loc.Lbn + 1, 1, &l.Lbn, &got);
            if (!UDF_SUCCESS(st)) { UdfHostFree(ads); return st; }
            l.Part = part;
            st = AddAed(I, &l);
            if (!UDF_SUCCESS(st)) { UdfHostFree(ads); return st; }
        }
        while (I->NAed > needAed) {
            PUDF_LOC l = &I->Aed[I->NAed - 1];
            st = UdfFree(Vol, l->Part, l->Lbn, 1);
            if (!UDF_SUCCESS(st)) { UdfHostFree(ads); return st; }
            I->NAed--;
        }
        lad = 0;
    }

    st = UdfBufGet(Vol, I->Loc.Part, I->Loc.Lbn, FALSE, &fb);
    if (!UDF_SUCCESS(st)) { if (ads) UdfHostFree(ads); return st; }
    d = fb->Data;
    UdfMemZero(d, B);
    /* the ICB tag */
    UdfMemCopy(d + UDF_ICBTAG, I->IcbTag, UDF_ICBTAG_SIZE);
    if (UdfGet16(d + UDF_ICBTAG + UDF_ICBTAG_STRATEGY) != 4096) {
        UdfPut16(d + UDF_ICBTAG + UDF_ICBTAG_STRATEGY, 4);
        UdfPut16(d + UDF_ICBTAG + UDF_ICBTAG_MAXENTRIES, 1);
    }
    d[UDF_ICBTAG + UDF_ICBTAG_FILETYPE] = I->FileType;
    I->IcbFlags = (USHORT)((I->IcbFlags & ~UDF_ICB_AD_MASK) | I->AdType);
    UdfPut16(d + UDF_ICBTAG + UDF_ICBTAG_FLAGS, I->IcbFlags);
    UdfMemCopy(I->IcbTag, d + UDF_ICBTAG, UDF_ICBTAG_SIZE);
    UdfPut32(d + UDF_FE_UID, I->Uid);
    UdfPut32(d + UDF_FE_GID, I->Gid);
    UdfPut32(d + UDF_FE_PERM, I->Perm);
    UdfPut16(d + UDF_FE_LINKS, I->Links);
    d[UDF_FE_RECFMT] = I->RecFmt;
    d[UDF_FE_RECATTR] = I->RecAttr;
    UdfPut32(d + UDF_FE_RECLEN, I->RecLen);
    UdfPut64(d + UDF_FE_INFOLEN, I->Size);
    if (I->Extended) {
        if (!(I->StreamIcb[0] | I->StreamIcb[1] | I->StreamIcb[2]) || I->ObjSize < I->Size) I->ObjSize = I->Size;
        UdfPut64(d + UDF_EFE_OBJSIZE, I->ObjSize);
        UdfPut64(d + UDF_EFE_BLOCKS, rec);
        UdfTimePut(d + UDF_EFE_ATIME, &I->Atime);
        UdfTimePut(d + UDF_EFE_MTIME, &I->Mtime);
        UdfTimePut(d + UDF_EFE_CTIME, &I->Btime);
        UdfTimePut(d + UDF_EFE_ATTRTIME, &I->Ctime);
        UdfPut32(d + UDF_EFE_CHECKPOINT, I->Checkpoint ? I->Checkpoint : 1);
        UdfMemCopy(d + UDF_EFE_EAICB, I->EaIcb, UDF_LONG_AD);
        UdfMemCopy(d + UDF_EFE_STREAMICB, I->StreamIcb, UDF_LONG_AD);
        UdfMemCopy(d + UDF_EFE_IMPL, I->Impl, 32);
        UdfPut64(d + UDF_EFE_UNIQUEID, I->UniqueId);
        UdfPut32(d + UDF_EFE_LEA, I->EaLen);
    } else {
        UdfPut64(d + UDF_FE_BLOCKS, rec);
        UdfTimePut(d + UDF_FE_ATIME, &I->Atime);
        UdfTimePut(d + UDF_FE_MTIME, &I->Mtime);
        UdfTimePut(d + UDF_FE_ATTRTIME, &I->Ctime);
        UdfPut32(d + UDF_FE_CHECKPOINT, I->Checkpoint ? I->Checkpoint : 1);
        UdfMemCopy(d + UDF_FE_EAICB, I->EaIcb, UDF_LONG_AD);
        UdfMemCopy(d + UDF_FE_IMPL, I->Impl, 32);
        UdfPut64(d + UDF_FE_UNIQUEID, I->UniqueId);
        UdfPut32(d + UDF_FE_LEA, I->EaLen);
    }
    if (I->EaLen) UdfMemCopy(d + hdr, I->Ea, I->EaLen);
    if (I->AdType == UDF_ICB_AD_IN_ICB) {
        if (lad) UdfMemCopy(d + hdr + I->EaLen, I->Inline, lad);
    } else {
        ULONG take;
        sz = AdSize(I->AdType);
        inFe = room / sz;
        take = needAed ? inFe - 1 : n;
        UdfMemCopy(d + hdr + I->EaLen, ads, take * sz);
        lad = take * sz;
        if (needAed) {
            PutNextAd(Vol, d + hdr + I->EaLen + lad, I->AdType, &I->Aed[0]);
            lad += sz;
        }
        /* the extents of descriptors */
        k = take;
        for (i = 0; i < needAed; i++) {
            PUDF_BUF ab;
            ULONG left = n - k, here, alen;
            BOOLEAN last = (BOOLEAN)(i + 1 == needAed);
            st = UdfBufGet(Vol, I->Aed[i].Part, I->Aed[i].Lbn, FALSE, &ab);
            if (!UDF_SUCCESS(st)) { UdfBufPut(Vol, fb); UdfHostFree(ads); return st; }
            UdfMemZero(ab->Data, B);
            here = last ? left : perAed - 1;
            if (here > left) here = left;
            UdfMemCopy(ab->Data + UDF_AED_ADS, ads + k * sz, here * sz);
            alen = here * sz;
            k += here;
            if (!last) {
                PutNextAd(Vol, ab->Data + UDF_AED_ADS + alen, I->AdType, &I->Aed[i + 1]);
                alen += sz;
            }
            UdfPut32(ab->Data + UDF_AED_PREV, i ? I->Aed[i - 1].Lbn : I->Loc.Lbn);
            UdfPut32(ab->Data + UDF_AED_LAD, alen);
            UdfTagMake(ab->Data, UDF_TAG_AED, Vol->TagVersion, Vol->TagSerial, I->Aed[i].Lbn, UDF_AED_ADS + alen);
            UdfBufDirty(Vol, ab);
            UdfBufPut(Vol, ab);
        }
    }
    if (I->Extended) UdfPut32(d + UDF_EFE_LAD, lad);
    else UdfPut32(d + UDF_FE_LAD, lad);
    UdfTagMake(d, I->Extended ? UDF_TAG_EFE : UDF_TAG_FE, Vol->TagVersion, Vol->TagSerial, I->Loc.Lbn,
               hdr + I->EaLen + lad);
    UdfBufDirty(Vol, fb);
    UdfBufPut(Vol, fb);
    if (ads) UdfHostFree(ads);
    I->Dirty = FALSE;
    if (I->Refs == 0) { LruAdd(Vol, I); TrimInodes(Vol); }
    return UDF_OK;
}

UDF_STATUS
UdfIflushAll(PUDF_VOL Vol)
{
    ULONG h, again;
    UDF_STATUS st = UDF_OK, s;
    /* writing one can dirty another (the metadata file growing): until none is left */
    for (again = 0; again < 8; again++) {
        BOOLEAN any = FALSE;
        for (h = 0; h < UDF_INODE_HASH; h++) {
            PUDF_INODE I, N;
            for (I = Vol->InoHash[h]; I; I = N) {
                N = I->HashNext;
                if (!I->Dirty) continue;
                any = TRUE;
                s = UdfIwrite(Vol, I);
                if (!UDF_SUCCESS(s) && UDF_SUCCESS(st)) st = s;
            }
        }
        if (!any || !UDF_SUCCESS(st)) break;
    }
    return st;
}

VOID
UdfIdropAll(PUDF_VOL Vol)
{
    ULONG h;
    for (h = 0; h < UDF_INODE_HASH; h++) {
        PUDF_INODE I, N;
        for (I = Vol->InoHash[h]; I; I = N) {
            N = I->HashNext;
            if (I->Ea) UdfHostFree(I->Ea);
            if (I->Inline) UdfHostFree(I->Inline);
            if (I->Ext) UdfHostFree(I->Ext);
            if (I->Aed) UdfHostFree(I->Aed);
            UdfHostFree(I);
        }
        Vol->InoHash[h] = NULL;
    }
    Vol->InoLruHead = Vol->InoLruTail = NULL;
    Vol->InoCount = 0;
}

/* ---- new and deleted files ------------------------------------------------------------------- */
UDF_STATUS
UdfInew(PUDF_VOL Vol, UCHAR FileType, const UDF_LOC *Near, PUDF_INODE *Out)
{
    PUDF_INODE I;
    UDF_LOC l;
    ULONG got;
    UDF_STATUS st;
    UDF_TIME now;
    if (Vol->ReadOnly) return UDF_READ_ONLY;
    l.Part = Vol->IcbPart;
    st = UdfAlloc(Vol, l.Part, Near && Near->Part == l.Part ? Near->Lbn + 1 : 0, 1, &l.Lbn, &got);
    if (!UDF_SUCCESS(st)) return st;
    UdfBufForget(Vol, l.Part, l.Lbn, 1);
    I = (PUDF_INODE)UdfHostAlloc(sizeof(UDF_INODE));
    if (!I) { (VOID)UdfFree(Vol, l.Part, l.Lbn, 1); return UDF_NO_MEMORY; }
    UdfMemZero(I, sizeof(*I));
    I->Loc = l;
    I->Extended = Vol->UseEfe;
    I->FileType = FileType;
    UdfPut16(I->IcbTag + UDF_ICBTAG_STRATEGY, 4);
    UdfPut16(I->IcbTag + UDF_ICBTAG_MAXENTRIES, 1);
    I->IcbTag[UDF_ICBTAG_FILETYPE] = FileType;
    if (FileType == UDF_FT_DIRECTORY || FileType == UDF_FT_STREAMDIR || FileType == UDF_FT_SYMLINK) {
        I->AdType = UDF_ICB_AD_IN_ICB;
        I->InlineCap = Vol->BlockSize;
        I->Inline = (PUCHAR)UdfHostAlloc(Vol->BlockSize);
        if (!I->Inline) { UdfHostFree(I); (VOID)UdfFree(Vol, l.Part, l.Lbn, 1); return UDF_NO_MEMORY; }
        UdfMemZero(I->Inline, Vol->BlockSize);
    } else {
        I->AdType = Vol->DataPart == l.Part ? UDF_ICB_AD_SHORT : UDF_ICB_AD_LONG;
    }
    I->IcbFlags = I->AdType;
    I->Uid = 0xFFFFFFFF;
    I->Gid = 0xFFFFFFFF;
    I->Perm = (UDF_PERM_CHATTR | UDF_PERM_DELETE) << 10;
    UdfTimeNow(Vol, &now);
    I->Atime = I->Mtime = I->Ctime = I->Btime = now;
    I->Checkpoint = 1;
    UdfRegidImpl(I->Impl);
    I->UniqueId = UdfNewUniqueId(Vol);
    I->Refs = 1;
    I->Dirty = TRUE;
    I->HashNext = Vol->InoHash[IHASH(l)];
    Vol->InoHash[IHASH(l)] = I;
    Vol->InoCount++;
    Vol->Changed = TRUE;
    *Out = I;
    return UDF_OK;
}

static UDF_STATUS DeleteStreams(PUDF_VOL Vol, PUDF_INODE I);

UDF_STATUS
UdfIdelete(PUDF_VOL Vol, PUDF_INODE I)
{
    UDF_STATUS st;
    ULONG len, i;
    UDF_LOC l;
    if (Vol->ReadOnly) return UDF_READ_ONLY;
    st = DeleteStreams(Vol, I);
    if (!UDF_SUCCESS(st)) return st;
    st = UdfIfreeFrom(Vol, I, 0);
    if (!UDF_SUCCESS(st)) return st;
    for (i = 0; i < I->NAed; i++) {
        st = UdfFree(Vol, I->Aed[i].Part, I->Aed[i].Lbn, 1);
        if (!UDF_SUCCESS(st)) return st;
    }
    I->NAed = 0;
    /* an extended attribute file */
    UdfLongAdGet(I->EaIcb, &len, &l);
    if (len & UDF_EXT_LEN_MASK) {
        PUDF_INODE E;
        if (UDF_SUCCESS(UdfIget(Vol, &l, &E))) {
            (VOID)UdfIfreeFrom(Vol, E, 0);
            for (i = 0; i < E->NAed; i++) (VOID)UdfFree(Vol, E->Aed[i].Part, E->Aed[i].Lbn, 1);
            E->NAed = 0;
            E->Gone = TRUE;
            E->Dirty = FALSE;
            (VOID)UdfFree(Vol, E->Loc.Part, E->Loc.Lbn, 1);
            UdfIput(Vol, E);
        }
    }
    st = UdfFree(Vol, I->Loc.Part, I->Loc.Lbn, 1);
    if (!UDF_SUCCESS(st)) return st;
    I->Gone = TRUE;
    I->Dirty = FALSE;
    Vol->Changed = TRUE;
    return UDF_OK;
}

/* a file's named streams (2.00+): its stream directory and every stream in it */
static UDF_STATUS
DeleteStreams(PUDF_VOL Vol, PUDF_INODE I)
{
    ULONG len;
    UDF_LOC l;
    PUDF_INODE S;
    UDF_STATUS st;
    UDF_DIRENT *ent;
    ULONGLONG cookie = 0;
    if (!I->Extended) return UDF_OK;
    UdfLongAdGet(I->StreamIcb, &len, &l);
    if (!(len & UDF_EXT_LEN_MASK)) return UDF_OK;
    st = UdfIget(Vol, &l, &S);
    if (!UDF_SUCCESS(st)) return UDF_OK;          /* unreadable: leave it */
    ent = (UDF_DIRENT *)UdfHostAlloc(sizeof(UDF_DIRENT));
    if (!ent) { UdfIput(Vol, S); return UDF_NO_MEMORY; }
    for (;;) {
        PUDF_INODE F;
        st = UdfDirNext(Vol, S, cookie, ent);
        if (st == UDF_NO_MORE) { st = UDF_OK; break; }
        if (!UDF_SUCCESS(st)) break;
        cookie = ent->Offset + ent->FidLen;
        if (ent->Chars & (UDF_FID_PARENT | UDF_FID_DELETED)) continue;
        if (!UDF_SUCCESS(UdfIget(Vol, &ent->Icb, &F))) continue;
        if (F->Links <= 1) {
            st = UdfIdelete(Vol, F);
            if (!UDF_SUCCESS(st)) { UdfIput(Vol, F); break; }
        } else {
            F->Links--;
            UdfIdirty(Vol, F);
        }
        UdfIput(Vol, F);
    }
    UdfHostFree(ent);
    if (UDF_SUCCESS(st)) st = UdfIdelete(Vol, S);
    UdfIput(Vol, S);
    UdfMemZero(I->StreamIcb, UDF_LONG_AD);
    return st;
}

/* ---- moving a file's data (the boot view) ----------------------------------------------------- */
static BOOLEAN
Misplaced(ULONG Lbn, ULONG Lo, ULONG Hi, BOOLEAN In)
{
    BOOLEAN inside = (BOOLEAN)(Lbn >= Lo && Lbn < Hi);
    return (BOOLEAN)(In ? !inside : inside);
}

BOOLEAN
UdfIplaced(PUDF_VOL Vol, PUDF_INODE I, ULONG Lo, ULONG Hi, ULONG Align, ULONG AlignBase, BOOLEAN In)
{
    ULONGLONG n = (I->Size + Vol->BlockSize - 1) >> Vol->BlockShift, blk;
    if (I->AdType == UDF_ICB_AD_IN_ICB) return (BOOLEAN)(!In || I->Size == 0);
    if (Align == 0) Align = 1;
    for (blk = 0; blk < n; ) {
        ULONG i = ExtFind(I, blk << Vol->BlockShift), k, run;
        PUDF_EXT e;
        if (i == I->NExt) return (BOOLEAN)!In;
        e = &I->Ext[i];
        if (e->Type != UDF_EXT_RECORDED || e->Part != Vol->DataPart) { if (In) return FALSE; blk = (e->Off + e->Len) >> Vol->BlockShift; continue; }
        run = (ULONG)((e->Off + e->Len - (blk << Vol->BlockShift)) >> Vol->BlockShift);
        for (k = 0; k < run && blk + k < n; k++) {
            ULONG lbn = e->Lbn + (ULONG)(blk - (e->Off >> Vol->BlockShift)) + k;
            if (Misplaced(lbn, Lo, Hi, In)) return FALSE;
            /* the first block of each piece starts a piece on the disk; the rest follow it */
            if (In && ((blk + k) % Align) == 0 && ((lbn - AlignBase) % Align) != 0) return FALSE;
            if (In && ((blk + k) % Align) != 0) {
                /* the previous block of the file must be the previous disk block */
                ULONG j = ExtFind(I, (blk + k - 1) << Vol->BlockShift);
                PUDF_EXT p = &I->Ext[j];
                ULONG plbn = p->Lbn + (ULONG)((((blk + k - 1) << Vol->BlockShift) - p->Off) >> Vol->BlockShift);
                if (p->Type != UDF_EXT_RECORDED || plbn + 1 != lbn) return FALSE;
            }
        }
        blk += run;
    }
    return TRUE;
}

UDF_STATUS
UdfIrelocate(PUDF_VOL Vol, PUDF_INODE I, ULONG Lo, ULONG Hi, ULONG Align, ULONG AlignBase, BOOLEAN In, PULONG Moved)
{
    USHORT part = Vol->DataPart;
    ULONGLONG n, blk;
    PUDF_EXT newExt = NULL, oldExt;
    ULONG nNew = 0, capNew, nOld, i, tmpBlocks, hint, cnt = 0;
    PUCHAR tmp = NULL;
    UDF_STATUS st = UDF_OK;
    *Moved = 0;
    if (Vol->ReadOnly) return UDF_READ_ONLY;
    if (Align == 0) Align = 1;
    if (Cached(I)) return UDF_NOT_SUPPORTED;
    if (UdfIplaced(Vol, I, Lo, Hi, Align, AlignBase, In)) return UDF_OK;
    if (I->AdType == UDF_ICB_AD_IN_ICB) {
        if (!In) return UDF_OK;
        st = UdfIuninline(Vol, I);
        if (!UDF_SUCCESS(st)) return st;
    }
    n = (I->Size + Vol->BlockSize - 1) >> Vol->BlockShift;
    if (n == 0) return UDF_OK;
    /* holes and unrecorded ranges get blocks first (written as zeros) */
    st = UdfIallocRange(Vol, I, 0, n, TRUE);
    if (!UDF_SUCCESS(st)) return st;
    NeedAdTypeFor(I, part);
    /* the copy buffer: a piece, or (moving out) up to 64 KB */
    tmpBlocks = In ? Align : (65536 >> Vol->BlockShift ? 65536 >> Vol->BlockShift : 1);
    tmp = (PUCHAR)UdfHostAlloc(tmpBlocks << Vol->BlockShift);
    capNew = (ULONG)(In ? n / Align : n / 64) + 8;
    newExt = (PUDF_EXT)UdfHostAlloc(capNew * sizeof(UDF_EXT));
    if (!tmp || !newExt) { st = UDF_NO_MEMORY; goto out; }
    hint = Lo;
    /* piece by piece: In, Align blocks (the last one shorter) each in one aligned run; out, runs as they come */
    for (blk = 0; blk < n && UDF_SUCCESS(st); blk += cnt) {
        ULONG lbn = 0, got, k;
        cnt = (ULONG)(n - blk < Align ? n - blk : Align);
        if (In) {
            /* the whole piece's clusters stay the file's: a short last piece still takes Align blocks */
            st = UdfAllocAligned(Vol, part, hint, Hi, Align, Align, AlignBase, &lbn);
            if (st == UDF_DISK_FULL && hint > Lo) st = UdfAllocAligned(Vol, part, Lo, Hi, Align, Align, AlignBase, &lbn);
            if (UDF_SUCCESS(st)) hint = lbn + Align;
        } else {
            ULONG savedLo = Vol->AvoidLo, savedHi = Vol->AvoidHi;
            USHORT savedPart = Vol->AvoidPart;
            ULONGLONG want = n - blk;
            if (want > 4096) want = 4096;
            Vol->AvoidPart = part; Vol->AvoidLo = Lo; Vol->AvoidHi = Hi;
            st = UdfAlloc(Vol, part, Hi, (ULONG)want, &lbn, &got);
            Vol->AvoidPart = savedPart; Vol->AvoidLo = savedLo; Vol->AvoidHi = savedHi;
            if (UDF_SUCCESS(st) && (got == 0 || (lbn < Hi && lbn + got > Lo))) {
                (VOID)UdfFree(Vol, part, lbn, got);
                st = UDF_DISK_FULL;
            }
            if (UDF_SUCCESS(st)) cnt = got;
        }
        if (!UDF_SUCCESS(st)) break;
        /* copy the piece */
        for (k = 0; k < cnt && UDF_SUCCESS(st); ) {
            ULONGLONG abs, dst;
            ULONG run, r2;
            UCHAR kind;
            st = UdfImap(Vol, I, blk + k, &abs, &run, &kind);
            if (!UDF_SUCCESS(st)) break;
            if (run > cnt - k) run = cnt - k;
            if (run > tmpBlocks) run = tmpBlocks;
            st = UdfTranslate(Vol, part, lbn + k, &dst, &r2, NULL);
            if (!UDF_SUCCESS(st)) break;
            if (kind == UDF_MAP_RECORDED) {
                st = UdfReadAbs(Vol, abs, run, tmp);
                if (UDF_SUCCESS(st)) st = UdfWriteAbs(Vol, dst, run, tmp);
            } else {
                ULONG z;
                for (z = 0; z < run && UDF_SUCCESS(st); z++) st = UdfWriteAbs(Vol, dst + z, 1, Vol->Zero);
            }
            k += run;
        }
        if (!UDF_SUCCESS(st)) { (VOID)UdfFree(Vol, part, lbn, In ? Align : cnt); break; }
        if (nNew + 2 > capNew) {
            PUDF_EXT bigger = (PUDF_EXT)UdfHostAlloc(capNew * 2 * sizeof(UDF_EXT));
            if (!bigger) { st = UDF_NO_MEMORY; (VOID)UdfFree(Vol, part, lbn, In ? Align : cnt); break; }
            UdfMemCopy(bigger, newExt, nNew * sizeof(UDF_EXT));
            UdfHostFree(newExt);
            newExt = bigger;
            capNew *= 2;
        }
        newExt[nNew].Off = blk << Vol->BlockShift;
        newExt[nNew].Len = cnt << Vol->BlockShift;
        newExt[nNew].Lbn = lbn;
        newExt[nNew].Part = part;
        newExt[nNew].Type = UDF_EXT_RECORDED;
        newExt[nNew].Pad = 0;
        nNew++;
        if (In && cnt < Align) {
            /* the rest of the last cluster: allocated, past the end of the file */
            newExt[nNew].Off = (blk + cnt) << Vol->BlockShift;
            newExt[nNew].Len = (Align - cnt) << Vol->BlockShift;
            newExt[nNew].Lbn = lbn + cnt;
            newExt[nNew].Part = part;
            newExt[nNew].Type = UDF_EXT_ALLOCATED;
            newExt[nNew].Pad = 0;
            nNew++;
        }
        *Moved += cnt;
    }
    if (!UDF_SUCCESS(st)) {
        for (i = 0; i < nNew; i++) (VOID)UdfFree(Vol, newExt[i].Part, newExt[i].Lbn, newExt[i].Len >> Vol->BlockShift);
        *Moved = 0;
        goto out;
    }
    /* the new map in, the old blocks out */
    oldExt = I->Ext;
    nOld = I->NExt;
    I->Ext = newExt;
    I->NExt = nNew;
    I->CapExt = capNew;
    newExt = NULL;
    for (i = 0; i < nOld; i++)
        if (oldExt[i].Type != UDF_EXT_SPARSE) (VOID)UdfFree(Vol, oldExt[i].Part, oldExt[i].Lbn, oldExt[i].Len >> Vol->BlockShift);
    UdfHostFree(oldExt);
    ExtTidy(Vol, I);
    UdfIdirty(Vol, I);
out:
    if (tmp) UdfHostFree(tmp);
    if (newExt) UdfHostFree(newExt);
    return st;
}
