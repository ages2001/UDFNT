/*
 * UDFNT - udfmkfs: lays out a new UDF volume for a hard disk partition (or
 * any random-access medium): revisions 1.02 - 2.60, blocks of 512 bytes to
 * 64 KB, a plain physical partition, a sparable one, or (2.50+) a metadata
 * partition with its mirror and bitmap.
 *
 *   block 0 ...          boot area (zeros; /BOOT puts a boot sector there)
 *   byte 32768           volume recognition sequence: BEA01, NSR02/03, TEA01
 *   block 96             main volume descriptor sequence (16 blocks)
 *   block 128            logical volume integrity sequence (16 blocks)
 *   block 256            anchor
 *   block 257 ...        the partition (sparable: the sparing tables and the
 *                        spare packets come first)
 *   block N-257          anchor
 *   block N-160          reserve volume descriptor sequence (16 blocks)
 *   block N-1            anchor
 */
#include "udfeng.h"
#include "udfmkfs.h"

#define MVDS        96
#define RVDS_BACK   160
#define LVIDS       128
#define VDS_LEN     16
#define ANCHOR      256

typedef struct {
    UDF_MKFS   *M;
    ULONG       B, Shift;
    ULONG       N;              /* volume blocks */
    PUCHAR      Blk;            /* one block */
    USHORT      Ver;            /* tag version */
    UDF_VOL     V;              /* only for the helpers (revision, time) */
    UDF_TIME    Now;
    ULONG       PStart, PLen;   /* the partition */
    ULONG       MapCount;
    /* sparable */
    ULONG       SparTable[2], SparTableBlocks, SpareStart, SpareCount;
    /* metadata */
    ULONG       MetaFeLbn, MirrorFeLbn, BitmapFeLbn;    /* in the physical partition */
    ULONG       MetaLbn, MirrorLbn, MetaLen;            /* the metadata file's and mirror's data */
    ULONG       MetaBitmapLbn, MetaBitmapBlocks;        /* the metadata bitmap file's data */
    ULONG       AllocUnit;
    /* physical partition layout */
    ULONG       SbdLbn, SbdBlocks;
    USHORT      FsPart;         /* where the file set lives (map index) */
    ULONG       FsdLbn, RootLbn;
    ULONG       PhysUsed;       /* used blocks of the physical partition, from 0 */
    PUCHAR      PhysBitmap;     /* 1 = free */
    PUCHAR      MetaBitmap;
    ULONG       MetaUsed;
    char        VolSetHex[17];
} MK;

static int
Put(MK *K, ULONGLONG Block, const VOID *Buf, ULONG Count)
{
    if (!K->M->Write(K->M->Ctx, Block << K->Shift, Buf, Count << K->Shift)) {
        K->M->Error = "write failed";
        return 0;
    }
    return 1;
}

static int
Zero(MK *K, ULONGLONG Block, ULONG Count)
{
    UdfMemZero(K->Blk, K->B);
    while (Count--) if (!Put(K, Block++, K->Blk, 1)) return 0;
    return 1;
}

static VOID
Tag(MK *K, PUCHAR D, USHORT Ident, ULONG Loc, ULONG Len)
{
    UdfTagMake(D, Ident, K->Ver, 1, Loc, Len);
}

/* ---- the volume recognition sequence ---------------------------------------- */
static int
WriteVrs(MK *K)
{
    static const char *ids[3] = { "BEA01", "NSR02", "TEA01" };
    ULONG step = K->B < 2048 ? 2048 : K->B, i;
    UCHAR d[2048];
    PUCHAR buf = (PUCHAR)UdfHostAlloc(step);
    if (!buf) return 0;
    for (i = 0; i < 3; i++) {
        UdfMemZero(buf, step);
        buf[0] = 0;
        UdfMemCopy(buf + 1, i == 1 && K->M->Revision >= UDF_REV_200 ? "NSR03" : ids[i], 5);
        buf[6] = 1;
        if (!K->M->Write(K->M->Ctx, 32768 + (ULONGLONG)i * step, buf, step)) { UdfHostFree(buf); K->M->Error = "write failed"; return 0; }
    }
    (VOID)d;
    UdfHostFree(buf);
    return 1;
}

