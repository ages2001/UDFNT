/*
 * UDFNT - udfchk: checks (and with /F repairs) a UDF volume.
 *
 *   udfchk E: [/F] [/V]
 *   udfchk \\.\PhysicalDriveN /P:n [/F] [/V]
 *   udfchk IMAGE [/P:n] [/F] [/V]
 *
 * Independent of the engine: it reads the volume itself. It checks the
 * descriptors and their tags, the partitions, every file entry reachable from
 * the root (and the streams), the directories (entries, parent entries),
 * link counts, unique IDs, that no block is used twice, that the space
 * bitmaps match what is used, and the integrity descriptor's counts. /F
 * rewrites the bitmaps and the integrity descriptor, and fixes link counts.
 *
 * Exit code: 0 no errors, 1 errors were fixed, 2 errors remain, 8 cannot check.
 */
#include "udfeng.h"
#include "udfimg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#ifndef _WIN32
#include <strings.h>
#include <sys/time.h>
#define _stricmp strcasecmp
#endif

/* the support routines need a host only for the time */
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

static UDF_IMAGE Img;
static ULONG B, Shift;
static ULONG Errors, Fixed, Warnings;
static int Fix, Verbose, Quiet;
static ULONG LastBlock;
static USHORT Revision, TagVer, TagSerial;

static void
Err(const char *F, ...)
{
    va_list a;
    Errors++;
    if (Errors > 200) return;
    va_start(a, F);
    printf("  ");
    vprintf(F, a);
    printf("\n");
    va_end(a);
}

static void
Warn(const char *F, ...)
{
    va_list a;
    Warnings++;
    if (!Verbose && Warnings > 50) return;
    va_start(a, F);
    printf("  note: ");
    vprintf(F, a);
    printf("\n");
    va_end(a);
}

static int
ReadBlk(ULONGLONG Abs, ULONG Count, void *Buf)
{
    if (Abs + Count > (ULONGLONG)LastBlock + 1) return 0;
    return UimgRead(&Img, Abs << Shift, Buf, Count << Shift);
}

static int
WriteBlk(ULONGLONG Abs, ULONG Count, const void *Buf)
{
    return UimgWrite(&Img, Abs << Shift, Buf, Count << Shift);
}

/* ---- partitions ------------------------------------------------------------- */
typedef struct {
    UCHAR   Kind;                       /* UDF_KIND_* */
    USHORT  Number, Under;
    ULONG   Start, Length;
    ULONG   BitmapLbn, BitmapBlocks;    /* phys/spar: the SBD extent */
    int     HasBitmap;
    int     BitmapEmbedded;             /* meta: the bitmap is in its file entry */
    USHORT  Packet;
    ULONG   NSpare, *SpOrig, *SpMap;
    ULONG  *Vat, NVat;
    ULONG   MetaFile, MetaMirror, MetaBitmap;
    UCHAR   MetaFlags;
    /* the metadata file's extents (into the physical partition) */
    ULONG  *MExtLbn, *MExtLen, NMExt;
    ULONG  *RExtLbn, *RExtLen, NRExt;   /* the mirror's */
    ULONG  *BExtLbn, *BExtLen, NBExt;   /* the bitmap file's */
    /* what the check finds */
    UCHAR  *Used;                       /* one byte per block: 0 free, else what */
    ULONG   UsedCount;
} PART;

static PART P[UDF_MAX_MAPS];
static ULONG NP;
static UCHAR *Lvid;
static ULONG LvidBlock;

#define U_META      1
#define U_ICB       2
#define U_DATA      3
#define U_AED       4
#define U_DIR       5

static const char *UseName[] = { "free", "metadata", "file entry", "file data", "allocation descriptors", "directory" };

static int
MapExt(ULONG *Lbn, ULONG *Len, ULONG N, ULONG Blk, ULONG *Out)
{
    ULONG i, at = 0;
    for (i = 0; i < N; i++) {
        ULONG blocks = (Len[i] + B - 1) >> Shift;
        if (Blk < at + blocks) { *Out = Lbn[i] + (Blk - at); return 1; }
        at += blocks;
    }
    return 0;
}

/* (partition, block) -> volume block */
static int
Translate(USHORT Part, ULONG Lbn, ULONGLONG *Abs)
{
    PART *p;
    if (Part >= NP) return 0;
    p = &P[Part];
    switch (p->Kind) {
    case UDF_KIND_PHYS:
        if (Lbn >= p->Length) return 0;
        *Abs = (ULONGLONG)p->Start + Lbn;
        return 1;
    case UDF_KIND_SPAR: {
        ULONG i, base = p->Packet ? Lbn - Lbn % p->Packet : Lbn;
        if (Lbn >= p->Length) return 0;
        for (i = 0; i < p->NSpare; i++)
            if (p->SpOrig[i] == base) { *Abs = (ULONGLONG)p->SpMap[i] + (Lbn - base); return 1; }
        *Abs = (ULONGLONG)p->Start + Lbn;
        return 1;
    }
    case UDF_KIND_VIRT:
        if (Lbn >= p->NVat) return 0;
        return Translate(p->Under, p->Vat[Lbn], Abs);
    case UDF_KIND_META: {
        ULONG l;
        if (!MapExt(p->MExtLbn, p->MExtLen, p->NMExt, Lbn, &l) && !MapExt(p->RExtLbn, p->RExtLen, p->NRExt, Lbn, &l)) return 0;
        return Translate(p->Under, l, Abs);
    }
    }
    return 0;
}

static int
ReadPart(USHORT Part, ULONG Lbn, void *Buf)
{
    ULONGLONG a;
    return Translate(Part, Lbn, &a) && ReadBlk(a, 1, Buf);
}

/* marks blocks as used; reports blocks used twice */
static void
MarkUsed(USHORT Part, ULONG Lbn, ULONG Count, UCHAR What, const char *Who)
{
    PART *p;
    ULONG i;
    if (Part >= NP) { Err("%s: partition %u does not exist", Who, Part); return; }
    p = &P[Part];
    if (p->Kind == UDF_KIND_VIRT) return;
    if (!p->Used) return;
    if ((ULONGLONG)Lbn + Count > p->Length) { Err("%s: blocks %lu-%lu lie outside partition %u", Who, (unsigned long)Lbn, (unsigned long)(Lbn + Count - 1), Part); return; }
    for (i = 0; i < Count; i++) {
        if (p->Used[Lbn + i]) {
            Err("%s: block %lu of partition %u is also used as %s", Who, (unsigned long)(Lbn + i), Part, UseName[p->Used[Lbn + i]]);
            continue;
        }
        p->Used[Lbn + i] = What;
        p->UsedCount++;
    }
}

/* ---- file entries ----------------------------------------------------------- */
typedef struct {
    UCHAR   Type;
    ULONG   Len, Lbn;
    USHORT  Part;
} EXT;

typedef struct {
    int     Ok;
    int     Efe;
    UCHAR   FileType;
    UCHAR   AdType;
    USHORT  Links;
    ULONGLONG Size, Blocks, UniqueId;
    UDF_LOC StreamDir, EaIcb;
    int     HasStream, HasEa;
    EXT    *X;
    ULONG   NX;
    UCHAR  *Inline;
    ULONG   InlineLen;
} FE;

static void
FreeFe(FE *F)
{
    free(F->X);
    free(F->Inline);
    memset(F, 0, sizeof(*F));
}

static void
AddExt(FE *F, UCHAR Type, ULONG Len, ULONG Lbn, USHORT Part)
{
    F->X = (EXT *)realloc(F->X, (F->NX + 1) * sizeof(EXT));
    F->X[F->NX].Type = Type;
    F->X[F->NX].Len = Len;
    F->X[F->NX].Lbn = Lbn;
    F->X[F->NX].Part = Part;
    F->NX++;
}