/* ---- volume descriptors ------------------------------------------------------------ */
static VOID
Pvd(MK *K, PUCHAR D, ULONG Loc, ULONG Num)
{
    char id[64];
    ULONG n = 0, max;
    UdfMemZero(D, K->B);
    UdfPut32(D + UDF_PVD_VDSNUM, Num);
    UdfPut32(D + UDF_PVD_NUMBER, 0);
    /* the volume identifier: 32 bytes (up to 30 Latin-1 characters) */
    for (max = 0; K->M->Label[max]; max++)
        ;
    n = max;
    while (n && !UdfUtf8ToDstring(K->M->Label, n, D + UDF_PVD_VOLID, 32)) {
        n--;
        while (n && ((UCHAR)K->M->Label[n] & 0xC0) == 0x80) n--;
    }
    UdfPut16(D + UDF_PVD_VOLSEQ, 1);
    UdfPut16(D + UDF_PVD_MAXVOLSEQ, 1);
    UdfPut16(D + UDF_PVD_ILEVEL, 2);
    UdfPut16(D + UDF_PVD_MAXILEVEL, 3);
    UdfPut32(D + UDF_PVD_CSLIST, 1);
    UdfPut32(D + UDF_PVD_MAXCSLIST, 1);
    /* the volume set identifier: 16 hex digits first (UDF 2.2.2.5) */
    UdfMemCopy(id, K->VolSetHex, 16);
    UdfMemCopy(id + 16, "UDFNT", 5);
    UdfUtf8ToDstring(id, 21, D + UDF_PVD_VOLSETID, 128);
    UdfCharspec(D + UDF_PVD_DESCCS);
    UdfCharspec(D + UDF_PVD_EXPLCS);
    UdfRegid(D + UDF_PVD_APPID, UDF_ID_IMPL, NULL, 0);
    UdfTimePut(D + UDF_PVD_RECTIME, &K->Now);
    UdfRegidImpl(D + UDF_PVD_IMPL);
    Tag(K, D, UDF_TAG_PVD, Loc, 512);
}

static VOID
Iuvd(MK *K, PUCHAR D, ULONG Loc, ULONG Num)
{
    ULONG n = 0;
    UdfMemZero(D, K->B);
    UdfPut32(D + 16, Num);
    UdfRegidUdf(&K->V, D + 20, UDF_ID_LV_INFO);
    UdfCharspec(D + 52);
    while (K->M->Label[n]) n++;
    UdfUtf8ToDstring(K->M->Label, n, D + 52 + 64, 128);
    UdfRegidImpl(D + 52 + 300);
    Tag(K, D, UDF_TAG_IUVD, Loc, 512);
}

static VOID
Pd(MK *K, PUCHAR D, ULONG Loc, ULONG Num)
{
    UdfMemZero(D, K->B);
    UdfPut32(D + 16, Num);
    UdfPut16(D + UDF_PD_FLAGS, 1);                  /* allocated */
    UdfPut16(D + UDF_PD_NUMBER, 0);
    UdfRegid(D + UDF_PD_CONTENTS, K->M->Revision >= UDF_REV_200 ? UDF_ID_NSR03 : UDF_ID_NSR02, NULL, 0);
    /* the unallocated space bitmap */
    UdfPut32(D + UDF_PD_PHD + UDF_PHD_USB, K->SbdBlocks << K->Shift);
    UdfPut32(D + UDF_PD_PHD + UDF_PHD_USB + 4, K->SbdLbn);
    UdfPut32(D + UDF_PD_ACCESS, UDF_ACCESS_OVERWRITE);
    UdfPut32(D + UDF_PD_START, K->PStart);
    UdfPut32(D + UDF_PD_LENGTH, K->PLen);
    UdfRegidImpl(D + UDF_PD_IMPL);
    Tag(K, D, UDF_TAG_PD, Loc, 512);
}