/* reads a file entry and its allocation descriptors; marks its blocks */
static int
LoadFe(const UDF_LOC *L, FE *F, const char *Path, int Mark, UCHAR DataUse)
{
    UCHAR *b = (UCHAR *)malloc(B);
    USHORT id;
    ULONG hdr, lea, lad, sz, off, hops = 0;
    UCHAR *ads;
    USHORT adPart = L->Part;
    memset(F, 0, sizeof(*F));
    if (!ReadPart(L->Part, L->Lbn, b)) { Err("%s: cannot read its file entry (%u:%lu)", Path, L->Part, (unsigned long)L->Lbn); free(b); return 0; }
    id = UdfGet16(b);
    if (id != UDF_TAG_FE && id != UDF_TAG_EFE) { Err("%s: no file entry at %u:%lu (tag %u)", Path, L->Part, (unsigned long)L->Lbn, id); free(b); return 0; }
    if (!UdfTagCheck(b, B, id, L->Lbn)) { Err("%s: the file entry's tag is damaged (checksum, CRC or location)", Path); free(b); return 0; }
    if (Mark == 1) MarkUsed(L->Part, L->Lbn, 1, U_ICB, Path);     /* 2: its blocks, not the entry */
    F->Efe = id == UDF_TAG_EFE;
    F->FileType = b[UDF_ICBTAG + UDF_ICBTAG_FILETYPE];
    F->AdType = (UCHAR)(UdfGet16(b + UDF_ICBTAG + UDF_ICBTAG_FLAGS) & 7);
    F->Links = UdfGet16(b + UDF_FE_LINKS);
    F->Size = UdfGet64(b + UDF_FE_INFOLEN);
    if (F->Efe) {
        F->Blocks = UdfGet64(b + UDF_EFE_BLOCKS);
        F->UniqueId = UdfGet64(b + UDF_EFE_UNIQUEID);
        lea = UdfGet32(b + UDF_EFE_LEA); lad = UdfGet32(b + UDF_EFE_LAD); hdr = UDF_EFE_EA;
        {
            ULONG l;
            UdfLongAdGet(b + UDF_EFE_STREAMICB, &l, &F->StreamDir);
            F->HasStream = (l & UDF_EXT_LEN_MASK) != 0;
            UdfLongAdGet(b + UDF_EFE_EAICB, &l, &F->EaIcb);
            F->HasEa = (l & UDF_EXT_LEN_MASK) != 0;
        }
    } else {
        F->Blocks = UdfGet64(b + UDF_FE_BLOCKS);
        F->UniqueId = UdfGet64(b + UDF_FE_UNIQUEID);
        lea = UdfGet32(b + UDF_FE_LEA); lad = UdfGet32(b + UDF_FE_LAD); hdr = UDF_FE_EA;
        {
            ULONG l;
            UdfLongAdGet(b + UDF_FE_EAICB, &l, &F->EaIcb);
            F->HasEa = (l & UDF_EXT_LEN_MASK) != 0;
        }
    }
    if (F->Efe && Revision < UDF_REV_200) Warn("%s: an extended file entry on a UDF %x.%02x volume", Path, Revision >> 8, Revision & 0xFF);
    if (lea > B - hdr || lad > B - hdr - lea) { Err("%s: extended attribute / descriptor lengths too large", Path); free(b); return 0; }
    if (F->AdType == UDF_ICB_AD_IN_ICB) {
        F->Inline = (UCHAR *)malloc(lad + 1);
        memcpy(F->Inline, b + hdr + lea, lad);
        F->InlineLen = lad;
        if (lad != F->Size) Err("%s: embedded data is %lu bytes, the information length %llu", Path, (unsigned long)lad, (unsigned long long)F->Size);
        F->Ok = 1;
        free(b);
        return 1;
    }
    if (F->AdType > UDF_ICB_AD_EXT) { Err("%s: unknown allocation descriptor type %u", Path, F->AdType); free(b); return 0; }
    sz = F->AdType == UDF_ICB_AD_SHORT ? 8 : F->AdType == UDF_ICB_AD_LONG ? 16 : 20;
    ads = (UCHAR *)malloc(B);
    memcpy(ads, b + hdr + lea, lad);
    for (;;) {
        int more = 0;
        UDF_LOC next;
        for (off = 0; off + sz <= lad; off += sz) {
            UCHAR *a = ads + off;
            ULONG el = UdfGet32(a), len = el & UDF_EXT_LEN_MASK;
            UCHAR type = (UCHAR)(el >> 30);
            UDF_LOC l;
            if (len == 0) break;
            l.Part = adPart;
            if (F->AdType == UDF_ICB_AD_SHORT) l.Lbn = UdfGet32(a + 4);
            else if (F->AdType == UDF_ICB_AD_LONG) { l.Lbn = UdfGet32(a + 4); l.Part = UdfGet16(a + 8); }
            else { l.Lbn = UdfGet32(a + 12); l.Part = UdfGet16(a + 16); }
            if (type == UDF_EXT_NEXT) { next = l; more = 1; break; }
            AddExt(F, type, len, l.Lbn, l.Part);
        }
        if (!more) break;
        if (++hops > 100000) { Err("%s: a loop in its allocation extent descriptors", Path); break; }
        if (!ReadPart(next.Part, next.Lbn, b) || !UdfTagCheck(b, B, UDF_TAG_AED, next.Lbn)) {
            Err("%s: a damaged allocation extent descriptor at %u:%lu", Path, next.Part, (unsigned long)next.Lbn);
            break;
        }
        if (Mark) MarkUsed(next.Part, next.Lbn, 1, U_AED, Path);
        lad = UdfGet32(b + UDF_AED_LAD);
        if (lad > B - UDF_AED_ADS) { Err("%s: an allocation extent descriptor too long", Path); break; }
        memcpy(ads, b + UDF_AED_ADS, lad);
        adPart = next.Part;
    }
    free(ads);
    free(b);
    /* the extents: inside their partitions, cover the information length, blocks once */
    {
        ULONGLONG total = 0, recorded = 0, last = 0;
        ULONG i;
        for (i = 0; i < F->NX; i++) {
            EXT *e = &F->X[i];
            ULONG blocks = (e->Len + B - 1) >> Shift;
            if (i + 1 < F->NX && (e->Len & (B - 1)))
                Err("%s: extent %lu is not a whole number of blocks (%lu bytes)", Path, (unsigned long)i, (unsigned long)e->Len);
            if (e->Type != UDF_EXT_SPARSE) {
                if (e->Part >= NP) { Err("%s: an extent in partition %u, which does not exist", Path, e->Part); continue; }
                if (P[e->Part].Kind != UDF_KIND_VIRT && (ULONGLONG)e->Lbn + blocks > P[e->Part].Length) {
                    Err("%s: an extent (%lu blocks at %lu) runs past partition %u", Path, (unsigned long)blocks, (unsigned long)e->Lbn, e->Part);
                    continue;
                }
                if (Mark && DataUse) MarkUsed(e->Part, e->Lbn, blocks, DataUse, Path);
                recorded += blocks;
                if (e->Type == UDF_EXT_RECORDED) last = total + e->Len;
            }
            total += e->Len;
        }
        if (total < F->Size) Err("%s: its extents cover %llu bytes of %llu", Path, (unsigned long long)total, (unsigned long long)F->Size);
        if (recorded != F->Blocks && Verbose) Warn("%s: %llu blocks recorded, the entry says %llu", Path, (unsigned long long)recorded, (unsigned long long)F->Blocks);
        (void)last;
    }
    F->Ok = 1;
    return 1;
}

/* reads [Off, Off+Len) of a file's data */
static int
ReadData(FE *F, ULONGLONG Off, ULONG Len, UCHAR *Out)
{
    ULONGLONG at = 0;
    ULONG i, done = 0;
    UCHAR *b;
    if (F->AdType == UDF_ICB_AD_IN_ICB) {
        if (Off + Len > F->InlineLen) return 0;
        memcpy(Out, F->Inline + Off, Len);
        return 1;
    }
    b = (UCHAR *)malloc(B);
    for (i = 0; i < F->NX && done < Len; i++) {
        EXT *e = &F->X[i];
        ULONGLONG elen = (e->Len + B - 1) & ~(ULONGLONG)(B - 1);
        while (Off + done >= at && Off + done < at + elen && done < Len) {
            ULONGLONG pos = Off + done - at;
            ULONG within = (ULONG)(pos & (B - 1)), take = B - within;
            if (take > Len - done) take = Len - done;
            if (e->Type != UDF_EXT_RECORDED) memset(Out + done, 0, take);
            else if (!ReadPart(e->Part, e->Lbn + (ULONG)(pos >> Shift), b)) { free(b); return 0; }
            else memcpy(Out + done, b + within, take);
            done += take;
        }
        at += elen;
    }
    free(b);
    if (done < Len) memset(Out + done, 0, Len - done);
    return 1;
}

/* ---- the tree --------------------------------------------------------------------- */
typedef struct {
    UDF_LOC Loc;
    ULONG   FidRefs;        /* entries naming it */
    ULONG   SubDirs;
    USHORT  Links;
    int     IsDir, Visited;
    ULONGLONG UniqueId;
    char   *Path;
} NODE;

static NODE *Nodes;
static ULONG NNodes, CapNodes;
static ULONG *Hash;
static ULONG HashSize = 1 << 16;
static ULONG CountFiles, CountDirs;
static ULONGLONG MaxUnique;

static NODE *
Node(const UDF_LOC *L, int Create)
{
    ULONG h = (L->Lbn * 2654435761u ^ L->Part) & (HashSize - 1);
    while (Hash[h]) {
        NODE *n = &Nodes[Hash[h] - 1];
        if (n->Loc.Lbn == L->Lbn && n->Loc.Part == L->Part) return n;
        h = (h + 1) & (HashSize - 1);
    }
    if (!Create) return NULL;
    if (NNodes + 1 >= HashSize / 2) {
        /* grow the table */
        ULONG i, old = HashSize;
        free(Hash);
        HashSize *= 2;
        Hash = (ULONG *)calloc(HashSize, sizeof(ULONG));
        for (i = 0; i < NNodes; i++) {
            ULONG k = (Nodes[i].Loc.Lbn * 2654435761u ^ Nodes[i].Loc.Part) & (HashSize - 1);
            while (Hash[k]) k = (k + 1) & (HashSize - 1);
            Hash[k] = i + 1;
        }
        (void)old;
        return Node(L, Create);
    }
    if (NNodes == CapNodes) {
        CapNodes = CapNodes ? CapNodes * 2 : 1024;
        Nodes = (NODE *)realloc(Nodes, CapNodes * sizeof(NODE));
    }
    memset(&Nodes[NNodes], 0, sizeof(NODE));
    Nodes[NNodes].Loc = *L;
    Hash[h] = ++NNodes;
    return &Nodes[NNodes - 1];
}

static char *
Join(const char *A, const char *Bn)
{
    size_t la = strlen(A), lb = strlen(Bn);
    char *s = (char *)malloc(la + lb + 2);
    memcpy(s, A, la);
    s[la] = '/';
    memcpy(s + la + 1, Bn, lb + 1);
    return s;
}

static void CheckDir(const UDF_LOC *L, const UDF_LOC *Parent, const char *Path, int Streams);

static void
Visit(const UDF_LOC *L, const UDF_LOC *Parent, const char *Path, int ViaStreamDir)
{
    NODE *n = Node(L, 1);
    FE F;
    n->FidRefs++;
    if (n->Visited) return;
    n->Visited = 1;
    n->Path = strdup(Path);
    if (!LoadFe(L, &F, Path, 1, U_DATA)) return;
    n->Links = F.Links;
    n->UniqueId = F.UniqueId;
    if (F.UniqueId > MaxUnique) MaxUnique = F.UniqueId;
    n->IsDir = F.FileType == UDF_FT_DIRECTORY || F.FileType == UDF_FT_STREAMDIR;
    if (!ViaStreamDir) { if (n->IsDir) CountDirs++; }
    /* re-mark a directory's data as directory blocks (cosmetic) */
    FreeFe(&F);
    if (n->IsDir) CheckDir(L, Parent, Path, 0);
    /* named streams, extended attribute files */
    if (LoadFe(L, &F, Path, 0, 0)) {
        if (F.HasStream) {
            char *sp = Join(Path, ":streams");
            NODE *s = Node(&F.StreamDir, 1);
            s->FidRefs++;
            if (!s->Visited) {
                FE S;
                s->Visited = 1;
                s->Path = sp;
                s->IsDir = 1;
                if (LoadFe(&F.StreamDir, &S, sp, 1, U_DIR)) {
                    if (S.FileType != UDF_FT_STREAMDIR) Err("%s: the stream directory has file type %u", sp, S.FileType);
                    s->Links = S.Links;
                    FreeFe(&S);
                    CheckDir(&F.StreamDir, L, sp, 1);
                }
            } else free(sp);
        }
        if (F.HasEa) {
            FE E;
            char *ep = Join(Path, ":ea");
            if (LoadFe(&F.EaIcb, &E, ep, 1, U_DATA)) FreeFe(&E);
            free(ep);
        }
        FreeFe(&F);
    }
}

static void
CheckDir(const UDF_LOC *L, const UDF_LOC *Parent, const char *Path, int Streams)
{
    FE F;
    UCHAR *d;
    ULONGLONG off = 0;
    int sawParent = 0;
    NODE *self = Node(L, 1);
    if (!LoadFe(L, &F, Path, 0, 0)) return;
    if (F.Size > (256u << 20)) { Err("%s: a directory of %llu bytes", Path, (unsigned long long)F.Size); FreeFe(&F); return; }
    d = (UCHAR *)malloc((size_t)F.Size + 512);
    if (!ReadData(&F, 0, (ULONG)F.Size, d)) { Err("%s: cannot read the directory", Path); free(d); FreeFe(&F); return; }
    while (off + UDF_FID_FIXED <= F.Size) {
        UCHAR *f = d + off;
        ULONG liu, lfi, len, l;
        UDF_LOC icb;
        UCHAR ch;
        char name[UDF_MAX_NAME_UTF8 + 1];
        if (UdfGet16(f) != UDF_TAG_FID) { Err("%s: no file identifier at offset %llu", Path, (unsigned long long)off); break; }
        liu = UdfGet16(f + UDF_FID_LIU);
        lfi = f[UDF_FID_LFI];
        len = (UDF_FID_FIXED + liu + lfi + 3) & ~3u;
        if (off + UDF_FID_FIXED + liu + lfi > F.Size) { Err("%s: a file identifier runs past the end", Path); break; }
        if (!UdfTagCheck(f, (ULONG)(F.Size - off < len ? F.Size - off : len), UDF_TAG_FID, (ULONG)~0UL))
            Err("%s: a file identifier at offset %llu has a bad tag", Path, (unsigned long long)off);
        else if (UdfGet16(f + 10) != len - 16 && off + len <= F.Size)
            Warn("%s: a file identifier's CRC covers %u bytes, not %lu", Path, UdfGet16(f + 10), (unsigned long)(len - 16));
        ch = f[UDF_FID_CHAR];
        UdfLongAdGet(f + UDF_FID_ICB, &l, &icb);
        UdfCs0ToUtf8(f + UDF_FID_FIXED + liu, lfi, name, sizeof(name) - 1);
        off += len;
        if (ch & UDF_FID_DELETED) continue;
        if (ch & UDF_FID_PARENT) {
            if (sawParent) Err("%s: two parent entries", Path);
            sawParent = 1;
            if (Parent && (icb.Lbn != Parent->Lbn || icb.Part != Parent->Part) &&
                !(Streams && icb.Lbn == L->Lbn && icb.Part == L->Part))     /* a system stream directory: itself */
                Err("%s: its parent entry points to %u:%lu, not to %u:%lu", Path, icb.Part, (unsigned long)icb.Lbn, Parent->Part, (unsigned long)Parent->Lbn);
            continue;
        }
        if (lfi == 0) { Err("%s: an entry without a name", Path); continue; }
        if (!(l & UDF_EXT_LEN_MASK)) { Err("%s/%s: the entry points nowhere", Path, name); continue; }
        {
            char *p = Join(Path, name);
            NODE *c;
            ULONG uid32 = UdfGet32(f + UDF_FID_ICB + 12);
            Visit(&icb, L, p, Streams);
            c = Node(&icb, 0);
            if (c) {
                if (c->IsDir != !!(ch & UDF_FID_DIRECTORY))
                    Err("%s: the entry says %s, the file entry %s", p, (ch & UDF_FID_DIRECTORY) ? "directory" : "file", c->IsDir ? "directory" : "file");
                self = Node(L, 1);              /* (the table may have moved) */
                if (c->IsDir && !Streams) self->SubDirs++;
                if (!c->IsDir && !Streams) CountFiles++;
                if (uid32 && uid32 != (ULONG)c->UniqueId && Verbose)
                    Warn("%s: the entry's unique ID %lu differs from the file's %llu", p, (unsigned long)uid32, (unsigned long long)c->UniqueId);
            }
            free(p);
        }
    }
    if (!sawParent && !Streams) Err("%s: no parent entry", Path);
    free(d);
    FreeFe(&F);
}