static ULONG
Lvd(MK *K, PUCHAR D, ULONG Loc, ULONG Num)
{
    ULONG n = 0, off = UDF_LVD_MAPS, len;
    UCHAR suffix[8];
    UDF_LOC fsd;
    UdfMemZero(D, K->B);   /* (a buffer of at least 1024 bytes) */
    UdfPut32(D + 16, Num);
    UdfCharspec(D + UDF_LVD_CHARSET);
    while (K->M->Label[n]) n++;
    UdfUtf8ToDstring(K->M->Label, n, D + UDF_LVD_IDENT, 128);
    UdfPut32(D + UDF_LVD_BLOCKSIZE, K->M->BlockSize);
    UdfMemZero(suffix, 8);
    UdfPut16(suffix, (USHORT)K->M->Revision);
    UdfRegid(D + UDF_LVD_DOMAIN, UDF_ID_COMPLIANT, suffix, 8);
    fsd.Part = K->FsPart;
    fsd.Lbn = K->FsdLbn;
    UdfLongAdPut(D + UDF_LVD_FSD, 2 * K->M->BlockSize, &fsd);
    UdfRegidImpl(D + UDF_LVD_IMPL);
    UdfPut32(D + UDF_LVD_INTEGRITY, VDS_LEN << K->Shift);
    UdfPut32(D + UDF_LVD_INTEGRITY + 4, LVIDS);
    /* the partition maps */
    if (K->M->Sparable) {
        PUCHAR pm = D + off;
        pm[0] = UDF_PM_TYPE2;
        pm[1] = 64;
        UdfRegidUdf(&K->V, pm + UDF_PM2_IDENT, UDF_ID_SPARABLE);
        UdfPut16(pm + UDF_PM2_VOLSEQ, 1);
        UdfPut16(pm + UDF_PM2_PARTNUM, 0);
        UdfPut16(pm + UDF_PM_SPAR_PACKET, (USHORT)K->M->Packet);
        pm[UDF_PM_SPAR_NTABLES] = 2;
        UdfPut32(pm + UDF_PM_SPAR_TABLESIZE, UDF_SPT_MAP + 8 * K->SpareCount);
        UdfPut32(pm + UDF_PM_SPAR_TABLES, K->SparTable[0]);
        UdfPut32(pm + UDF_PM_SPAR_TABLES + 4, K->SparTable[1]);
        off += 64;
    } else {
        PUCHAR pm = D + off;
        pm[0] = UDF_PM_TYPE1;
        pm[1] = 6;
        UdfPut16(pm + 2, 1);
        UdfPut16(pm + 4, 0);
        off += 6;
    }
    if (K->M->Metadata) {
        PUCHAR pm = D + off;
        pm[0] = UDF_PM_TYPE2;
        pm[1] = 64;
        UdfRegidUdf(&K->V, pm + UDF_PM2_IDENT, UDF_ID_METADATA);
        UdfPut16(pm + UDF_PM2_VOLSEQ, 1);
        UdfPut16(pm + UDF_PM2_PARTNUM, 0);
        UdfPut32(pm + UDF_PM_META_FILE, K->MetaFeLbn);
        UdfPut32(pm + UDF_PM_META_MIRROR, K->MirrorFeLbn);
        UdfPut32(pm + UDF_PM_META_BITMAP, K->BitmapFeLbn);
        UdfPut32(pm + UDF_PM_META_ALLOCUNIT, K->AllocUnit);
        UdfPut16(pm + UDF_PM_META_ALIGNUNIT, 1);
        pm[UDF_PM_META_FLAGS] = 1;                   /* the mirror has a copy of its own */
        off += 64;
    }
    UdfPut32(D + UDF_LVD_MAPLEN, off - UDF_LVD_MAPS);
    UdfPut32(D + UDF_LVD_NMAPS, K->MapCount);
    len = off;
    Tag(K, D, UDF_TAG_LVD, Loc, len);
    return len;
}

static int
WriteVds(MK *K, ULONG At)
{
    PUCHAR d = K->Blk;
    ULONG i = 0, len, blocks;
    Pvd(K, d, At + i, i); if (!Put(K, At + i, d, 1)) return 0; i++;
    Iuvd(K, d, At + i, i); if (!Put(K, At + i, d, 1)) return 0; i++;
    Pd(K, d, At + i, i); if (!Put(K, At + i, d, 1)) return 0; i++;
    {
        /* the logical volume descriptor can be longer than a 512-byte block */
        PUCHAR big = (PUCHAR)UdfHostAlloc(2 * K->B > 1024 ? 2 * K->B : 1024);
        if (!big) return 0;
        UdfMemZero(big, 2 * K->B > 1024 ? 2 * K->B : 1024);
        {
            /* build in a big buffer */
            MK k2 = *K;
            k2.Blk = big;
            k2.B = K->B < 1024 ? 1024 : K->B;
            len = Lvd(&k2, big, At + i, i);
        }
        blocks = (len + K->B - 1) >> K->Shift;
        if (!Put(K, At + i, big, blocks)) { UdfHostFree(big); return 0; }
        UdfHostFree(big);
        i += blocks;
    }
    /* the unallocated space descriptor: none */
    UdfMemZero(d, K->B);
    UdfPut32(d + 16, i);
    UdfPut32(d + 20, 0);
    Tag(K, d, UDF_TAG_USD, At + i, 24);
    if (!Put(K, At + i, d, 1)) return 0;
    i++;
    UdfMemZero(d, K->B);
    Tag(K, d, UDF_TAG_TD, At + i, 512);
    if (!Put(K, At + i, d, 1)) return 0;
    i++;
    return Zero(K, At + i, VDS_LEN - i);
}

static int
WriteAnchor(MK *K, ULONG At)
{
    PUCHAR d = K->Blk;
    UdfMemZero(d, K->B);
    UdfPut32(d + 16, VDS_LEN << K->Shift);
    UdfPut32(d + 20, MVDS);
    UdfPut32(d + 24, VDS_LEN << K->Shift);
    UdfPut32(d + 28, K->N - RVDS_BACK);
    Tag(K, d, UDF_TAG_AVDP, At, 512);
    return Put(K, At, d, 1);
}