/* ---- descriptors ---------------------------------------------------------------- */
static int
FindVolume(void)
{
    static const ULONG sizes[] = { 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536 };
    UCHAR t[512];
    ULONG i, k;
    for (i = 0; i < 8; i++) {
        ULONG b = sizes[i], step = b < 2048 ? 2048 : b, nsr = 0, bea = 0;
        ULONGLONG n = Img.Size / b;
        if (b < Img.Sector) continue;
        for (k = 0; k < 32; k++) {
            if (!UimgRead(&Img, 32768 + (ULONGLONG)k * step, t, 8)) break;
            if (!memcmp(t + 1, "BEA01", 5)) bea = 1;
            if (bea && (!memcmp(t + 1, "NSR02", 5) || !memcmp(t + 1, "NSR03", 5))) nsr = 1;
            if (!memcmp(t + 1, "TEA01", 5)) break;
        }
        if (!nsr) continue;
        B = b;
        for (Shift = 0; (1u << Shift) < B; Shift++)
            ;
        LastBlock = (ULONG)(n - 1);
        if (UimgRead(&Img, 256ULL * b, t, 512) && UdfTagCheck(t, 512, UDF_TAG_AVDP, 256)) return 1;
        if (UimgRead(&Img, (n - 1) * b, t, 512) && UdfTagCheck(t, 512, UDF_TAG_AVDP, (ULONG)(n - 1))) return 1;
    }
    return 0;
}

typedef struct {
    UCHAR Pvd[512], Lvd[4096], Pd[4][512], Iuvd[512];
    int HavePvd, HaveLvd, NPd, HaveIuvd;
    ULONG Blocks[16];
} VDSET;

static int
ReadVds(ULONG Loc, ULONG Len, VDSET *S, const char *Which)
{
    ULONG i, n = Len >> Shift;
    UCHAR *b = (UCHAR *)malloc(8192 + B);
    memset(S, 0, sizeof(*S));
    for (i = 0; i < n; i++) {
        USHORT id;
        ULONG need, have = B;
        if (!ReadBlk(Loc + i, 1, b)) { Err("%s: cannot read block %lu", Which, (unsigned long)(Loc + i)); break; }
        id = UdfGet16(b);
        need = 16 + UdfGet16(b + 10);
        if (need > have && need <= 8192) { ReadBlk(Loc + i + 1, (need - have + B - 1) >> Shift, b + B); have += ((need - have + B - 1) >> Shift) << Shift; }
        if (id == 0 && i > 0) break;
        if (!UdfTagCheck(b, have, id, Loc + i)) { Err("%s: a damaged descriptor in block %lu", Which, (unsigned long)(Loc + i)); continue; }
        if (id == UDF_TAG_PVD) { memcpy(S->Pvd, b, 512); S->HavePvd = 1; }
        else if (id == UDF_TAG_LVD) { memcpy(S->Lvd, b, need < 4096 ? (need > 512 ? need : 512) : 4096); S->HaveLvd = 1; }
        else if (id == UDF_TAG_PD && S->NPd < 4) memcpy(S->Pd[S->NPd++], b, 512);
        else if (id == UDF_TAG_IUVD) { memcpy(S->Iuvd, b, 512); S->HaveIuvd = 1; }
        else if (id == UDF_TAG_TD) break;
        if (have > B) i += (have >> Shift) - 1;
    }
    free(b);
    return S->HaveLvd && S->NPd;
}

static ULONG *
ReadExtents(const UDF_LOC *L, ULONG **Lens, ULONG *N, const char *What, UCHAR Use, int Mark)
{
    FE F;
    ULONG *lbn, i;
    if (!LoadFe(L, &F, What, Mark, Use)) { *N = 0; *Lens = NULL; return NULL; }
    lbn = (ULONG *)malloc((F.NX + 1) * sizeof(ULONG));
    *Lens = (ULONG *)malloc((F.NX + 1) * sizeof(ULONG));
    for (i = 0; i < F.NX; i++) { lbn[i] = F.X[i].Lbn; (*Lens)[i] = F.X[i].Len; }
    *N = F.NX;
    FreeFe(&F);
    return lbn;
}

/* ---- the space bitmaps ------------------------------------------------------------ */
typedef struct {
    UCHAR *Sbd;             /* the descriptor as read (header + bitmap) */
    ULONG  Bytes;
    ULONG  Bits;
} SBD;

static int
LoadSbd(USHORT Part, SBD *S)
{
    PART *p = &P[Part];
    memset(S, 0, sizeof(*S));
    if (p->Kind == UDF_KIND_META) {
        UDF_LOC l;
        FE F;
        l.Part = p->Under;
        l.Lbn = p->MetaBitmap;
        if (!LoadFe(&l, &F, "(metadata bitmap)", 0, 0)) return 0;
        S->Bytes = (ULONG)F.Size;
        S->Sbd = (UCHAR *)calloc(1, S->Bytes + B);
        if (!ReadData(&F, 0, S->Bytes, S->Sbd)) { FreeFe(&F); return 0; }
        FreeFe(&F);
    } else {
        ULONG i;
        S->Bytes = p->BitmapBlocks << Shift;
        S->Sbd = (UCHAR *)calloc(1, S->Bytes + B);
        for (i = 0; i < p->BitmapBlocks; i++) if (!ReadPart(Part, p->BitmapLbn + i, S->Sbd + (i << Shift))) return 0;
    }
    if (UdfGet16(S->Sbd) != UDF_TAG_SBD) { Err("partition %u: no space bitmap descriptor", Part); return 0; }
    if (!UdfTagCheck(S->Sbd, S->Bytes, UDF_TAG_SBD, (ULONG)~0UL)) Err("partition %u: the space bitmap's tag is damaged", Part);
    S->Bits = UdfGet32(S->Sbd + UDF_SBD_BITS);
    if (S->Bits != p->Length) Err("partition %u: the bitmap has %lu bits for %lu blocks", Part, (unsigned long)S->Bits, (unsigned long)p->Length);
    return 1;
}

static int
StoreSbd(USHORT Part, SBD *S)
{
    PART *p = &P[Part];
    ULONG loc = p->Kind == UDF_KIND_META ? (p->BitmapEmbedded ? p->MetaBitmap : p->NBExt ? p->BExtLbn[0] : 0) : p->BitmapLbn;
    UdfPut32(S->Sbd + UDF_SBD_BITS, p->Length);
    UdfPut32(S->Sbd + UDF_SBD_BYTES, (p->Length + 7) / 8);
    UdfTagMake(S->Sbd, UDF_TAG_SBD, TagVer, TagSerial, loc, UDF_SBD_MAP);
    if (p->Kind == UDF_KIND_META && p->BitmapEmbedded) {
        /* back into the file entry, whose tag then changes too */
        UCHAR *b = (UCHAR *)malloc(B);
        ULONG hdr, lea, lad;
        ULONGLONG a;
        int ok;
        if (!Translate(p->Under, p->MetaBitmap, &a) || !ReadBlk(a, 1, b)) { free(b); return 0; }
        hdr = UdfGet16(b) == UDF_TAG_EFE ? UDF_EFE_EA : UDF_FE_EA;
        lea = UdfGet32(b + (hdr == UDF_EFE_EA ? UDF_EFE_LEA : UDF_FE_LEA));
        lad = UdfGet32(b + (hdr == UDF_EFE_EA ? UDF_EFE_LAD : UDF_FE_LAD));
        if (lad != S->Bytes || hdr + lea + lad > B) { free(b); return 0; }
        memcpy(b + hdr + lea, S->Sbd, lad);
        UdfTagMake(b, UdfGet16(b), UdfGet16(b + 2), UdfGet16(b + 6), p->MetaBitmap, hdr + lea + lad);
        ok = WriteBlk(a, 1, b);
        free(b);
        return ok;
    }
    if (p->Kind == UDF_KIND_META) {
        ULONG blk, nblk = (S->Bytes + B - 1) >> Shift;
        for (blk = 0; blk < nblk; blk++) {
            ULONG l;
            ULONGLONG a;
            if (!MapExt(p->BExtLbn, p->BExtLen, p->NBExt, blk, &l) || !Translate(p->Under, l, &a) || !WriteBlk(a, 1, S->Sbd + (blk << Shift))) return 0;
        }
    } else {
        ULONG i;
        for (i = 0; i < p->BitmapBlocks; i++) {
            ULONGLONG a;
            if (!Translate(Part, p->BitmapLbn + i, &a) || !WriteBlk(a, 1, S->Sbd + (i << Shift))) return 0;
        }
    }
    return 1;
}

static void
CompareBitmap(USHORT Part, ULONG *FreeCount)
{
    PART *p = &P[Part];
    SBD S;
    ULONG i, lost = 0, bad = 0, freeN = 0, firstBad = 0;
    if (!p->HasBitmap) return;
    if (!LoadSbd(Part, &S)) { Err("partition %u: cannot read its space bitmap", Part); return; }
    for (i = 0; i < p->Length && i < S.Bits; i++) {
        int isFree = (S.Sbd[UDF_SBD_MAP + (i >> 3)] >> (i & 7)) & 1;
        if (isFree && p->Used[i]) { if (!bad) firstBad = i; bad++; }
        else if (!isFree && !p->Used[i]) lost++;
    }
    for (i = 0; i < p->Length; i++) if (!p->Used[i]) freeN++;
    *FreeCount = freeN;
    if (bad) Err("partition %u: %lu blocks in use are marked free (the first: %s at %lu)", Part, (unsigned long)bad,
                 UseName[p->Used[firstBad]], (unsigned long)firstBad);
    if (lost) Warn("partition %u: %lu blocks marked used are not used by anything", Part, (unsigned long)lost);
    if ((bad || lost) && Fix) {
        for (i = 0; i < p->Length; i++) {
            UCHAR *c = &S.Sbd[UDF_SBD_MAP + (i >> 3)];
            if (p->Used[i]) *c &= (UCHAR)~(1u << (i & 7)); else *c |= (UCHAR)(1u << (i & 7));
        }
        if (StoreSbd(Part, &S)) { Fixed += bad + (lost ? 1 : 0); printf("  partition %u: the space bitmap was rewritten.\n", Part); }
        else Err("partition %u: cannot write the space bitmap", Part);
    }
    free(S.Sbd);
}

static void
CompareMirror(PART *p, UCHAR *b)
{
    ULONG k;
    /* the two copies of every block in use: equal */
    {
                    ULONG blk, total = 0, diff = 0;
                    UCHAR *b2 = (UCHAR *)malloc(B);
                    for (k = 0; k < p->NMExt; k++) total += (p->MExtLen[k] + B - 1) >> Shift;
                    for (blk = 0; blk < total; blk++) {
                        ULONG l1, l2;
                        ULONGLONG a1, a2;
                        if (!MapExt(p->MExtLbn, p->MExtLen, p->NMExt, blk, &l1) || !MapExt(p->RExtLbn, p->RExtLen, p->NRExt, blk, &l2)) break;
                        if (l1 == l2 || blk >= p->Length || !p->Used[blk]) continue;
                        if (!Translate(p->Under, l1, &a1) || !Translate(p->Under, l2, &a2) || !ReadBlk(a1, 1, b) || !ReadBlk(a2, 1, b2)) break;
                        if (memcmp(b, b2, B)) {
                            if (!diff) Err("the metadata mirror differs from the metadata file (first at block %lu: tags %u / %u)",
                                           (unsigned long)blk, UdfGet16(b), UdfGet16(b2));
                            diff++;
                        }
                    }
                    free(b2);
    }
}

static int
Usage(void)
{
    printf("Checks a UDF volume.\n\n"
           "udfchk drive: [/F] [/V]\n"
           "udfchk \\\\.\\PhysicalDriveN /P:n [/F] [/V]\n"
           "udfchk image-file [/P:n] [/F] [/V]\n\n"
           "  /F    fix what can be fixed (space bitmaps, link counts, the integrity descriptor)\n"
           "  /V    verbose\n"
           "  /Q    quiet\n");
    return 8;
}