static ULONG
CountFree(const UCHAR *Bitmap, ULONG Bits)
{
    ULONG i, n = 0;
    for (i = 0; i < Bits; i++) n += (Bitmap[i >> 3] >> (i & 7)) & 1;
    return n;
}

static int
WriteLvid(MK *K)
{
    PUCHAR d = K->Blk, iu;
    ULONG np = K->MapCount, i;
    UdfMemZero(d, K->B);
    UdfTimePut(d + UDF_LVID_TIME, &K->Now);
    UdfPut32(d + UDF_LVID_TYPE, UDF_LVID_CLOSE);
    UdfPut64(d + UDF_LVID_UNIQUEID, UDF_FIRST_UNIQUE_ID);
    UdfPut32(d + UDF_LVID_NPART, np);
    UdfPut32(d + UDF_LVID_IMPLLEN, UDF_LVIU_SIZE);
    for (i = 0; i < np; i++) {
        ULONG fr, sz;
        if (i == 0) { fr = CountFree(K->PhysBitmap, K->PLen); sz = K->PLen; }
        else { fr = CountFree(K->MetaBitmap, K->MetaLen); sz = K->MetaLen; }
        UdfPut32(d + UDF_LVID_TABLES + 4 * i, fr);
        UdfPut32(d + UDF_LVID_TABLES + 4 * np + 4 * i, sz);
    }
    iu = d + UDF_LVID_TABLES + 8 * np;
    UdfRegidImpl(iu + UDF_LVIU_IMPL);
    UdfPut32(iu + UDF_LVIU_FILES, 0);
    UdfPut32(iu + UDF_LVIU_DIRS, 1);
    UdfPut16(iu + UDF_LVIU_MINREAD, (USHORT)K->M->Revision);
    UdfPut16(iu + UDF_LVIU_MINWRITE, (USHORT)K->M->Revision);
    UdfPut16(iu + UDF_LVIU_MAXWRITE, (USHORT)K->M->Revision);
    Tag(K, d, UDF_TAG_LVID, LVIDS, UDF_LVID_TABLES + 8 * np + UDF_LVIU_SIZE);
    if (!Put(K, LVIDS, d, 1)) return 0;
    return Zero(K, LVIDS + 1, VDS_LEN - 1);
}

/* ---- the file set ------------------------------------------------------------------------ */
static VOID
Fsd(MK *K, PUCHAR D)
{
    ULONG n = 0;
    UDF_LOC root;
    UdfMemZero(D, K->B);
    UdfTimePut(D + UDF_FSD_TIME, &K->Now);
    UdfPut16(D + UDF_FSD_ILEVEL, 3);
    UdfPut16(D + UDF_FSD_MAXILEVEL, 3);
    UdfPut32(D + UDF_FSD_CSLIST, 1);
    UdfPut32(D + UDF_FSD_MAXCSLIST, 1);
    UdfCharspec(D + UDF_FSD_LVICS);
    while (K->M->Label[n]) n++;
    UdfUtf8ToDstring(K->M->Label, n, D + UDF_FSD_LVID, 128);
    UdfCharspec(D + UDF_FSD_FSCS);
    UdfUtf8ToDstring(K->M->Label, n > 30 ? 30 : n, D + UDF_FSD_FSID, 32);
    root.Part = K->FsPart;
    root.Lbn = K->RootLbn;
    UdfLongAdPut(D + UDF_FSD_ROOT, K->B, &root);
    {
        UCHAR suffix[8];
        UdfMemZero(suffix, 8);
        UdfPut16(suffix, (USHORT)K->M->Revision);
        UdfRegid(D + UDF_FSD_DOMAIN, UDF_ID_COMPLIANT, suffix, 8);
    }
    Tag(K, D, UDF_TAG_FSD, K->FsdLbn, 512);
}

/* a file entry: Ads (short_ad image, or the embedded data) */
static ULONG
FileEntry(MK *K, PUCHAR D, ULONG Loc, UCHAR Type, UCHAR AdType, ULONG Mode, USHORT Links, ULONGLONG Size,
          ULONGLONG UniqueId, const UCHAR *Ads, ULONG AdLen, ULONGLONG Blocks)
{
    BOOLEAN efe = (BOOLEAN)(K->M->Revision >= UDF_REV_200 && !K->M->UseFe);
    ULONG hdr = efe ? UDF_EFE_EA : UDF_FE_EA;
    UDF_INODE tmp;
    UdfMemZero(D, K->B);
    UdfPut16(D + UDF_ICBTAG + UDF_ICBTAG_STRATEGY, 4);
    UdfPut16(D + UDF_ICBTAG + UDF_ICBTAG_MAXENTRIES, 1);
    D[UDF_ICBTAG + UDF_ICBTAG_FILETYPE] = Type;
    UdfMemZero(&tmp, sizeof(tmp));
    tmp.FileType = Type;
    UdfISetMode(&tmp, Mode);
    tmp.Perm |= (UDF_PERM_CHATTR | UDF_PERM_DELETE) << 10;
    UdfPut16(D + UDF_ICBTAG + UDF_ICBTAG_FLAGS, (USHORT)(AdType | (tmp.IcbFlags & ~7)));
    UdfPut32(D + UDF_FE_UID, K->M->Uid);
    UdfPut32(D + UDF_FE_GID, K->M->Gid);
    UdfPut32(D + UDF_FE_PERM, tmp.Perm);
    UdfPut16(D + UDF_FE_LINKS, Links);
    UdfPut64(D + UDF_FE_INFOLEN, Size);
    if (efe) {
        UdfPut64(D + UDF_EFE_OBJSIZE, Size);
        UdfPut64(D + UDF_EFE_BLOCKS, Blocks);
        UdfTimePut(D + UDF_EFE_ATIME, &K->Now);
        UdfTimePut(D + UDF_EFE_MTIME, &K->Now);
        UdfTimePut(D + UDF_EFE_CTIME, &K->Now);
        UdfTimePut(D + UDF_EFE_ATTRTIME, &K->Now);
        UdfPut32(D + UDF_EFE_CHECKPOINT, 1);
        UdfRegidImpl(D + UDF_EFE_IMPL);
        UdfPut64(D + UDF_EFE_UNIQUEID, UniqueId);
        UdfPut32(D + UDF_EFE_LAD, AdLen);
    } else {
        UdfPut64(D + UDF_FE_BLOCKS, Blocks);
        UdfTimePut(D + UDF_FE_ATIME, &K->Now);
        UdfTimePut(D + UDF_FE_MTIME, &K->Now);
        UdfTimePut(D + UDF_FE_ATTRTIME, &K->Now);
        UdfPut32(D + UDF_FE_CHECKPOINT, 1);
        UdfRegidImpl(D + UDF_FE_IMPL);
        UdfPut64(D + UDF_FE_UNIQUEID, UniqueId);
        UdfPut32(D + UDF_FE_LAD, AdLen);
    }
    if (AdLen) UdfMemCopy(D + hdr, Ads, AdLen);
    Tag(K, D, efe ? UDF_TAG_EFE : UDF_TAG_FE, Loc, hdr + AdLen);
    return hdr + AdLen;
}

static VOID
SetUsed(PUCHAR Bitmap, ULONG First, ULONG Count)
{
    while (Count--) { Bitmap[First >> 3] &= (UCHAR)~(1u << (First & 7)); First++; }
}

/* a space bitmap descriptor of Bits blocks, the used ones cleared */
static int
WriteSbd(MK *K, ULONGLONG At, ULONG Loc, PUCHAR Bitmap, ULONG Bits, ULONG Blocks)
{
    ULONG bytes = (Bits + 7) / 8, total = Blocks << K->Shift;
    PUCHAR d = (PUCHAR)UdfHostAlloc(total);
    int ok;
    if (!d) { K->M->Error = "out of memory"; return 0; }
    UdfMemZero(d, total);
    UdfPut32(d + UDF_SBD_BITS, Bits);
    UdfPut32(d + UDF_SBD_BYTES, bytes);
    UdfMemCopy(d + UDF_SBD_MAP, Bitmap, bytes);
    UdfTagMake(d, UDF_TAG_SBD, K->Ver, 1, Loc, UDF_SBD_MAP);
    ok = Put(K, At, d, Blocks);
    UdfHostFree(d);
    return ok;
}

static ULONG
SbdBlocks(MK *K, ULONG Bits)
{
    return (UDF_SBD_MAP + (Bits + 7) / 8 + K->B - 1) >> K->Shift;
}