int
main(int argc, char **argv)
{
    const char *Target = NULL;
    int Slot = -1, i;
    UCHAR avdp[512];
    VDSET S, R;
    const UCHAR *lvd;
    ULONG nmaps, off, j;
    UDF_LOC fsdLoc, root;
    ULONG len;
    UCHAR *b;
    ULONG freeTable[UDF_MAX_MAPS];
    int lvidOpen = 0;

    for (i = 1; i < argc; i++) {
        const char *A = argv[i];
        if ((A[0] == '/' || A[0] == '-') && A[1] && (A[2] == 0 || A[2] == ':')) {
            char C = (char)((A[1] >= 'a' && A[1] <= 'z') ? A[1] - 32 : A[1]);
            if (C == 'F' && !A[2]) Fix = 1;
            else if (C == 'V' && !A[2]) Verbose = 1;
            else if (C == 'Q' && !A[2]) Quiet = 1;
            else if (C == 'P' && A[2] == ':') Slot = atoi(A + 3) - 1;
            else return Usage();
        } else if (!Target) Target = A;
        else return Usage();
    }
    if (!Target) return Usage();
    if (!UimgOpen(&Img, Target, Slot, !Fix)) { printf("Cannot open %s.\n", Target); return 8; }
    if (Fix && !UimgLock(&Img)) { printf("Cannot lock %s: it is in use.\n", Target); return 8; }
    if (!FindVolume()) { printf("No UDF volume found.\n"); return 8; }
    b = (UCHAR *)malloc(B > 512 ? B : 512);
    memset(freeTable, 0xFF, sizeof(freeTable));

    if (!Quiet) printf("Checking the volume descriptors...\n");
    if (!UimgRead(&Img, 256ULL << Shift, avdp, 512) || !UdfTagCheck(avdp, 512, UDF_TAG_AVDP, 256)) {
        Warn("the anchor at block 256 is missing or damaged");
        UimgRead(&Img, (ULONGLONG)LastBlock << Shift, avdp, 512);
    }
    if (!UimgRead(&Img, (ULONGLONG)LastBlock << Shift, b, 512) || !UdfTagCheck(b, 512, UDF_TAG_AVDP, LastBlock)) {
        if (!UimgRead(&Img, (ULONGLONG)(LastBlock - 256) << Shift, b, 512) || !UdfTagCheck(b, 512, UDF_TAG_AVDP, LastBlock - 256))
            Warn("no anchor at the end of the volume (block %lu or %lu)", (unsigned long)LastBlock, (unsigned long)(LastBlock - 256));
    }
    TagVer = UdfGet16(avdp + 2);
    TagSerial = UdfGet16(avdp + 6);
    if (!ReadVds(UdfGet32(avdp + 20), UdfGet32(avdp + 16), &S, "main volume descriptor sequence")) {
        Err("the main volume descriptor sequence is incomplete");
        if (!ReadVds(UdfGet32(avdp + 28), UdfGet32(avdp + 24), &S, "reserve volume descriptor sequence")) { printf("No usable volume descriptors.\n"); return 8; }
    } else if (!ReadVds(UdfGet32(avdp + 28), UdfGet32(avdp + 24), &R, "reserve volume descriptor sequence")) {
        Err("the reserve volume descriptor sequence is incomplete");
    } else if (memcmp(R.Lvd + 16, S.Lvd + 16, 440 - 16) || memcmp(R.Pd[0] + 16, S.Pd[0] + 16, 512 - 16)) {
        Warn("the reserve volume descriptors differ from the main ones");
    }
    if (!S.HavePvd) Err("no primary volume descriptor");
    if (!S.HaveIuvd) Warn("no implementation use volume descriptor (*UDF LV Info)");
    lvd = S.Lvd;
    if (UdfGet32(lvd + UDF_LVD_BLOCKSIZE) != B) { printf("The logical block size (%lu) is not the one found (%lu).\n", (unsigned long)UdfGet32(lvd + UDF_LVD_BLOCKSIZE), (unsigned long)B); return 8; }
    if (!UdfRegidIs(lvd + UDF_LVD_DOMAIN, UDF_ID_COMPLIANT)) Err("the domain is not \"*OSTA UDF Compliant\"");
    Revision = UdfGet16(lvd + UDF_LVD_DOMAIN + 24);
    if (!Quiet) {
        char label[400];
        UdfDstringToUtf8(lvd + UDF_LVD_IDENT, 128, label, sizeof(label));
        printf("UDF %x.%02x, block size %lu, %lu blocks, label \"%s\".\n", Revision >> 8, Revision & 0xFF,
               (unsigned long)B, (unsigned long)(LastBlock + 1), label);
    }

    /* partitions */
    nmaps = UdfGet32(lvd + UDF_LVD_NMAPS);
    if (nmaps == 0 || nmaps > UDF_MAX_MAPS) { printf("%lu partition maps: not supported.\n", (unsigned long)nmaps); return 8; }
    for (i = 0, off = UDF_LVD_MAPS; i < (int)nmaps; i++) {
        const UCHAR *pm = lvd + off;
        PART *p = &P[i];
        const UCHAR *pd = NULL;
        memset(p, 0, sizeof(*p));
        p->Under = (USHORT)i;
        if (pm[0] == 1) { p->Kind = UDF_KIND_PHYS; p->Number = UdfGet16(pm + 4); }
        else {
            p->Number = UdfGet16(pm + UDF_PM2_PARTNUM);
            if (UdfRegidIs(pm + 4, UDF_ID_SPARABLE)) p->Kind = UDF_KIND_SPAR;
            else if (UdfRegidIs(pm + 4, UDF_ID_VIRTUAL)) p->Kind = UDF_KIND_VIRT;
            else if (UdfRegidIs(pm + 4, UDF_ID_METADATA)) {
                p->Kind = UDF_KIND_META;
                p->MetaFile = UdfGet32(pm + UDF_PM_META_FILE);
                p->MetaMirror = UdfGet32(pm + UDF_PM_META_MIRROR);
                p->MetaBitmap = UdfGet32(pm + UDF_PM_META_BITMAP);
                p->MetaFlags = pm[UDF_PM_META_FLAGS];
            } else { printf("Partition map %d is of an unknown type.\n", i); return 8; }
        }
        for (j = 0; j < (ULONG)S.NPd; j++) if (UdfGet16(S.Pd[j] + UDF_PD_NUMBER) == p->Number) pd = S.Pd[j];
        if (!pd) { printf("No partition descriptor for partition %u.\n", p->Number); return 8; }
        p->Start = UdfGet32(pd + UDF_PD_START);
        p->Length = UdfGet32(pd + UDF_PD_LENGTH);
        if ((ULONGLONG)p->Start + p->Length > (ULONGLONG)LastBlock + 1) Err("partition %u runs past the end of the volume", p->Number);
        if (p->Kind == UDF_KIND_PHYS || p->Kind == UDF_KIND_SPAR) {
            ULONG bl = UdfGet32(pd + UDF_PD_PHD + UDF_PHD_USB);
            if (bl & UDF_EXT_LEN_MASK) {
                p->HasBitmap = 1;
                p->BitmapLbn = UdfGet32(pd + UDF_PD_PHD + UDF_PHD_USB + 4);
                p->BitmapBlocks = ((bl & UDF_EXT_LEN_MASK) + B - 1) >> Shift;
            }
            p->Used = (UCHAR *)calloc(p->Length + 1, 1);
            if (p->Kind == UDF_KIND_SPAR) {
                ULONG nt = pm[UDF_PM_SPAR_NTABLES], sz = UdfGet32(pm + UDF_PM_SPAR_TABLESIZE), t, nb = (sz + B - 1) >> Shift;
                UCHAR *tb = (UCHAR *)malloc((nb + 1) << Shift);
                p->Packet = UdfGet16(pm + UDF_PM_SPAR_PACKET);
                for (t = 0; t < nt && t < 4; t++) {
                    ULONG loc = UdfGet32(pm + UDF_PM_SPAR_TABLES + 4 * t), k;
                    if (!ReadBlk(loc, nb, tb) || !UdfTagCheck(tb, nb << Shift, 0, loc)) { Err("sparing table %lu (block %lu) is damaged", (unsigned long)t, (unsigned long)loc); continue; }
                    if (p->SpOrig) continue;
                    p->NSpare = UdfGet16(tb + UDF_SPT_COUNT);
                    p->SpOrig = (ULONG *)malloc((p->NSpare + 1) * 4);
                    p->SpMap = (ULONG *)malloc((p->NSpare + 1) * 4);
                    for (k = 0; k < p->NSpare; k++) { p->SpOrig[k] = UdfGet32(tb + UDF_SPT_MAP + 8 * k); p->SpMap[k] = UdfGet32(tb + UDF_SPT_MAP + 8 * k + 4); }
                }
                free(tb);
            }
        }
        off += pm[1];
    }
    NP = nmaps;
    for (i = 0; i < (int)NP; i++) {
        PART *p = &P[i];
        if (p->Kind == UDF_KIND_VIRT || p->Kind == UDF_KIND_META) {
            for (j = 0; j < NP; j++) if ((P[j].Kind == UDF_KIND_PHYS || P[j].Kind == UDF_KIND_SPAR) && P[j].Number == p->Number) p->Under = (USHORT)j;
        }
    }
    for (i = 0; i < (int)NP; i++) {
        PART *p = &P[i];
        if (p->HasBitmap) MarkUsed((USHORT)i, p->BitmapLbn, p->BitmapBlocks, U_META, "the space bitmap");
        if (p->Kind == UDF_KIND_META) {
            UDF_LOC l;
            l.Part = p->Under;
            l.Lbn = p->MetaFile;
            p->MExtLbn = ReadExtents(&l, &p->MExtLen, &p->NMExt, "(metadata file)", U_META, 1);
            if (p->MetaMirror != 0xFFFFFFFF) {
                ULONG k;
                int inside = 0;
                l.Lbn = p->MetaMirror;
                /* the mirror's entry may lie inside the metadata file (Windows 11): marked with it */
                for (k = 0; k < p->NMExt; k++)
                    if (p->MetaMirror >= p->MExtLbn[k] && p->MetaMirror < p->MExtLbn[k] + ((p->MExtLen[k] + B - 1) >> Shift)) inside = 1;
                /* the mirror's own extent is marked unless it shares the metadata file's */
                p->RExtLbn = ReadExtents(&l, &p->RExtLen, &p->NRExt, "(metadata mirror)", 0, inside ? 2 : 1);
                if (Fix && p->NRExt == 0 && p->NMExt && !(p->MetaFlags & 1)) {
                    /* the mirror shares the metadata file's blocks: its entry is the metadata
                       file's with the mirror's type (Windows 11 can fail before writing it) */
                    UCHAR *fb = (UCHAR *)malloc(B);
                    ULONGLONG a1, a2;
                    if (Translate(p->Under, p->MetaFile, &a1) && ReadBlk(a1, 1, fb) && Translate(p->Under, p->MetaMirror, &a2)) {
                        fb[UDF_ICBTAG + UDF_ICBTAG_FILETYPE] = UDF_FT_METAMIRROR;
                        UdfTagMake(fb, UdfGet16(fb), UdfGet16(fb + 2), UdfGet16(fb + 6), p->MetaMirror, 16 + UdfGet16(fb + 10));
                        if (WriteBlk(a2, 1, fb)) {
                            Fixed++;
                            printf("  the metadata mirror's file entry was rewritten.\n");
                            free(p->RExtLbn); free(p->RExtLen);
                            p->RExtLbn = ReadExtents(&l, &p->RExtLen, &p->NRExt, "(metadata mirror)", 0, inside ? 2 : 1);
                        }
                    }
                    free(fb);
                }
                {
                    /* the same length; the extents themselves may be split differently */
                    ULONGLONG mt = 0, rt = 0;
                    for (k = 0; k < p->NMExt; k++) mt += (p->MExtLen[k] + B - 1) >> Shift;
                    for (k = 0; k < p->NRExt; k++) rt += (p->RExtLen[k] + B - 1) >> Shift;
                    if (p->NRExt && rt != mt) Err("the metadata mirror has %llu blocks, the metadata file %llu", rt, mt);
                }
                for (k = 0; k < p->NRExt; k++) {
                    /* the mirror's blocks are marked unless they are the metadata file's (shared) */
                    ULONG n = (p->RExtLen[k] + B - 1) >> Shift, j, m;
                    for (j = 0; j < n; j++) {
                        ULONG x = p->RExtLbn[k] + j;
                        int shared = 0;
                        for (m = 0; m < p->NMExt && !shared; m++)
                            if (x >= p->MExtLbn[m] && x < p->MExtLbn[m] + ((p->MExtLen[m] + B - 1) >> Shift)) shared = 1;
                        if (!shared) MarkUsed(p->Under, x, 1, U_META, "(metadata mirror)");
                    }
                }
                if ((p->MetaFlags & 1) && p->NRExt && p->NMExt && p->RExtLbn[0] == p->MExtLbn[0]) Warn("the metadata is marked duplicated, but the mirror shares its blocks");
            }
            if (p->MetaBitmap != 0xFFFFFFFF) {
                l.Lbn = p->MetaBitmap;
                p->BExtLbn = ReadExtents(&l, &p->BExtLen, &p->NBExt, "(metadata bitmap)", U_META, 1);
                p->HasBitmap = p->NBExt > 0;
                if (!p->HasBitmap) {
                    /* Windows embeds a small bitmap in the bitmap file's entry */
                    FE F;
                    if (LoadFe(&l, &F, "(metadata bitmap)", 0, 0)) {
                        p->BitmapEmbedded = F.AdType == UDF_ICB_AD_IN_ICB;
                        p->HasBitmap = p->BitmapEmbedded;
                        FreeFe(&F);
                    }
                }
            }
            {
                ULONG k, total = 0;
                for (k = 0; k < p->NMExt; k++) total += (p->MExtLen[k] + B - 1) >> Shift;
                p->Length = total;
            }
            p->Used = (UCHAR *)calloc(p->Length + 1, 1);
            {
                /* the partition's own file entries inside the metadata file (Windows 11 records
                   the mirror's there): those blocks of the metadata partition are in use */
                ULONG fe[3], f, k, at;
                fe[0] = p->MetaFile; fe[1] = p->MetaMirror; fe[2] = p->MetaBitmap;
                for (f = 0; f < 3; f++) {
                    if (fe[f] == 0xFFFFFFFF) continue;
                    for (k = 0, at = 0; k < p->NMExt; k++) {
                        ULONG n = (p->MExtLen[k] + B - 1) >> Shift;
                        if (fe[f] >= p->MExtLbn[k] && fe[f] < p->MExtLbn[k] + n) {
                            ULONG m = at + fe[f] - p->MExtLbn[k];
                            if (m < p->Length && !p->Used[m]) { p->Used[m] = U_META; p->UsedCount++; }
                        }
                        at += n;
                    }
                }
            }
        } else if (p->Kind == UDF_KIND_VIRT) {
            Warn("a virtual (VAT) partition: write-once media; only the tree is checked");
            p->Vat = NULL;
        }
    }
    if (P[0].Kind == UDF_KIND_VIRT || (NP > 1 && P[1].Kind == UDF_KIND_VIRT)) {
        printf("Write-once (VAT) volumes are not checked.\n");
        return 0;
    }

    /* the integrity descriptor */
    if (!Quiet) printf("Checking the integrity descriptor...\n");
    {
        ULONG loc = UdfGet32(lvd + UDF_LVD_INTEGRITY + 4), ilen = UdfGet32(lvd + UDF_LVD_INTEGRITY), k, hops = 0;
        Lvid = (UCHAR *)malloc(B);
        while (ilen && hops++ < 16) {
            ULONG nloc = 0, nlen = 0;
            for (k = 0; k < (ilen >> Shift); k++) {
                if (!ReadBlk(loc + k, 1, b) || !UdfTagCheck(b, B, UDF_TAG_LVID, loc + k)) break;
                memcpy(Lvid, b, B);
                LvidBlock = loc + k;
                nlen = UdfGet32(b + UDF_LVID_NEXT);
                nloc = UdfGet32(b + UDF_LVID_NEXT + 4);
            }
            loc = nloc;
            ilen = nlen;
        }
        if (!LvidBlock) Err("no logical volume integrity descriptor");
        else if (UdfGet32(Lvid + UDF_LVID_TYPE) != UDF_LVID_CLOSE) {
            lvidOpen = 1;
            Warn("the volume was not closed (in use, or not unmounted cleanly)");
        }
    }

    /* the file set and the tree */
    if (!Quiet) printf("Checking the files and directories...\n");
    Hash = (ULONG *)calloc(HashSize, sizeof(ULONG));
    UdfLongAdGet(lvd + UDF_LVD_FSD, &len, &fsdLoc);
    if (!ReadPart(fsdLoc.Part, fsdLoc.Lbn, b) || !UdfTagCheck(b, B, UDF_TAG_FSD, fsdLoc.Lbn)) { printf("The file set descriptor is damaged.\n"); return 8; }
    MarkUsed(fsdLoc.Part, fsdLoc.Lbn, 1, U_META, "the file set descriptor");
    {
        UCHAR *t = (UCHAR *)malloc(B);
        ULONG k, n = (len & UDF_EXT_LEN_MASK) >> Shift;
        for (k = 1; k < n; k++) {
            if (!ReadPart(fsdLoc.Part, fsdLoc.Lbn + k, t)) break;
            if (UdfTagCheck(t, B, UDF_TAG_TD, fsdLoc.Lbn + k) || UdfTagCheck(t, B, UDF_TAG_FSD, fsdLoc.Lbn + k)) MarkUsed(fsdLoc.Part, fsdLoc.Lbn + k, 1, U_META, "the file set descriptor sequence");
            else break;
        }
        free(t);
    }
    UdfLongAdGet(b + UDF_FSD_ROOT, &len, &root);
    {
        UDF_LOC ss;
        ULONG sl;
        UdfLongAdGet(b + UDF_FSD_SSDIR, &sl, &ss);
        Visit(&root, &root, "", 0);
        if ((sl & UDF_EXT_LEN_MASK) && Revision >= UDF_REV_200) {
            NODE *s = Node(&ss, 1);
            FE S2;
            s->FidRefs++;
            s->Visited = 1;
            s->IsDir = 1;
            s->Path = strdup(":system streams");
            if (LoadFe(&ss, &S2, s->Path, 1, U_DIR)) { s->Links = S2.Links; FreeFe(&S2); CheckDir(&ss, &fsdLoc, s->Path, 1); }
        }
    }
    {
        NODE *r = Node(&root, 0);
        if (r) r->FidRefs--;            /* the root has no name: its own count starts at 1 */
    }

    /* link counts */
    if (!Quiet) printf("Checking link counts...\n");
    for (j = 0; j < NNodes; j++) {
        NODE *n = &Nodes[j];
        ULONG want = n->IsDir ? 1 + n->SubDirs : n->FidRefs;
        if (n->IsDir && n->Path && n->Path[0] == ':') continue;
        if (n->Links != want) {
            int ok = 0;
            /* some writers count a directory's own name only, or with "." */
            if (n->IsDir && (n->Links == want + 1 || n->Links == 1)) { if (Verbose) Warn("%s: link count %u, %lu expected", n->Path ? n->Path : "/", n->Links, (unsigned long)want); ok = 1; }
            if (!ok) {
                Err("%s: link count %u, %lu expected", n->Path && n->Path[0] ? n->Path : "/", n->Links, (unsigned long)want);
                if (Fix) {
                    ULONGLONG a;
                    if (Translate(n->Loc.Part, n->Loc.Lbn, &a) && ReadBlk(a, 1, b)) {
                        USHORT id = UdfGet16(b);
                        UdfPut16(b + UDF_FE_LINKS, (USHORT)want);
                        UdfTagMake(b, id, UdfGet16(b + 2), UdfGet16(b + 6), n->Loc.Lbn, 16 + UdfGet16(b + 10));
                        if (WriteBlk(a, 1, b)) {
                            /* the metadata mirror too */
                            PART *p = &P[n->Loc.Part];
                            if (p->Kind == UDF_KIND_META && p->NRExt) {
                                ULONG l2;
                                ULONGLONG a2;
                                if (MapExt(p->RExtLbn, p->RExtLen, p->NRExt, n->Loc.Lbn, &l2) && Translate(p->Under, l2, &a2) && a2 != a) WriteBlk(a2, 1, b);
                            }
                            Fixed++;
                        }
                    }
                }
            }
        }
    }

    for (i = 0; i < (int)NP; i++) if (P[i].Kind == UDF_KIND_META && P[i].NRExt) CompareMirror(&P[i], b);

    /* space */
    if (!Quiet) printf("Checking free space...\n");
    for (i = 0; i < (int)NP; i++) CompareBitmap((USHORT)i, &freeTable[i]);

    /* the integrity descriptor's numbers */
    if (LvidBlock) {
        ULONG np = UdfGet32(Lvid + UDF_LVID_NPART), il = UdfGet32(Lvid + UDF_LVID_IMPLLEN);
        int bad = lvidOpen;
        if (UdfGet64(Lvid + UDF_LVID_UNIQUEID) <= MaxUnique) {
            Err("the next unique ID (%llu) is not above the largest in use (%llu)", (unsigned long long)UdfGet64(Lvid + UDF_LVID_UNIQUEID), (unsigned long long)MaxUnique);
            bad = 1;
        }
        if (np != NP) { Err("the integrity descriptor lists %lu partitions, not %lu", (unsigned long)np, (unsigned long)NP); bad = 1; }
        else {
            for (j = 0; j < np; j++) {
                ULONG fr = UdfGet32(Lvid + UDF_LVID_TABLES + 4 * j);
                if (freeTable[j] != 0xFFFFFFFF && fr != freeTable[j]) {
                    if (lvidOpen) Warn("partition %lu: %lu free blocks recorded, %lu found", (unsigned long)j, (unsigned long)fr, (unsigned long)freeTable[j]);
                    else Err("partition %lu: %lu free blocks recorded, %lu found", (unsigned long)j, (unsigned long)fr, (unsigned long)freeTable[j]);
                    bad = 1;
                }
            }
            if (il >= UDF_LVIU_SIZE) {
                UCHAR *iu = Lvid + UDF_LVID_TABLES + 8 * np;
                ULONG nf = UdfGet32(iu + UDF_LVIU_FILES), nd = UdfGet32(iu + UDF_LVIU_DIRS);
                if (nf != CountFiles || nd != CountDirs) {
                    if (lvidOpen) Warn("%lu files and %lu directories recorded, %lu and %lu found", (unsigned long)nf, (unsigned long)nd, (unsigned long)CountFiles, (unsigned long)CountDirs);
                    else Err("%lu files and %lu directories recorded, %lu and %lu found", (unsigned long)nf, (unsigned long)nd, (unsigned long)CountFiles, (unsigned long)CountDirs);
                    bad = 1;
                }
            }
        }
        if (bad && Fix && np == NP) {
            UCHAR *iu = Lvid + UDF_LVID_TABLES + 8 * np;
            for (j = 0; j < np; j++) if (freeTable[j] != 0xFFFFFFFF) UdfPut32(Lvid + UDF_LVID_TABLES + 4 * j, freeTable[j]);
            if (UdfGet64(Lvid + UDF_LVID_UNIQUEID) <= MaxUnique) UdfPut64(Lvid + UDF_LVID_UNIQUEID, MaxUnique + 1);
            if (il >= UDF_LVIU_SIZE) { UdfPut32(iu + UDF_LVIU_FILES, CountFiles); UdfPut32(iu + UDF_LVIU_DIRS, CountDirs); }
            UdfPut32(Lvid + UDF_LVID_TYPE, UDF_LVID_CLOSE);
            UdfTagMake(Lvid, UDF_TAG_LVID, TagVer, TagSerial, LvidBlock, 16 + UdfGet16(Lvid + 10));
            if (WriteBlk(LvidBlock, 1, Lvid)) { Fixed++; printf("  the integrity descriptor was rewritten (closed).\n"); }
        }
    }

    if (!Quiet) {
        ULONG k;
        printf("\n%lu files, %lu directories.", (unsigned long)CountFiles, (unsigned long)CountDirs);
        for (k = 0; k < NP; k++) if (P[k].HasBitmap) printf(" Partition %lu: %lu of %lu blocks free.", (unsigned long)k, (unsigned long)freeTable[k], (unsigned long)P[k].Length);
        printf("\n");
    }
    if (Fix) UimgUnlock(&Img);
    UimgClose(&Img);
    if (Errors == 0) {
        if (!Quiet) printf("No errors found.\n");
        return 0;
    }
    if (Fix && Fixed) {
        printf("%lu error(s) found, %lu fixed.\n", (unsigned long)Errors, (unsigned long)Fixed);
        return 1;
    }
    printf("%lu error(s) found.\n", (unsigned long)Errors);
    return 2;
}