int
UdfMakeFs(UDF_MKFS *M)
{
    MK K;
    ULONG i;
    UCHAR fid[64];
    ULONG fidLen;
    ULONGLONG t;
    int ok = 0;

    UdfMemZero(&K, sizeof(K));
    K.M = M;
    K.B = M->BlockSize ? M->BlockSize : 512;
    M->BlockSize = K.B;
    if (K.B < 512 || K.B > 65536 || (K.B & (K.B - 1))) { M->Error = "bad block size"; return 0; }
    for (K.Shift = 0; (1u << K.Shift) < K.B; K.Shift++)
        ;
    if (M->Revision < UDF_REV_102 || M->Revision > UDF_REV_260) { M->Error = "bad UDF revision"; return 0; }
    if (M->Metadata && M->Revision < UDF_REV_250) { M->Error = "a metadata partition needs UDF 2.50 or later"; return 0; }
    if (M->Sparable && M->Revision < UDF_REV_150) { M->Error = "a sparable partition needs UDF 1.50 or later"; return 0; }
    if (M->Metadata && M->Sparable) { M->Error = "a metadata partition on a sparable partition is not supported"; return 0; }
    if (M->Bytes >> K.Shift > 0xFFFFFFFFULL) K.N = 0xFFFFFFFF;
    else K.N = (ULONG)(M->Bytes >> K.Shift);
    if (K.N < 2048) { M->Error = "the volume is too small"; return 0; }
    K.Ver = M->Revision >= UDF_REV_200 ? 3 : 2;
    K.V.Revision = (USHORT)M->Revision;
    K.Now.Nt = M->NowNt;
    K.Now.Tz = 0;
    K.Blk = (PUCHAR)UdfHostAlloc(K.B);
    if (!K.Blk) { M->Error = "out of memory"; return 0; }
    t = M->NowNt / 10000000ULL;
    {
        static const char hx[] = "0123456789ABCDEF";
        ULONG a = (ULONG)t, b = M->Serial ? M->Serial : (ULONG)(M->NowNt ^ (M->NowNt >> 29));
        for (i = 0; i < 8; i++) { K.VolSetHex[i] = hx[(a >> (28 - 4 * i)) & 15]; K.VolSetHex[8 + i] = hx[(b >> (28 - 4 * i)) & 15]; }
    }

    /* the partition */
    K.PStart = ANCHOR + 1;
    K.PLen = (K.N - ANCHOR - 1) - K.PStart;            /* up to the second anchor (N-257) */
    if (M->Sparable) {
        /* the sparing tables and the spare packets go first, outside the partition */
        ULONG pk = M->Packet ? M->Packet : 32;
        ULONG sc = M->SpareEntries ? M->SpareEntries : 32;
        ULONG tb = (UDF_SPT_MAP + 8 * sc + K.B - 1) >> K.Shift;
        M->Packet = pk;
        K.SpareCount = sc;
        K.SparTableBlocks = tb;
        K.SparTable[0] = K.PStart;
        K.SparTable[1] = K.PStart + tb;
        K.SpareStart = (K.PStart + 2 * tb + pk - 1) / pk * pk;
        K.PStart = K.SpareStart + sc * pk;
        K.PStart = (K.PStart + pk - 1) / pk * pk;
        K.PLen = ((K.N - ANCHOR - 1) - K.PStart) / pk * pk;
    }
    if (K.PLen < 256) { M->Error = "the volume is too small"; goto out; }
    K.MapCount = M->Metadata ? 2 : 1;
    K.PhysBitmap = (PUCHAR)UdfHostAlloc((K.PLen + 7) / 8 + 8);
    if (!K.PhysBitmap) { M->Error = "out of memory"; goto out; }
    for (i = 0; i < (K.PLen + 7) / 8 + 8; i++) K.PhysBitmap[i] = 0xFF;
    for (i = K.PLen; i < ((K.PLen + 7) & ~7u); i++) K.PhysBitmap[i >> 3] &= (UCHAR)~(1u << (i & 7));

    /* the physical partition: the bitmap first, then what the file set needs */
    K.SbdLbn = 0;
    K.SbdBlocks = SbdBlocks(&K, K.PLen);
    K.PhysUsed = K.SbdBlocks;
    if (M->BootBlocks) K.PhysUsed = K.SbdBlocks + M->BootBlocks;    /* kept free for the boot view */
    if (M->Metadata) {
        ULONG unit = 32, len;
        while (unit << K.Shift < (64u << 10)) unit <<= 1;          /* at least 64 KB */
        len = K.PLen / 128;                                          /* about 1% to begin with */
        if (len < 4 * unit) len = 4 * unit;
        if (len > K.PLen / 8) len = K.PLen / 8 / unit * unit;
        len = len / unit * unit;
        if (len < unit) { M->Error = "the volume is too small for a metadata partition"; goto out; }
        K.AllocUnit = unit;
        K.MetaLen = len;
        K.MetaFeLbn = K.PhysUsed;
        K.BitmapFeLbn = K.PhysUsed + 1;
        K.MetaBitmapBlocks = SbdBlocks(&K, len);
        K.MetaBitmapLbn = K.PhysUsed + 2;
        K.MetaLbn = (K.MetaBitmapLbn + K.MetaBitmapBlocks + unit - 1) / unit * unit;
        K.PhysUsed = K.MetaLbn + len;
        /* the mirror lies at the far end, in a unit of its own */
        K.MirrorLbn = (K.PLen - len - 1) / unit * unit;
        K.MirrorFeLbn = K.MirrorLbn - 1;
        if (K.MirrorFeLbn <= K.PhysUsed) { M->Error = "the volume is too small for a metadata partition"; goto out; }
        SetUsed(K.PhysBitmap, 0, K.MetaBitmapLbn + K.MetaBitmapBlocks);
        SetUsed(K.PhysBitmap, K.MetaLbn, len);
        if (M->BootBlocks) {
            /* the boot area stays free */
            ULONG b;
            for (b = K.SbdBlocks; b < K.SbdBlocks + M->BootBlocks; b++) K.PhysBitmap[b >> 3] |= (UCHAR)(1u << (b & 7));
        }
        SetUsed(K.PhysBitmap, K.MirrorFeLbn, len + 1);
        K.FsPart = 1;
        K.FsdLbn = 0;
        K.RootLbn = 2;
        K.MetaUsed = 3;
        K.MetaBitmap = (PUCHAR)UdfHostAlloc((len + 7) / 8 + 8);
        if (!K.MetaBitmap) { M->Error = "out of memory"; goto out; }
        for (i = 0; i < (len + 7) / 8 + 8; i++) K.MetaBitmap[i] = 0xFF;
        for (i = len; i < ((len + 7) & ~7u); i++) K.MetaBitmap[i >> 3] &= (UCHAR)~(1u << (i & 7));
        SetUsed(K.MetaBitmap, 0, K.MetaUsed);
    } else {
        ULONG base = K.PhysUsed;
        K.FsPart = 0;
        K.FsdLbn = base;
        K.RootLbn = base + 2;
        K.PhysUsed = base + 3;
        SetUsed(K.PhysBitmap, 0, K.PhysUsed);
        if (M->BootBlocks) {
            ULONG b;
            for (b = K.SbdBlocks; b < K.SbdBlocks + M->BootBlocks; b++) K.PhysBitmap[b >> 3] |= (UCHAR)(1u << (b & 7));
        }
    }

    /* the boot area and the volume recognition sequence */
    if (!Zero(&K, 0, (32768 + K.B - 1) >> K.Shift)) goto out;
    if (!WriteVrs(&K)) goto out;
    {
        ULONG vrsEnd = (32768 + 3 * (K.B < 2048 ? 2048 : K.B) + K.B - 1) >> K.Shift;
        if (vrsEnd < MVDS && !Zero(&K, vrsEnd, MVDS - vrsEnd)) goto out;
    }
    /* descriptor sequences, integrity, anchors */
    if (!WriteVds(&K, MVDS)) goto out;
    if (!Zero(&K, MVDS + VDS_LEN, LVIDS - MVDS - VDS_LEN)) goto out;
    if (!WriteLvid(&K)) goto out;
    if (!Zero(&K, LVIDS + VDS_LEN, ANCHOR - LVIDS - VDS_LEN)) goto out;
    if (!WriteAnchor(&K, ANCHOR)) goto out;
    if (!WriteVds(&K, K.N - RVDS_BACK)) goto out;
    if (!WriteAnchor(&K, K.N - ANCHOR - 1)) goto out;
    if (!WriteAnchor(&K, K.N - 1)) goto out;
    if (!Zero(&K, K.N - ANCHOR, ANCHOR - RVDS_BACK)) goto out;
    if (!Zero(&K, K.N - RVDS_BACK + VDS_LEN, RVDS_BACK - VDS_LEN - 1)) goto out;

    /* sparing tables and spare packets */
    if (M->Sparable) {
        ULONG tb = K.SparTableBlocks, total = tb << K.Shift, tt;
        PUCHAR d = (PUCHAR)UdfHostAlloc(total);
        if (!d) { M->Error = "out of memory"; goto out; }
        for (tt = 0; tt < 2; tt++) {
            UdfMemZero(d, total);
            UdfRegidUdf(&K.V, d + UDF_SPT_IDENT, UDF_ID_SPARING_TABLE);
            UdfPut16(d + UDF_SPT_COUNT, (USHORT)K.SpareCount);
            UdfPut32(d + UDF_SPT_SEQ, 0);
            for (i = 0; i < K.SpareCount; i++) {
                UdfPut32(d + UDF_SPT_MAP + 8 * i, 0xFFFFFFFF);       /* available */
                UdfPut32(d + UDF_SPT_MAP + 8 * i + 4, K.SpareStart + i * M->Packet);
            }
            UdfTagMake(d, UDF_TAG_SPARING, K.Ver, 1, K.SparTable[tt], UDF_SPT_MAP + 8 * K.SpareCount);
            if (!Put(&K, K.SparTable[tt], d, tb)) { UdfHostFree(d); goto out; }
        }
        UdfHostFree(d);
    }

    /* the physical partition's bitmap */
    if (!WriteSbd(&K, K.PStart + K.SbdLbn, K.SbdLbn, K.PhysBitmap, K.PLen, K.SbdBlocks)) goto out;

    /* the file set (in the metadata partition, or in the physical one) */
    {
        ULONGLONG fsdAbs, rootAbs;
        if (M->Metadata) {
            fsdAbs = K.PStart + K.MetaLbn + K.FsdLbn;
            rootAbs = K.PStart + K.MetaLbn + K.RootLbn;
        } else {
            fsdAbs = K.PStart + K.FsdLbn;
            rootAbs = K.PStart + K.RootLbn;
        }
        Fsd(&K, K.Blk);
        if (!Put(&K, fsdAbs, K.Blk, 1)) goto out;
        if (M->Metadata && !Put(&K, K.PStart + K.MirrorLbn + K.FsdLbn, K.Blk, 1)) goto out;
        UdfMemZero(K.Blk, K.B);
        Tag(&K, K.Blk, UDF_TAG_TD, K.FsdLbn + 1, 512);
        if (!Put(&K, fsdAbs + 1, K.Blk, 1)) goto out;
        if (M->Metadata && !Put(&K, K.PStart + K.MirrorLbn + K.FsdLbn + 1, K.Blk, 1)) goto out;
        /* the root: its parent entry only (pointing to itself) */
        {
            UDF_LOC self;
            self.Part = K.FsPart;
            self.Lbn = K.RootLbn;
            UdfMemZero(fid, sizeof(fid));
            UdfPut16(fid + UDF_FID_VERSION, 1);
            fid[UDF_FID_CHAR] = UDF_FID_DIRECTORY | UDF_FID_PARENT;
            UdfLongAdPut(fid + UDF_FID_ICB, K.B, &self);
            fidLen = 40;
            UdfTagMake(fid, UDF_TAG_FID, K.Ver, 1, K.RootLbn, fidLen);
        }
        FileEntry(&K, K.Blk, K.RootLbn, UDF_FT_DIRECTORY, UDF_ICB_AD_IN_ICB, M->Mode ? M->Mode : 0755, 1, fidLen, 0, fid, fidLen, 0);
        if (!Put(&K, rootAbs, K.Blk, 1)) goto out;
        if (M->Metadata && !Put(&K, K.PStart + K.MirrorLbn + K.RootLbn, K.Blk, 1)) goto out;
    }

    /* the metadata partition's files */
    if (M->Metadata) {
        UCHAR ad[UDF_SHORT_AD];
        UdfPut32(ad, K.MetaLen << K.Shift);
        UdfPut32(ad + 4, K.MetaLbn);
        FileEntry(&K, K.Blk, K.MetaFeLbn, UDF_FT_METADATA, UDF_ICB_AD_SHORT, 0, 1, (ULONGLONG)K.MetaLen << K.Shift, 0, ad, 8, K.MetaLen);
        if (!Put(&K, K.PStart + K.MetaFeLbn, K.Blk, 1)) goto out;
        UdfPut32(ad + 4, K.MirrorLbn);
        FileEntry(&K, K.Blk, K.MirrorFeLbn, UDF_FT_METAMIRROR, UDF_ICB_AD_SHORT, 0, 1, (ULONGLONG)K.MetaLen << K.Shift, 0, ad, 8, K.MetaLen);
        if (!Put(&K, K.PStart + K.MirrorFeLbn, K.Blk, 1)) goto out;
        /* the bitmap file: a space bitmap descriptor of the metadata partition */
        UdfPut32(ad, (UDF_SBD_MAP + (K.MetaLen + 7) / 8));
        UdfPut32(ad + 4, K.MetaBitmapLbn);
        FileEntry(&K, K.Blk, K.BitmapFeLbn, UDF_FT_METABITMAP, UDF_ICB_AD_SHORT, 0, 1,
                  UDF_SBD_MAP + (K.MetaLen + 7) / 8, 0, ad, 8, K.MetaBitmapBlocks);
        if (!Put(&K, K.PStart + K.BitmapFeLbn, K.Blk, 1)) goto out;
        if (!WriteSbd(&K, K.PStart + K.MetaBitmapLbn, K.MetaBitmapLbn, K.MetaBitmap, K.MetaLen, K.MetaBitmapBlocks)) goto out;
    }
    M->Blocks = K.N;
    M->PartStart = K.PStart;
    M->PartLength = K.PLen;
    M->MetaLength = K.MetaLen;
    ok = 1;
out:
    if (K.Blk) UdfHostFree(K.Blk);
    if (K.PhysBitmap) UdfHostFree(K.PhysBitmap);
    if (K.MetaBitmap) UdfHostFree(K.MetaBitmap);
    return ok;
}
