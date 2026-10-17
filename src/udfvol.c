/*
 * UDFNT - the volume: recognition, the volume descriptor sequence, the
 * partitions (physical, sparable, virtual, metadata), block translation,
 * free space, the integrity descriptor, commits and the label.
 */
#include "udfeng.h"
#include "udfshim.h"

VOID UdfBufDropAll(PUDF_VOL Vol);

static const ULONG BlockSizes[] = { 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536 };

/* ---- recognition ------------------------------------------------------------ */
static BOOLEAN
ReadBytes(PUDF_VOL V, ULONGLONG Off, ULONG Len, PVOID Buf)
{
    if (V->DeviceBytes && Off + Len > V->DeviceBytes) return FALSE;
    return (BOOLEAN)UDF_SUCCESS(UdfHostRead(V, Off, Len, Buf));
}

/* the volume recognition sequence: BEA01, NSR02 or NSR03 (any order with others), TEA01 */
static BOOLEAN
HasVrs(PUDF_VOL V, ULONG Step, PUCHAR Tmp)
{
    ULONG i;
    BOOLEAN bea = FALSE, nsr = FALSE;
    for (i = 0; i < 64; i++) {
        if (!ReadBytes(V, UDF_VRS_OFFSET + (ULONGLONG)i * Step, 7, Tmp)) return FALSE;
        if (Tmp[1] == 'B' && !UdfMemCmp(Tmp + 1, "BEA01", 5)) bea = TRUE;
        else if (!UdfMemCmp(Tmp + 1, "NSR02", 5) || !UdfMemCmp(Tmp + 1, "NSR03", 5)) { if (bea) nsr = TRUE; }
        else if (!UdfMemCmp(Tmp + 1, "TEA01", 5)) return nsr;
        else if (!UdfMemCmp(Tmp + 1, "CD001", 5) || !UdfMemCmp(Tmp + 1, "CDW02", 5) ||
                 !UdfMemCmp(Tmp + 1, "BOOT2", 5)) continue;
        else if (Tmp[1] == 0 && i > 0 && bea) return nsr;     /* end of the sequence */
        else if (i > 0 || Tmp[1] != 0) { if (!bea) return FALSE; }
    }
    return nsr;
}

/* an anchor at block Lbn with block size B */
static BOOLEAN
AnchorAt(PUDF_VOL V, ULONG B, ULONGLONG Lbn, PUCHAR Tmp)
{
    if (!ReadBytes(V, Lbn * B, 512, Tmp)) return FALSE;
    return UdfTagCheck(Tmp, 512, UDF_TAG_AVDP, (ULONG)Lbn);
}

static BOOLEAN
FindAnchor(PUDF_VOL V, ULONG B, PUCHAR Avdp, PULONG Last)
{
    ULONGLONG n = V->DeviceBytes / B;
    if (AnchorAt(V, B, UDF_ANCHOR_LBN, Avdp)) { *Last = n ? (ULONG)(n - 1) : 0; return TRUE; }
    if (n > 1 && AnchorAt(V, B, n - 1, Avdp)) { *Last = (ULONG)(n - 1); return TRUE; }
    if (n > 257 && AnchorAt(V, B, n - 257, Avdp)) { *Last = (ULONG)(n - 1); return TRUE; }
    if (n > 512 && AnchorAt(V, B, 512, Avdp)) { *Last = (ULONG)(n - 1); return TRUE; }
    return FALSE;
}

BOOLEAN
UdfProbe(PUDF_VOL Vol)
{
    UCHAR Tmp[512];
    ULONG i, step, last;
    for (i = 0; i < sizeof(BlockSizes) / sizeof(BlockSizes[0]); i++) {
        ULONG B = BlockSizes[i];
        if (Vol->SectorSize && B < Vol->SectorSize) continue;
        step = B < UDF_VSD_SIZE ? UDF_VSD_SIZE : B;
        if (!HasVrs(Vol, step, Tmp)) continue;
        if (FindAnchor(Vol, B, Tmp, &last)) return TRUE;
    }
    return FALSE;
}

/* ---- block translation ----------------------------------------------------- */
static ULONG
SpareRemap(PUDF_PMAP M, ULONG Lbn)
{
    ULONG i, base;
    if (!M->SpareCount || !M->Packet) return Lbn;
    base = Lbn - (Lbn % M->Packet);
    for (i = 0; i < M->SpareCount; i++) {
        if (M->SpareOrig[i] == base) return M->SpareMap[i] - M->Start + (Lbn - base);
        if (M->SpareOrig[i] >= 0xFFFFFFF0) break;
    }
    return Lbn;
}

static UDF_STATUS MetaMap(PUDF_VOL V, PUDF_PMAP M, PUDF_INODE F, ULONG Lbn, PULONGLONG Abs, PULONG Run);

UDF_STATUS
UdfTranslate(PUDF_VOL Vol, USHORT Part, ULONG Lbn, PULONGLONG Abs, PULONG Run, PULONGLONG Mirror)
{
    PUDF_PMAP M;
    ULONG l;
    if (Mirror) *Mirror = UDF_NO_MIRROR;
    if (Part >= Vol->NMaps) return UDF_CORRUPT;
    M = &Vol->Map[Part];
    switch (M->Kind) {
    case UDF_KIND_PHYS:
        if (Lbn >= M->Length) return UDF_CORRUPT;
        *Abs = (ULONGLONG)M->Start + Lbn;
        *Run = M->Length - Lbn;
        return UDF_OK;
    case UDF_KIND_SPAR:
        if (Lbn >= M->Length) return UDF_CORRUPT;
        l = SpareRemap(M, Lbn);
        *Abs = (ULONGLONG)M->Start + l;
        *Run = M->Packet ? M->Packet - (Lbn % M->Packet) : M->Length - Lbn;
        return UDF_OK;
    case UDF_KIND_VIRT:
        if (Lbn >= M->VatCount || M->Vat[Lbn] == 0xFFFFFFFF) return UDF_CORRUPT;
        *Run = 1;
        return UdfTranslate(Vol, M->Under, M->Vat[Lbn], Abs, &l, NULL);
    case UDF_KIND_META: {
        UDF_STATUS st;
        if (!M->MetaFile) return UDF_CORRUPT;
        st = MetaMap(Vol, M, M->MetaFile, Lbn, Abs, Run);
        if (!UDF_SUCCESS(st) && M->MetaMirror) st = MetaMap(Vol, M, M->MetaMirror, Lbn, Abs, Run);
        if (UDF_SUCCESS(st) && Mirror && M->MetaMirror && M->MetaFile) {
            ULONGLONG a2;
            ULONG r2;
            if (UDF_SUCCESS(MetaMap(Vol, M, M->MetaMirror, Lbn, &a2, &r2)) && a2 != *Abs) *Mirror = a2;
        }
        return st;
    }
    }
    return UDF_CORRUPT;
}

static UDF_STATUS
MetaMap(PUDF_VOL V, PUDF_PMAP M, PUDF_INODE F, ULONG Lbn, PULONGLONG Abs, PULONG Run)
{
    ULONGLONG a;
    ULONG r;
    UCHAR kind;
    UDF_STATUS st = UdfImap(V, F, Lbn, &a, &r, &kind);
    (VOID)M;
    if (!UDF_SUCCESS(st)) return st;
    if (kind != UDF_MAP_RECORDED && kind != UDF_MAP_UNRECORDED) return UDF_CORRUPT;
    *Abs = a;
    *Run = r;
    return UDF_OK;
}

/* ---- the volume descriptor sequence ---------------------------------------- */
typedef struct {
    ULONG   Num;            /* VDS number seen (the highest wins) */
    ULONG   Block;
    ULONG   Len;
    UCHAR   D[4096];
    BOOLEAN Have;
} VD;

typedef struct {
    VD      Pvd, Lvd, Iuvd;
    VD      Pd[4];
    ULONG   NPd;
} VDS;

static VOID
Keep(VD *Slot, const UCHAR *D, ULONG Len, ULONG Block)
{
    ULONG num = UdfGet32(D + 16);
    if (Slot->Have && num < Slot->Num) return;
    if (Len > sizeof(Slot->D)) Len = sizeof(Slot->D);
    Slot->Num = num;
    Slot->Block = Block;
    Slot->Len = Len;
    UdfMemZero(Slot->D, sizeof(Slot->D));
    UdfMemCopy(Slot->D, D, Len);
    Slot->Have = TRUE;
}

static UDF_STATUS
ReadVds(PUDF_VOL V, ULONG Loc, ULONG Len, VDS *S, PUCHAR Buf, ULONG BufLen)
{
    ULONG i, n = Len >> V->BlockShift, hops = 0;
    for (i = 0; i < n && i < 1024; i++) {
        ULONG blk = Loc + i, have = V->BlockSize, need;
        USHORT id;
        if (!UDF_SUCCESS(UdfReadAbs(V, blk, 1, Buf))) return UDF_IO_ERROR;
        id = UdfGet16(Buf);
        need = UDF_TAG_SIZE + UdfGet16(Buf + 10);
        if (need > have && need <= BufLen) {
            /* a descriptor longer than a block (a big logical volume descriptor) */
            ULONG more = (need - have + V->BlockSize - 1) >> V->BlockShift;
            if (!UDF_SUCCESS(UdfReadAbs(V, blk + 1, more, Buf + have))) return UDF_IO_ERROR;
            have += more << V->BlockShift;
        }
        if (!UdfTagCheck(Buf, have, id, blk)) {
            if (id == 0 && i > 0) break;
            continue;
        }
        switch (id) {
        case UDF_TAG_PVD: Keep(&S->Pvd, Buf, 512, blk); break;
        case UDF_TAG_LVD: Keep(&S->Lvd, Buf, need > 512 ? need : 512, blk); break;
        case UDF_TAG_IUVD: if (UdfRegidIs(Buf + 20, UDF_ID_LV_INFO)) Keep(&S->Iuvd, Buf, 512, blk); break;
        case UDF_TAG_PD: {
            USHORT num = UdfGet16(Buf + UDF_PD_NUMBER);
            ULONG j;
            for (j = 0; j < S->NPd; j++) if (UdfGet16(S->Pd[j].D + UDF_PD_NUMBER) == num) break;
            if (j == S->NPd) { if (S->NPd == 4) break; S->NPd++; S->Pd[j].Have = FALSE; }
            Keep(&S->Pd[j], Buf, 512, blk);
            break;
        }
        case UDF_TAG_VDP:
            if (++hops > 16) return UDF_CORRUPT;
            Loc = UdfGet32(Buf + 24);
            n = UdfGet32(Buf + 20) >> V->BlockShift;
            i = (ULONG)-1;
            break;
        case UDF_TAG_TD:
            return UDF_OK;
        }
        if (have > V->BlockSize) i += (have >> V->BlockShift) - 1;
    }
    return UDF_OK;
}

/* ---- partitions ---------------------------------------------------------------- */
static UDF_STATUS
LoadSparing(PUDF_VOL V, PUDF_PMAP M, const UCHAR *Pm)
{
    ULONG nt = Pm[UDF_PM_SPAR_NTABLES], size = UdfGet32(Pm + UDF_PM_SPAR_TABLESIZE), t, blocks;
    PUCHAR buf;
    M->Packet = UdfGet16(Pm + UDF_PM_SPAR_PACKET);
    if (nt > 4) nt = 4;
    if (size < UDF_SPT_MAP || size > (1u << 20)) return UDF_OK;
    blocks = (size + V->BlockSize - 1) >> V->BlockShift;
    buf = (PUCHAR)UdfHostAlloc(blocks << V->BlockShift);
    if (!buf) return UDF_NO_MEMORY;
    for (t = 0; t < nt; t++) {
        ULONG loc = UdfGet32(Pm + UDF_PM_SPAR_TABLES + 4 * t), cnt, i;
        if (!UDF_SUCCESS(UdfReadAbs(V, loc, blocks, buf))) continue;
        if (!UdfTagCheck(buf, blocks << V->BlockShift, UDF_TAG_SPARING, loc) || !UdfRegidIs(buf + UDF_SPT_IDENT, UDF_ID_SPARING_TABLE))
            continue;
        cnt = UdfGet16(buf + UDF_SPT_COUNT);
        if (UDF_SPT_MAP + cnt * 8 > (blocks << V->BlockShift)) continue;
        M->SpareOrig = (PULONG)UdfHostAlloc((cnt + 1) * 4);
        M->SpareMap = (PULONG)UdfHostAlloc((cnt + 1) * 4);
        if (!M->SpareOrig || !M->SpareMap) { UdfHostFree(buf); return UDF_NO_MEMORY; }
        for (i = 0; i < cnt; i++) {
            M->SpareOrig[i] = UdfGet32(buf + UDF_SPT_MAP + 8 * i);
            M->SpareMap[i] = UdfGet32(buf + UDF_SPT_MAP + 8 * i + 4);
        }
        M->SpareCount = cnt;
        break;
    }
    UdfHostFree(buf);
    return UDF_OK;
}

/* the VAT: the file entry in the last recorded block of the physical partition */
static UDF_STATUS
LoadVat(PUDF_VOL V, PUDF_PMAP M)
{
    PUDF_PMAP P = &V->Map[M->Under];
    ULONG tries, blk = V->LastBlock;
    PUCHAR b = V->Scratch;
    /* an image of a disc can end in unrecorded (zero) blocks: start from the last recorded one */
    {
        ULONG chunk = 64, scanned = 0, i;
        PUCHAR c = (PUCHAR)UdfHostAlloc(chunk << V->BlockShift);
        if (c) {
            ULONG at = V->LastBlock + 1;
            BOOLEAN found = FALSE;
            while (at > P->Start && scanned < (1u << 20) && !found) {
                ULONG n = at - P->Start < chunk ? at - P->Start : chunk;
                at -= n;
                scanned += n;
                if (!UDF_SUCCESS(UdfReadAbs(V, at, n, c))) break;
                for (i = n; i-- > 0; ) {
                    ULONG j;
                    PUCHAR q = c + (i << V->BlockShift);
                    for (j = 0; j < V->BlockSize && !q[j]; j++)
                        ;
                    if (j < V->BlockSize) { blk = at + i; found = TRUE; break; }
                }
            }
            UdfHostFree(c);
        }
    }
    for (tries = 0; tries < 64 && blk > P->Start; tries++, blk--) {
        UDF_LOC loc;
        PUDF_INODE F;
        UDF_STATUS st;
        ULONG got, hdr = 0, n, i;
        PUCHAR data;
        BOOLEAN v150 = FALSE;
        if (!UDF_SUCCESS(UdfReadAbs(V, blk, 1, b))) continue;
        if (!UdfTagCheck(b, V->BlockSize, UDF_TAG_FE, blk - P->Start) &&
            !UdfTagCheck(b, V->BlockSize, UDF_TAG_EFE, blk - P->Start)) continue;
        if (b[UDF_ICBTAG + UDF_ICBTAG_FILETYPE] == UDF_FT_VAT20) v150 = FALSE;
        else if (b[UDF_ICBTAG + UDF_ICBTAG_FILETYPE] == UDF_FT_UNSPECIFIED) v150 = TRUE;
        else continue;
        loc.Part = M->Under;
        loc.Lbn = blk - P->Start;
        st = UdfIget(V, &loc, &F);
        if (!UDF_SUCCESS(st)) continue;
        if (F->Size < 36 || F->Size > (64u << 20)) { UdfIput(V, F); continue; }
        data = (PUCHAR)UdfHostAlloc((ULONG)F->Size);
        if (!data) { UdfIput(V, F); return UDF_NO_MEMORY; }
        st = UdfIread(V, F, 0, (ULONG)F->Size, data, &got);
        if (!UDF_SUCCESS(st) || got != F->Size) { UdfHostFree(data); UdfIput(V, F); continue; }
        if (v150) {
            if (!UdfRegidIs(data + got - 36, UDF_ID_VAT150)) { UdfHostFree(data); UdfIput(V, F); continue; }
            n = (got - 36) / 4;
        } else {
            hdr = UdfGet16(data + UDF_VAT_HDRLEN);
            if (hdr < UDF_VAT_HDRSIZE || hdr > got) { UdfHostFree(data); UdfIput(V, F); continue; }
            n = (got - hdr) / 4;
            V->NumFiles = UdfGet32(data + UDF_VAT_FILES);
            V->NumDirs = UdfGet32(data + UDF_VAT_DIRS);
        }
        M->Vat = (PULONG)UdfHostAlloc((n ? n : 1) * 4);
        if (!M->Vat) { UdfHostFree(data); UdfIput(V, F); return UDF_NO_MEMORY; }
        for (i = 0; i < n; i++) M->Vat[i] = UdfGet32(data + hdr + 4 * i);
        M->VatCount = n;
        M->Length = n;
        UdfHostFree(data);
        UdfIput(V, F);
        return UDF_OK;
    }
    return UDF_CORRUPT;
}

static UDF_STATUS
LoadMeta(PUDF_VOL V, PUDF_PMAP M, ULONG Index)
{
    UDF_LOC l;
    UDF_STATUS st;
    l.Part = M->Under;
    l.Lbn = M->MetaFileLbn;
    st = UdfIget(V, &l, &M->MetaFile);
    if (!UDF_SUCCESS(st)) M->MetaFile = NULL;
    if (M->MetaMirrorLbn != 0xFFFFFFFF) {
        l.Lbn = M->MetaMirrorLbn;
        if (!UDF_SUCCESS(UdfIget(V, &l, &M->MetaMirror))) M->MetaMirror = NULL;
    }
    if (!M->MetaFile && !M->MetaMirror) return UDF_CORRUPT;
    if (!M->MetaFile) {
        M->MetaFile = M->MetaMirror;
        M->MetaMirror = NULL;
        V->ReadOnlyForced = TRUE;
        V->ReadOnlyWhy = "the metadata file is damaged (its mirror is used)";
    }
    M->Length = (ULONG)(M->MetaFile->Size >> V->BlockShift);
    if (M->MetaBitmapLbn != 0xFFFFFFFF) {
        l.Lbn = M->MetaBitmapLbn;
        if (UDF_SUCCESS(UdfIget(V, &l, &M->MetaBitmap))) {
            UCHAR hdr[UDF_SBD_MAP];
            ULONG got;
            if (UDF_SUCCESS(UdfIread(V, M->MetaBitmap, 0, UDF_SBD_MAP, hdr, &got)) && got == UDF_SBD_MAP &&
                UdfGet16(hdr) == UDF_TAG_SBD) {
                M->BitmapBits = UdfGet32(hdr + UDF_SBD_BITS);
                /* the bits must lie in the file (Windows embeds a small bitmap in its file entry) */
                M->HasBitmap = (BOOLEAN)(UDF_SBD_MAP + ((ULONGLONG)M->BitmapBits + 7) / 8 <= M->MetaBitmap->Size);
            }
        }
    }
    (VOID)Index;
    return UDF_OK;
}

/* ---- free space ------------------------------------------------------------------ */
/* the byte of the bitmap holding bit Lbn: Buf held, Ptr into it */
static UDF_STATUS
BitmapByte(PUDF_VOL V, PUDF_PMAP M, ULONG Lbn, PUDF_BUF *Buf, PUCHAR *Ptr)
{
    ULONGLONG byteOff = UDF_SBD_MAP + (ULONGLONG)(Lbn >> 3);
    ULONG blk = (ULONG)(byteOff >> V->BlockShift), within = (ULONG)(byteOff & (V->BlockSize - 1));
    UDF_STATUS st;
    if (M->Kind == UDF_KIND_META && M->MetaBitmap->AdType == UDF_ICB_AD_IN_ICB) {
        /* embedded in the bitmap file's entry: no block of its own */
        if (byteOff >= M->MetaBitmap->Size) return UDF_CORRUPT;
        *Buf = NULL;
        *Ptr = M->MetaBitmap->Inline + (ULONG)byteOff;
        return UDF_OK;
    }
    if (M->Kind == UDF_KIND_META) {
        ULONGLONG a;
        ULONG r;
        UCHAR kind;
        st = UdfImap(V, M->MetaBitmap, blk, &a, &r, &kind);
        if (!UDF_SUCCESS(st)) return st;
        if (kind != UDF_MAP_RECORDED) return UDF_CORRUPT;
        st = UdfBufGetAbs(V, a, TRUE, Buf);
    } else {
        st = UdfBufGet(V, (USHORT)(M - V->Map), M->BitmapLbn + blk, TRUE, Buf);
    }
    if (UDF_SUCCESS(st)) *Ptr = (*Buf)->Data + within;
    return st;
}

/* the metadata bitmap's descriptor header: Buf held (NULL when embedded), Ptr to it */
static UDF_STATUS
BitmapHeader(PUDF_VOL V, PUDF_PMAP M, PUDF_BUF *Buf, PUCHAR *Ptr)
{
    ULONGLONG a;
    ULONG r;
    UCHAR kind;
    UDF_STATUS st;
    if (M->MetaBitmap->AdType == UDF_ICB_AD_IN_ICB) {
        if (M->MetaBitmap->Size < UDF_SBD_MAP) return UDF_CORRUPT;
        *Buf = NULL;
        *Ptr = M->MetaBitmap->Inline;
        return UDF_OK;
    }
    st = UdfImap(V, M->MetaBitmap, 0, &a, &r, &kind);
    if (!UDF_SUCCESS(st)) return st;
    if (kind != UDF_MAP_RECORDED) return UDF_CORRUPT;
    st = UdfBufGetAbs(V, a, TRUE, Buf);
    if (UDF_SUCCESS(st)) *Ptr = (*Buf)->Data;
    return st;
}

static UDF_STATUS
CountFree(PUDF_VOL V, PUDF_PMAP M)
{
    ULONG lbn = 0, n = 0;
    UDF_STATUS st;
    while (lbn < M->BitmapBits) {
        PUDF_BUF b;
        PUCHAR p;
        ULONGLONG byteOff = UDF_SBD_MAP + (ULONGLONG)(lbn >> 3);
        ULONG left = V->BlockSize - (ULONG)(byteOff & (V->BlockSize - 1)), i;
        st = BitmapByte(V, M, lbn, &b, &p);
        if (!UDF_SUCCESS(st)) return st;
        for (i = 0; i < left && lbn < M->BitmapBits; i++) {
            UCHAR c = p[i];
            if ((lbn & 7) == 0 && lbn + 8 <= M->BitmapBits) {
                while (c) { n += c & 1; c >>= 1; }
                lbn += 8;
            } else {
                ULONG bit;
                for (bit = lbn & 7; bit < 8 && lbn < M->BitmapBits; bit++, lbn++) n += (c >> bit) & 1;
            }
        }
        UdfBufPut(V, b);
    }
    M->Free = n;
    M->FreeKnown = TRUE;
    return UDF_OK;
}

/* the length (up to Max) of the run of blocks from Lbn that are all free (Free) or all used */
static UDF_STATUS
RunOf(PUDF_VOL V, PUDF_PMAP M, ULONG Lbn, ULONG Max, BOOLEAN Free, PULONG Len)
{
    ULONG n = 0;
    UDF_STATUS st;
    while (n < Max) {
        PUDF_BUF b;
        PUCHAR p;
        ULONG lbn = Lbn + n;
        ULONGLONG byteOff = UDF_SBD_MAP + (ULONGLONG)(lbn >> 3);
        ULONG bits = (V->BlockSize - (ULONG)(byteOff & (V->BlockSize - 1))) * 8 - (lbn & 7);
        BOOLEAN stop = FALSE;
        st = BitmapByte(V, M, lbn, &b, &p);
        if (!UDF_SUCCESS(st)) return st;
        while (bits && n < Max) {
            if ((lbn & 7) == 0 && bits >= 8 && Max - n >= 8 && *p == (Free ? 0xFF : 0x00)) {
                n += 8; lbn += 8; bits -= 8; p++;
                continue;
            }
            if ((BOOLEAN)((*p >> (lbn & 7)) & 1) != Free) { stop = TRUE; break; }
            n++; lbn++; bits--;
            if ((lbn & 7) == 0) p++;
        }
        UdfBufPut(V, b);
        if (stop) break;
    }
    *Len = n;
    return UDF_OK;
}

static UDF_STATUS
SetBits(PUDF_VOL V, PUDF_PMAP M, ULONG Lbn, ULONG Count, BOOLEAN Free)
{
    UDF_STATUS st;
    while (Count) {
        PUDF_BUF b;
        PUCHAR p;
        ULONGLONG byteOff = UDF_SBD_MAP + (ULONGLONG)(Lbn >> 3);
        ULONG bits = (V->BlockSize - (ULONG)(byteOff & (V->BlockSize - 1))) * 8 - (Lbn & 7);
        st = BitmapByte(V, M, Lbn, &b, &p);
        if (!UDF_SUCCESS(st)) return st;
        while (bits && Count) {
            if ((Lbn & 7) == 0 && bits >= 8 && Count >= 8) {
                *p++ = Free ? 0xFF : 0x00;
                Lbn += 8; Count -= 8; bits -= 8;
                continue;
            }
            if (Free) *p = (UCHAR)(*p | (1u << (Lbn & 7)));
            else *p = (UCHAR)(*p & ~(1u << (Lbn & 7)));
            Lbn++; Count--; bits--;
            if ((Lbn & 7) == 0) p++;
        }
        if (b) UdfBufDirty(V, b); else UdfIdirty(V, M->MetaBitmap);
        UdfBufPut(V, b);
    }
    return UDF_OK;
}

/* the metadata partition is full: give it another allocation unit */
static UDF_STATUS GrowMeta(PUDF_VOL V, PUDF_PMAP M);

/* a block of the metadata partition that holds one of the partition's own file entries
   (Windows 11 records the mirror's entry inside the metadata file, its bit left free) */
static BOOLEAN
MetaReserved(PUDF_VOL V, PUDF_PMAP M, ULONG Lbn)
{
    ULONGLONG a, base = V->Map[M->Under].Start;
    ULONG r;
    UCHAR kind;
    if (!UDF_SUCCESS(UdfImap(V, M->MetaFile, Lbn, &a, &r, &kind)) || kind == UDF_MAP_HOLE) return FALSE;
    return (BOOLEAN)(a == base + M->MetaFileLbn ||
                     (M->MetaMirrorLbn != 0xFFFFFFFF && a == base + M->MetaMirrorLbn) ||
                     (M->MetaBitmapLbn != 0xFFFFFFFF && a == base + M->MetaBitmapLbn));
}

UDF_STATUS
UdfAlloc(PUDF_VOL Vol, USHORT Part, ULONG Goal, ULONG Want, PULONG Lbn, PULONG Got)
{
    PUDF_PMAP M;
    ULONG pass, lbn, from, to, len, best = 0, bestAt = 0;
    BOOLEAN recounted = FALSE;
    UDF_STATUS st;
    if (Vol->ReadOnly) return UDF_READ_ONLY;
    if (Part >= Vol->NMaps) return UDF_CORRUPT;
    M = &Vol->Map[Part];
    if (!M->HasBitmap) return UDF_READ_ONLY;
    if (Want == 0) Want = 1;
    if (!M->FreeKnown) { st = CountFree(Vol, M); if (!UDF_SUCCESS(st)) return st; }
again:
    if (M->Free == 0 && M->Kind == UDF_KIND_META) {
        st = GrowMeta(Vol, M);
        if (!UDF_SUCCESS(st)) return st;
    }
    if (M->Free == 0) return UDF_DISK_FULL;
    if (Want > M->Free) Want = M->Free;
    if (Goal >= M->BitmapBits) Goal = M->Hint < M->BitmapBits ? M->Hint : 0;
    /* a range kept for the boot view (Vol->AvoidHi > AvoidLo) is taken only when nothing else is free */
    if (Part == Vol->AvoidPart && Vol->AvoidHi > Vol->AvoidLo && Goal >= Vol->AvoidLo && Goal < Vol->AvoidHi)
        Goal = Vol->AvoidHi < M->BitmapBits ? Vol->AvoidHi : 0;
    for (pass = 0; pass < 3; pass++) {
        BOOLEAN avoid = (BOOLEAN)(pass < 2 && Part == Vol->AvoidPart && Vol->AvoidHi > Vol->AvoidLo);
        if (pass == 2 && (best || !(Part == Vol->AvoidPart && Vol->AvoidHi > Vol->AvoidLo))) break;
        from = pass == 0 ? Goal : pass == 1 ? 0 : Vol->AvoidLo;
        to = pass == 0 ? M->BitmapBits : pass == 1 ? Goal : Vol->AvoidHi;
        if (to > M->BitmapBits) to = M->BitmapBits;
        for (lbn = from; lbn < to; ) {
            ULONG lim = to;
            if (avoid && lbn >= Vol->AvoidLo && lbn < Vol->AvoidHi) { lbn = Vol->AvoidHi; continue; }
            if (avoid && lbn < Vol->AvoidLo && lim > Vol->AvoidLo) lim = Vol->AvoidLo;
            st = RunOf(Vol, M, lbn, lim - lbn, FALSE, &len);       /* used blocks */
            if (!UDF_SUCCESS(st)) return st;
            lbn += len;
            if (lbn >= lim) continue;
            st = RunOf(Vol, M, lbn, (lim - lbn < Want) ? lim - lbn : Want, TRUE, &len);
            if (!UDF_SUCCESS(st)) return st;
            if (len >= Want) { best = len; bestAt = lbn; goto found; }
            if (len > best) { best = len; bestAt = lbn; }
            if (len == 0) lbn++;
            lbn += len;
        }
    }
    if (!best) {
        /* the count came from the integrity descriptor and was wrong (an interrupted
           format leaves one): count the bitmap itself, then grow or give up */
        if (recounted || M->Free == 0) return UDF_DISK_FULL;
        st = CountFree(Vol, M);
        if (!UDF_SUCCESS(st)) return st;
        recounted = TRUE;
        goto again;
    }
found:
    if (best > Want) best = Want;
    if (M->Kind == UDF_KIND_META) {
        ULONG k;
        for (k = 0; k < best && !MetaReserved(Vol, M, bestAt + k); k++)
            ;
        if (k == 0) {
            /* it is in use: say so in the bitmap, and look again */
            st = SetBits(Vol, M, bestAt, 1, FALSE);
            if (!UDF_SUCCESS(st)) return st;
            M->Free--;
            best = 0;
            goto again;
        }
        best = k;
    }
    st = SetBits(Vol, M, bestAt, best, FALSE);
    if (!UDF_SUCCESS(st)) return st;
    M->Free -= best;
    M->Hint = bestAt + best;
    *Lbn = bestAt;
    *Got = best;
    Vol->Changed = TRUE;
    return UDF_OK;
}

/*
 * Count free blocks in [Lo, Hi) of partition Part, starting at a block B with
 * (B - AlignBase) % Align == 0; marks them used. For the boot view.
 */
UDF_STATUS
UdfAllocAligned(PUDF_VOL Vol, USHORT Part, ULONG Lo, ULONG Hi, ULONG Count, ULONG Align, ULONG AlignBase, PULONG Lbn)
{
    PUDF_PMAP M;
    ULONG p, len;
    UDF_STATUS st;
    if (Vol->ReadOnly) return UDF_READ_ONLY;
    if (Part >= Vol->NMaps) return UDF_CORRUPT;
    M = &Vol->Map[Part];
    if (!M->HasBitmap) return UDF_READ_ONLY;
    if (!M->FreeKnown) { st = CountFree(Vol, M); if (!UDF_SUCCESS(st)) return st; }
    if (Align == 0) Align = 1;
    if (Hi > M->BitmapBits) Hi = M->BitmapBits;
    p = Lo;
    if (p < AlignBase) p = AlignBase;
    p = AlignBase + (p - AlignBase + Align - 1) / Align * Align;
    while (p + Count <= Hi) {
        st = RunOf(Vol, M, p, Count, TRUE, &len);
        if (!UDF_SUCCESS(st)) return st;
        if (len >= Count) {
            st = SetBits(Vol, M, p, Count, FALSE);
            if (!UDF_SUCCESS(st)) return st;
            M->Free -= Count;
            *Lbn = p;
            Vol->Changed = TRUE;
            return UDF_OK;
        }
        /* past the used block that ended the run, to the next aligned start */
        p += len + 1;
        p = AlignBase + (p - AlignBase + Align - 1) / Align * Align;
    }
    return UDF_DISK_FULL;
}

/* the length (up to Max) of the run from Lbn of free (Free) or used blocks of partition Part */
UDF_STATUS
UdfRunOf(PUDF_VOL Vol, USHORT Part, ULONG Lbn, ULONG Max, BOOLEAN Free, PULONG Len)
{
    PUDF_PMAP M;
    *Len = 0;
    if (Part >= Vol->NMaps) return UDF_CORRUPT;
    M = &Vol->Map[Part];
    if (!M->HasBitmap) return UDF_NOT_SUPPORTED;
    if (Lbn >= M->BitmapBits) return UDF_OK;
    if (Max > M->BitmapBits - Lbn) Max = M->BitmapBits - Lbn;
    return RunOf(Vol, M, Lbn, Max, Free, Len);
}

UDF_STATUS
UdfFree(PUDF_VOL Vol, USHORT Part, ULONG Lbn, ULONG Count)
{
    PUDF_PMAP M;
    UDF_STATUS st;
    if (!Count) return UDF_OK;
    if (Vol->ReadOnly) return UDF_READ_ONLY;
    if (Part >= Vol->NMaps) return UDF_CORRUPT;
    M = &Vol->Map[Part];
    if (!M->HasBitmap) return UDF_READ_ONLY;
    if (Lbn >= M->BitmapBits || Count > M->BitmapBits - Lbn) return UDF_CORRUPT;
    UdfBufForget(Vol, Part, Lbn, Count);
    st = SetBits(Vol, M, Lbn, Count, TRUE);
    if (!UDF_SUCCESS(st)) return st;
    if (M->FreeKnown) M->Free += Count;
    if (Lbn < M->Hint) M->Hint = Lbn;
    Vol->Changed = TRUE;
    return UDF_OK;
}

/* ---- the metadata partition grows -------------------------------------------------- */
UDF_STATUS UdfIappend(PUDF_VOL Vol, PUDF_INODE Inode, USHORT Part, ULONG Lbn, ULONG Bytes, UCHAR Type);

static UDF_STATUS
AllocUnitIn(PUDF_VOL V, USHORT Part, ULONG Unit, ULONG Align, ULONG Goal, PULONG Lbn)
{
    ULONG tries, lbn, got;
    UDF_STATUS st;
    for (tries = 0; tries < 64; tries++) {
        st = UdfAlloc(V, Part, Goal, Unit, &lbn, &got);
        if (!UDF_SUCCESS(st)) return st;
        if (got == Unit && (Align <= 1 || lbn % Align == 0)) { *Lbn = lbn; return UDF_OK; }
        /* not a whole, aligned unit: give it back and look further on */
        st = UdfFree(V, Part, lbn, got);
        if (!UDF_SUCCESS(st)) return st;
        Goal = lbn + got;
        if (Align > 1) Goal = (Goal + Align - 1) / Align * Align;
        if (Goal >= V->Map[Part].BitmapBits) Goal = 0;
    }
    return UDF_DISK_FULL;
}

static UDF_STATUS
GrowMeta(PUDF_VOL V, PUDF_PMAP M)
{
    ULONG unit = M->AllocUnit ? M->AllocUnit : 32, align = M->AlignUnit ? M->AlignUnit : 1;
    ULONG lbn, lbn2, newBits, needBytes;
    ULONG goal = 0;
    UDF_STATUS st;
    PUDF_INODE F = M->MetaFile;
    if (!F || !M->MetaBitmap) return UDF_DISK_FULL;
    if (F->NExt) goal = F->Ext[F->NExt - 1].Lbn + (F->Ext[F->NExt - 1].Len >> V->BlockShift);
    st = AllocUnitIn(V, M->Under, unit, align, goal, &lbn);
    if (!UDF_SUCCESS(st)) return st;
    st = UdfIappend(V, F, M->Under, lbn, unit << V->BlockShift, UDF_EXT_RECORDED);
    if (!UDF_SUCCESS(st)) return st;
    if (M->MetaMirror) {
        if (M->MetaFlags & 1) {
            st = AllocUnitIn(V, M->Under, unit, align, lbn + unit, &lbn2);
            if (!UDF_SUCCESS(st)) return st;
        } else {
            lbn2 = lbn;
        }
        st = UdfIappend(V, M->MetaMirror, M->Under, lbn2, unit << V->BlockShift, UDF_EXT_RECORDED);
        if (!UDF_SUCCESS(st)) return st;
    }
    newBits = M->Length + unit;
    needBytes = UDF_SBD_MAP + (newBits + 7) / 8;
    if (M->MetaBitmap->Size < needBytes) {
        ULONGLONG have = M->MetaBitmap->Size;
        st = UdfIallocRange(V, M->MetaBitmap, 0, (needBytes + V->BlockSize - 1) >> V->BlockShift, TRUE);
        (VOID)have;
        if (!UDF_SUCCESS(st)) return st;
        st = UdfIsetSize(V, M->MetaBitmap, needBytes);
        if (!UDF_SUCCESS(st)) return st;
    }
    {
        PUDF_BUF b;
        PUCHAR h;
        st = BitmapHeader(V, M, &b, &h);
        if (!UDF_SUCCESS(st)) return st;
        UdfPut32(h + UDF_SBD_BITS, newBits);
        UdfPut32(h + UDF_SBD_BYTES, (newBits + 7) / 8);
        if (b) UdfBufDirty(V, b); else UdfIdirty(V, M->MetaBitmap);
        UdfBufPut(V, b);
    }
    M->BitmapBits = newBits;
    M->Length = newBits;
    st = SetBits(V, M, newBits - unit, unit, TRUE);
    if (!UDF_SUCCESS(st)) return st;
    M->Free += unit;
    UdfIdirty(V, F);
    if (M->MetaMirror) UdfIdirty(V, M->MetaMirror);
    UdfIdirty(V, M->MetaBitmap);
    return UDF_OK;
}

VOID
UdfFreeSpace(PUDF_VOL Vol, PULONGLONG Total, PULONGLONG Free)
{
    PUDF_PMAP M = &Vol->Map[Vol->DataPart];
    if (M->HasBitmap && !M->FreeKnown) (VOID)CountFree(Vol, M);
    *Total = M->Length;
    *Free = M->HasBitmap ? M->Free : 0;
}

ULONGLONG
UdfNewUniqueId(PUDF_VOL Vol)
{
    ULONGLONG id = Vol->NextUniqueId;
    if (id < UDF_FIRST_UNIQUE_ID) id = UDF_FIRST_UNIQUE_ID;
    /* the low 32 bits are recorded in FIDs: they skip the reserved 0-15 */
    if ((ULONG)id < UDF_FIRST_UNIQUE_ID) id = (id & ~0xFFFFFFFFULL) + UDF_FIRST_UNIQUE_ID;
    Vol->NextUniqueId = id + 1;
    return id;
}

/* ---- the logical volume integrity descriptor ------------------------------------------ */
static UDF_STATUS
WriteLvid(PUDF_VOL V, ULONG Type)
{
    PUCHAR d = V->Lvid;
    ULONG np = UdfGet32(d + UDF_LVID_NPART), il = UdfGet32(d + UDF_LVID_IMPLLEN), i, len;
    PUCHAR iu;
    UDF_TIME now;
    UDF_STATUS st;
    if (np != V->NMaps || il < UDF_LVIU_SIZE || UDF_LVID_TABLES + 8 * np + il > V->BlockSize) {
        /* rebuild it in the expected shape */
        ULONG oldIl = il < 1024 ? il : 0;
        UCHAR keep[UDF_LVIU_SIZE];
        UdfMemZero(keep, sizeof(keep));
        if (oldIl >= UDF_LVIU_SIZE && UDF_LVID_TABLES + 8 * np + UDF_LVIU_SIZE <= V->BlockSize)
            UdfMemCopy(keep, d + UDF_LVID_TABLES + 8 * np, UDF_LVIU_SIZE);
        np = V->NMaps;
        il = UDF_LVIU_SIZE;
        UdfMemZero(d + UDF_LVID_NPART, V->BlockSize - UDF_LVID_NPART);
        UdfPut32(d + UDF_LVID_NPART, np);
        UdfPut32(d + UDF_LVID_IMPLLEN, il);
        UdfMemCopy(d + UDF_LVID_TABLES + 8 * np, keep, UDF_LVIU_SIZE);
    }
    UdfTimeNow(V, &now);
    UdfTimePut(d + UDF_LVID_TIME, &now);
    UdfPut32(d + UDF_LVID_TYPE, Type);
    UdfPut64(d + UDF_LVID_UNIQUEID, V->NextUniqueId);
    for (i = 0; i < np; i++) {
        PUDF_PMAP M = &V->Map[i];
        ULONG fr = 0xFFFFFFFF, sz = M->Length;
        if (M->HasBitmap && M->FreeKnown) fr = M->Free;
        else if (M->HasBitmap) fr = UdfGet32(d + UDF_LVID_TABLES + 4 * i);
        UdfPut32(d + UDF_LVID_TABLES + 4 * i, fr);
        UdfPut32(d + UDF_LVID_TABLES + 4 * np + 4 * i, sz);
    }
    iu = d + UDF_LVID_TABLES + 8 * np;
    UdfRegidImpl(iu + UDF_LVIU_IMPL);
    UdfPut32(iu + UDF_LVIU_FILES, V->NumFiles);
    UdfPut32(iu + UDF_LVIU_DIRS, V->NumDirs);
    if (UdfGet16(iu + UDF_LVIU_MINREAD) == 0) UdfPut16(iu + UDF_LVIU_MINREAD, V->Revision < UDF_REV_150 ? V->Revision : V->Revision);
    if (UdfGet16(iu + UDF_LVIU_MINWRITE) == 0) UdfPut16(iu + UDF_LVIU_MINWRITE, V->Revision);
    if (UdfGet16(iu + UDF_LVIU_MAXWRITE) < V->Revision) UdfPut16(iu + UDF_LVIU_MAXWRITE, V->Revision);
    len = UDF_LVID_TABLES + 8 * np + il;
    UdfMemZero(d + len, V->BlockSize - len);
    UdfTagMake(d, UDF_TAG_LVID, V->TagVersion, V->TagSerial, V->LvidBlock, len);
    st = UdfWriteAbs(V, V->LvidBlock, 1, d);
    if (UDF_SUCCESS(st)) V->LvidOpen = (BOOLEAN)(Type == UDF_LVID_OPEN);
    return st;
}

UDF_STATUS
UdfChangeBegin(PUDF_VOL Vol)
{
    UDF_STATUS st;
    if (Vol->ReadOnly) return UDF_READ_ONLY;
    Vol->Changed = TRUE;
    if (Vol->LvidOpen || !Vol->Lvid) return UDF_OK;
    st = WriteLvid(Vol, UDF_LVID_OPEN);
    if (UDF_SUCCESS(st)) st = UdfHostFlush(Vol);
    return st;
}

BOOLEAN UdfDirty(PUDF_VOL Vol) { return (BOOLEAN)(Vol->Changed || Vol->DirtyBlocks || Vol->LvidOpen); }
ULONG   UdfDirtyBytes(PUDF_VOL Vol) { return Vol->DirtyBlocks << Vol->BlockShift; }

/* the bitmaps' descriptor tags: the CRC covers the header only (8 bytes) */
static UDF_STATUS
RetagBitmaps(PUDF_VOL V)
{
    ULONG i;
    for (i = 0; i < V->NMaps; i++) {
        PUDF_PMAP M = &V->Map[i];
        PUDF_BUF b;
        UDF_STATUS st;
        ULONG loc;
        PUCHAR h = NULL;
        if (!M->HasBitmap) continue;
        if (M->Kind == UDF_KIND_META) {
            st = BitmapHeader(V, M, &b, &h);
            /* embedded: the tag names the bitmap file's entry */
            loc = M->MetaBitmap->AdType == UDF_ICB_AD_IN_ICB ? M->MetaBitmap->Loc.Lbn :
                  (M->MetaBitmap->NExt ? M->MetaBitmap->Ext[0].Lbn : 0);
        } else {
            st = UdfBufGet(V, (USHORT)i, M->BitmapLbn, TRUE, &b);
            if (UDF_SUCCESS(st)) h = b->Data;
            loc = M->BitmapLbn;
        }
        if (!UDF_SUCCESS(st)) return st;
        {
            UCHAR hdr[UDF_SBD_MAP];
            UdfMemCopy(hdr, h, UDF_SBD_MAP);
            UdfPut32(hdr + UDF_SBD_BITS, M->BitmapBits);
            UdfPut32(hdr + UDF_SBD_BYTES, (M->BitmapBits + 7) / 8);
            UdfTagMake(hdr, UDF_TAG_SBD, V->TagVersion, V->TagSerial, loc, UDF_SBD_MAP);
            if (UdfMemCmp(hdr, h, UDF_SBD_MAP)) {
                UdfMemCopy(h, hdr, UDF_SBD_MAP);
                if (b) UdfBufDirty(V, b); else UdfIdirty(V, M->MetaBitmap);
            }
        }
        UdfBufPut(V, b);
    }
    return UDF_OK;
}

UDF_STATUS
UdfCommit(PUDF_VOL Vol)
{
    UDF_STATUS st;
    if (Vol->ReadOnly) return UDF_OK;
    if (!Vol->Changed && !Vol->DirtyBlocks && !Vol->LvidOpen) return UDF_OK;
    st = UdfIflushAll(Vol);
    if (UDF_SUCCESS(st)) st = RetagBitmaps(Vol);
    if (UDF_SUCCESS(st)) st = UdfBufFlush(Vol);
    if (UDF_SUCCESS(st)) st = UdfHostFlush(Vol);
    if (UDF_SUCCESS(st) && Vol->Lvid) st = WriteLvid(Vol, UDF_LVID_CLOSE);
    if (UDF_SUCCESS(st)) st = UdfHostFlush(Vol);
    if (UDF_SUCCESS(st)) Vol->Changed = FALSE;
    return st;
}

/* ---- mounting --------------------------------------------------------------------------- */
static UDF_STATUS
LoadLvid(PUDF_VOL V, ULONG Loc, ULONG Len)
{
    ULONG hops = 0, i, n;
    PUCHAR b = V->Scratch;
    V->Lvid = (PUCHAR)UdfHostAlloc(V->BlockSize);
    if (!V->Lvid) return UDF_NO_MEMORY;
    V->LvidBlock = 0;
    while (Len && hops++ < 32) {
        ULONG nextLoc = 0, nextLen = 0;
        n = Len >> V->BlockShift;
        for (i = 0; i < n && i < 4096; i++) {
            if (!UDF_SUCCESS(UdfReadAbs(V, Loc + i, 1, b))) break;
            if (!UdfTagCheck(b, V->BlockSize, UDF_TAG_LVID, Loc + i)) break;
            UdfMemCopy(V->Lvid, b, V->BlockSize);
            V->LvidBlock = Loc + i;
            nextLen = UdfGet32(b + UDF_LVID_NEXT);
            nextLoc = UdfGet32(b + UDF_LVID_NEXT + 4);
            if (UdfGet32(b + UDF_LVID_TYPE) == UDF_LVID_CLOSE && nextLen == 0) { /* keep looking in this extent */ }
        }
        if (!nextLen) break;
        Loc = nextLoc;
        Len = nextLen;
    }
    if (!V->LvidBlock) {
        UdfHostFree(V->Lvid);
        V->Lvid = NULL;
        return UDF_OK;
    }
    {
        PUCHAR d = V->Lvid;
        ULONG np = UdfGet32(d + UDF_LVID_NPART), il = UdfGet32(d + UDF_LVID_IMPLLEN);
        V->LvidOpen = V->WasOpen = (BOOLEAN)(UdfGet32(d + UDF_LVID_TYPE) != UDF_LVID_CLOSE);
        V->NextUniqueId = UdfGet64(d + UDF_LVID_UNIQUEID);
        if (np <= UDF_MAX_MAPS * 4 && UDF_LVID_TABLES + 8 * np + UDF_LVIU_SIZE <= V->BlockSize && il >= UDF_LVIU_SIZE) {
            PUCHAR iu = d + UDF_LVID_TABLES + 8 * np;
            V->NumFiles = UdfGet32(iu + UDF_LVIU_FILES);
            V->NumDirs = UdfGet32(iu + UDF_LVIU_DIRS);
            V->MinRead = UdfGet16(iu + UDF_LVIU_MINREAD);
            V->MinWrite = UdfGet16(iu + UDF_LVIU_MINWRITE);
            V->MaxWrite = UdfGet16(iu + UDF_LVIU_MAXWRITE);
            for (i = 0; i < np && i < V->NMaps; i++) {
                ULONG fr = UdfGet32(d + UDF_LVID_TABLES + 4 * i);
                PUDF_PMAP M = &V->Map[i];
                if (!V->WasOpen && M->HasBitmap && fr != 0xFFFFFFFF && fr <= M->BitmapBits) {
                    M->Free = fr;
                    M->FreeKnown = TRUE;
                }
            }
        }
    }
    return UDF_OK;
}

static ULONG
FsdSerial(const UCHAR *Fsd)
{
    UCHAR c[4] = { 0, 0, 0, 0 };
    ULONG i;
    for (i = 0; i < 512; i++) c[i & 3] = (UCHAR)(c[i & 3] + Fsd[i]);
    return ((ULONG)c[3] << 24) | ((ULONG)c[2] << 16) | ((ULONG)c[1] << 8) | c[0];
}

static PUDF_PMAP
MapFor(PUDF_VOL V, USHORT Number, UCHAR Kind)
{
    ULONG i;
    for (i = 0; i < V->NMaps; i++)
        if (V->Map[i].Number == Number && V->Map[i].Kind == Kind) return &V->Map[i];
    return NULL;
}

static UDF_STATUS
MountAt(PUDF_VOL V, ULONG B, BOOLEAN ReadOnly)
{
    UCHAR avdp[512];
    VDS *S;
    PUCHAR buf;
    ULONG last, i, off, nmaps, maplen;
    UDF_STATUS st;
    const UCHAR *lvd;

    V->BlockSize = B;
    for (V->BlockShift = 0; (1UL << V->BlockShift) < B; V->BlockShift++)
        ;
    if (!FindAnchor(V, B, avdp, &last)) return UDF_UNRECOGNIZED;
    V->Blocks = V->DeviceBytes >> V->BlockShift;
    if (!V->Blocks) V->Blocks = (ULONGLONG)last + 1;
    V->LastBlock = last;
    V->TagVersion = (UCHAR)UdfGet16(avdp + 2);
    V->TagSerial = UdfGet16(avdp + 6);
    V->ExtMax = (UDF_EXT_LEN_MASK >> V->BlockShift) << V->BlockShift;

    S = (VDS *)UdfHostAlloc(sizeof(VDS));
    buf = (PUCHAR)UdfHostAlloc(8192 + B);
    V->Scratch = (PUCHAR)UdfHostAlloc(B);
    V->Zero = (PUCHAR)UdfHostAlloc(B);
    if (!S || !buf || !V->Scratch || !V->Zero) { st = UDF_NO_MEMORY; goto out; }
    UdfMemZero(V->Zero, B);
    UdfMemZero(S, sizeof(VDS));
    st = ReadVds(V, UdfGet32(avdp + 20), UdfGet32(avdp + 16), S, buf, 8192);
    if (!UDF_SUCCESS(st) || !S->Lvd.Have || !S->NPd) {
        VDS *R = S;
        UdfMemZero(R, sizeof(VDS));
        st = ReadVds(V, UdfGet32(avdp + 28), UdfGet32(avdp + 24), R, buf, 8192);
        if (!UDF_SUCCESS(st) || !R->Lvd.Have || !R->NPd) { st = UDF_UNRECOGNIZED; goto out; }
    }
    lvd = S->Lvd.D;
    if (UdfGet32(lvd + UDF_LVD_BLOCKSIZE) != B) { st = UDF_UNRECOGNIZED; goto out; }
    V->LvdBlock[0] = S->Lvd.Block;
    V->PvdBlock[0] = S->Pvd.Have ? S->Pvd.Block : 0;
    V->IuvdBlock[0] = S->Iuvd.Have ? S->Iuvd.Block : 0;
    /* the reserve copies, to keep a new label in both */
    {
        VDS *R = (VDS *)UdfHostAlloc(sizeof(VDS));
        if (R) {
            UdfMemZero(R, sizeof(VDS));
            if (UDF_SUCCESS(ReadVds(V, UdfGet32(avdp + 28), UdfGet32(avdp + 24), R, buf, 8192))) {
                V->LvdBlock[1] = R->Lvd.Have && R->Lvd.Block != S->Lvd.Block ? R->Lvd.Block : 0;
                V->PvdBlock[1] = R->Pvd.Have && R->Pvd.Block != S->Pvd.Block ? R->Pvd.Block : 0;
                V->IuvdBlock[1] = R->Iuvd.Have && R->Iuvd.Block != S->Iuvd.Block ? R->Iuvd.Block : 0;
            }
            UdfHostFree(R);
        }
    }

    /* the domain: UDF and its revision */
    if (UdfRegidIs(lvd + UDF_LVD_DOMAIN, UDF_ID_COMPLIANT)) {
        V->Revision = UdfGet16(lvd + UDF_LVD_DOMAIN + 24);
        if (lvd[UDF_LVD_DOMAIN + 26] & UDF_DOMAIN_HARD_WP) V->HardWriteProtect = TRUE;
        if (lvd[UDF_LVD_DOMAIN + 26] & UDF_DOMAIN_SOFT_WP) V->SoftWriteProtect = TRUE;
    } else {
        V->Revision = UDF_REV_102;
        V->ReadOnlyForced = TRUE;
        V->ReadOnlyWhy = "not a UDF domain (plain ECMA-167)";
    }
    if (V->Revision < UDF_REV_102) V->Revision = UDF_REV_102;

    /* partition maps */
    nmaps = UdfGet32(lvd + UDF_LVD_NMAPS);
    maplen = UdfGet32(lvd + UDF_LVD_MAPLEN);
    if (nmaps == 0 || nmaps > UDF_MAX_MAPS || UDF_LVD_MAPS + maplen > S->Lvd.Len + 512) { st = UDF_UNRECOGNIZED; goto out; }
    for (i = 0, off = UDF_LVD_MAPS; i < nmaps; i++) {
        const UCHAR *pm = lvd + off;
        PUDF_PMAP M = &V->Map[i];
        ULONG j;
        const UCHAR *pd = NULL;
        if (off + 2 > UDF_LVD_MAPS + maplen || pm[1] < 6) { st = UDF_UNRECOGNIZED; goto out; }
        UdfMemZero(M, sizeof(*M));
        if (pm[0] == UDF_PM_TYPE1) {
            M->Kind = UDF_KIND_PHYS;
            M->Number = UdfGet16(pm + 4);
        } else if (pm[0] == UDF_PM_TYPE2 && pm[1] >= 40) {
            M->Number = UdfGet16(pm + UDF_PM2_PARTNUM);
            if (UdfRegidIs(pm + UDF_PM2_IDENT, UDF_ID_SPARABLE)) {
                M->Kind = UDF_KIND_SPAR;
            } else if (UdfRegidIs(pm + UDF_PM2_IDENT, UDF_ID_VIRTUAL)) {
                M->Kind = UDF_KIND_VIRT;
            } else if (UdfRegidIs(pm + UDF_PM2_IDENT, UDF_ID_METADATA) && pm[1] >= 64) {
                M->Kind = UDF_KIND_META;
                M->MetaFileLbn = UdfGet32(pm + UDF_PM_META_FILE);
                M->MetaMirrorLbn = UdfGet32(pm + UDF_PM_META_MIRROR);
                M->MetaBitmapLbn = UdfGet32(pm + UDF_PM_META_BITMAP);
                M->AllocUnit = UdfGet32(pm + UDF_PM_META_ALLOCUNIT);
                M->AlignUnit = UdfGet16(pm + UDF_PM_META_ALIGNUNIT);
                M->MetaFlags = pm[UDF_PM_META_FLAGS];
            } else {
                st = UDF_UNRECOGNIZED;
                goto out;
            }
        } else {
            st = UDF_UNRECOGNIZED;
            goto out;
        }
        for (j = 0; j < S->NPd; j++) if (UdfGet16(S->Pd[j].D + UDF_PD_NUMBER) == M->Number) pd = S->Pd[j].D;
        if (!pd) { st = UDF_UNRECOGNIZED; goto out; }
        M->Start = UdfGet32(pd + UDF_PD_START);
        M->Length = UdfGet32(pd + UDF_PD_LENGTH);
        M->Access = UdfGet32(pd + UDF_PD_ACCESS);
        for (j = 0; j < S->NPd; j++) if (S->Pd[j].D == pd) M->PdBlock[0] = S->Pd[j].Block;
        if (M->Kind == UDF_KIND_PHYS || M->Kind == UDF_KIND_SPAR) {
            ULONG bl = UdfGet32(pd + UDF_PD_PHD + UDF_PHD_USB);
            if (bl & UDF_EXT_LEN_MASK) {
                M->HasBitmap = TRUE;
                M->BitmapLbn = UdfGet32(pd + UDF_PD_PHD + UDF_PHD_USB + 4);
                M->BitmapBits = M->Length;
            } else if (UdfGet32(pd + UDF_PD_PHD + UDF_PHD_UST) & UDF_EXT_LEN_MASK) {
                if (!V->ReadOnlyForced) V->ReadOnlyWhy = "free space is kept in a table (not supported for writing)";
                V->ReadOnlyForced = TRUE;
            }
            if (UdfGet32(pd + UDF_PD_PHD + UDF_PHD_FSB) & UDF_EXT_LEN_MASK || UdfGet32(pd + UDF_PD_PHD + UDF_PHD_FST) & UDF_EXT_LEN_MASK) {
                if (!V->ReadOnlyForced) V->ReadOnlyWhy = "freed space tables (rewritable media) are not supported for writing";
                V->ReadOnlyForced = TRUE;
            }
            if (M->Access == UDF_ACCESS_RO || M->Access == UDF_ACCESS_WORM) {
                if (!V->ReadOnlyForced) V->ReadOnlyWhy = "a read-only or write-once partition";
                V->ReadOnlyForced = TRUE;
            }
            if (M->Kind == UDF_KIND_SPAR) {
                st = LoadSparing(V, M, pm);
                if (!UDF_SUCCESS(st)) goto out;
            }
        }
        off += pm[1];
    }
    V->NMaps = nmaps;
    for (i = 0; i < nmaps; i++) V->Map[i].Under = (USHORT)i;
    /* the maps on top of a physical partition */
    for (i = 0; i < nmaps; i++) {
        PUDF_PMAP M = &V->Map[i], P;
        if (M->Kind != UDF_KIND_VIRT && M->Kind != UDF_KIND_META) continue;
        P = MapFor(V, M->Number, UDF_KIND_PHYS);
        if (!P) P = MapFor(V, M->Number, UDF_KIND_SPAR);
        if (!P) { st = UDF_UNRECOGNIZED; goto out; }
        M->Under = (USHORT)(P - V->Map);
        M->HasBitmap = FALSE;
    }
    for (i = 0; i < nmaps; i++) {
        PUDF_PMAP M = &V->Map[i];
        if (M->Kind == UDF_KIND_META) {
            st = LoadMeta(V, M, i);
            if (!UDF_SUCCESS(st)) goto out;
        } else if (M->Kind == UDF_KIND_VIRT) {
            st = LoadVat(V, M);
            if (!UDF_SUCCESS(st)) goto out;
            if (!V->ReadOnlyForced) V->ReadOnlyWhy = "write-once media (a virtual allocation table)";
            V->ReadOnlyForced = TRUE;
        }
    }

    /* the file set */
    {
        ULONG len;
        PUDF_BUF b;
        UdfLongAdGet(lvd + UDF_LVD_FSD, &len, &V->Fsd);
        if (V->Fsd.Part >= nmaps) { st = UDF_UNRECOGNIZED; goto out; }
        st = UdfBufGet(V, V->Fsd.Part, V->Fsd.Lbn, TRUE, &b);
        if (!UDF_SUCCESS(st)) goto out;
        if (!UdfTagCheck(b->Data, B, UDF_TAG_FSD, V->Fsd.Lbn)) { UdfBufPut(V, b); st = UDF_CORRUPT; goto out; }
        UdfLongAdGet(b->Data + UDF_FSD_ROOT, &len, &V->Root);
        UdfLongAdGet(b->Data + UDF_FSD_SSDIR, &len, &V->SysStreamDir);
        V->HasSysStreamDir = (BOOLEAN)(V->Revision >= UDF_REV_200 && (len & UDF_EXT_LEN_MASK) != 0);
        V->Serial = FsdSerial(b->Data);
        UdfBufPut(V, b);
        if (V->Root.Part >= nmaps) { st = UDF_CORRUPT; goto out; }
    }
    V->IcbPart = V->Root.Part;
    V->DataPart = V->Map[V->IcbPart].Kind == UDF_KIND_META ? V->Map[V->IcbPart].Under : V->IcbPart;

    /* labels */
    UdfDstringToUtf8(lvd + UDF_LVD_IDENT, 128, V->Label, sizeof(V->Label));
    if (S->Pvd.Have) {
        UdfDstringToUtf8(S->Pvd.D + UDF_PVD_VOLID, 32, V->VolId, sizeof(V->VolId));
        UdfMemCopy(V->VolSetId, S->Pvd.D + UDF_PVD_VOLSETID, 128);
    }
    if (!V->Label[0]) UdfMemCopy(V->Label, V->VolId, sizeof(V->VolId));

    /* integrity */
    st = LoadLvid(V, UdfGet32(lvd + UDF_LVD_INTEGRITY + 4), UdfGet32(lvd + UDF_LVD_INTEGRITY));
    if (!UDF_SUCCESS(st)) goto out;
    if (!V->Lvid && !V->ReadOnlyForced) {
        V->ReadOnlyForced = TRUE;
        V->ReadOnlyWhy = "no logical volume integrity descriptor";
    }
    if (V->Revision > UDF_REV_260 || V->MinWrite > UDF_REV_260) {
        V->ReadOnlyForced = TRUE;
        V->ReadOnlyWhy = "a UDF revision newer than 2.60";
    }
    if (V->HardWriteProtect || V->SoftWriteProtect) {
        V->ReadOnlyForced = TRUE;
        V->ReadOnlyWhy = "write-protected (domain flags)";
    }
    if (V->Map[V->DataPart].Kind != UDF_KIND_VIRT && !V->Map[V->DataPart].HasBitmap && !V->ReadOnlyForced) {
        V->ReadOnlyForced = TRUE;
        V->ReadOnlyWhy = "no unallocated space bitmap";
    }
    if (V->Map[V->IcbPart].Kind == UDF_KIND_META && !V->Map[V->IcbPart].HasBitmap && !V->ReadOnlyForced) {
        V->ReadOnlyForced = TRUE;
        V->ReadOnlyWhy = "the metadata partition has no bitmap";
    }
    V->UseEfe = (BOOLEAN)(V->Revision >= UDF_REV_200);
    V->ReadOnly = (BOOLEAN)(ReadOnly || V->ReadOnlyForced);

    /* the root must be readable */
    {
        PUDF_INODE R;
        st = UdfIget(V, &V->Root, &R);
        if (!UDF_SUCCESS(st)) goto out;
        if (!UdfIsDir(R)) { UdfIput(V, R); st = UDF_CORRUPT; goto out; }
        if (V->Revision >= UDF_REV_200) V->UseEfe = R->Extended;
        UdfIput(V, R);
    }
    st = UDF_OK;
out:
    if (S) UdfHostFree(S);
    if (buf) UdfHostFree(buf);
    return st;
}

static VOID
FreeMaps(PUDF_VOL V)
{
    ULONG i;
    for (i = 0; i < UDF_MAX_MAPS; i++) {
        PUDF_PMAP M = &V->Map[i];
        if (M->SpareOrig) UdfHostFree(M->SpareOrig);
        if (M->SpareMap) UdfHostFree(M->SpareMap);
        if (M->Vat) UdfHostFree(M->Vat);
        M->SpareOrig = M->SpareMap = M->Vat = NULL;
        M->MetaFile = M->MetaMirror = M->MetaBitmap = NULL;
    }
}

VOID
UdfUnmount(PUDF_VOL Vol)
{
    ULONG i;
    for (i = 0; i < Vol->NMaps; i++) {
        PUDF_PMAP M = &Vol->Map[i];
        if (M->MetaMirror && M->MetaMirror != M->MetaFile) UdfIput(Vol, M->MetaMirror);
        if (M->MetaFile) UdfIput(Vol, M->MetaFile);
        if (M->MetaBitmap) UdfIput(Vol, M->MetaBitmap);
        M->MetaFile = M->MetaMirror = M->MetaBitmap = NULL;
    }
    UdfIdropAll(Vol);
    UdfBufDropAll(Vol);
    FreeMaps(Vol);
    if (Vol->Lvid) UdfHostFree(Vol->Lvid);
    if (Vol->Scratch) UdfHostFree(Vol->Scratch);
    if (Vol->Zero) UdfHostFree(Vol->Zero);
    Vol->Lvid = Vol->Scratch = Vol->Zero = NULL;
    Vol->NMaps = 0;
}

UDF_STATUS
UdfMount(PUDF_VOL Vol, BOOLEAN ReadOnly)
{
    ULONG i;
    UDF_STATUS st = UDF_UNRECOGNIZED;
    UCHAR tmp[512];
    if (!Vol->BufMax) Vol->BufMax = 2048;
    if (!Vol->InoMax) Vol->InoMax = 1024;
    for (i = 0; i < sizeof(BlockSizes) / sizeof(BlockSizes[0]); i++) {
        ULONG B = BlockSizes[i], step;
        PVOID host = Vol->HostContext;
        ULONG ss = Vol->SectorSize, bm = Vol->BufMax, im = Vol->InoMax;
        ULONGLONG db = Vol->DeviceBytes;
        BOOLEAN opt = Vol->Optical;
        if (ss && B < ss) continue;
        step = B < UDF_VSD_SIZE ? UDF_VSD_SIZE : B;
        if (!HasVrs(Vol, step, tmp)) continue;
        st = MountAt(Vol, B, ReadOnly);
        if (UDF_SUCCESS(st)) {
            /* a boot view (udfboot): its window is allocated last */
            ULONG lo, hi;
            if (!Vol->Optical && UDF_SUCCESS(UdfHostRead(Vol, 0, 512, tmp)) && UdfShimWindow(Vol, tmp, &lo, &hi)) {
                Vol->AvoidPart = Vol->DataPart;
                Vol->AvoidLo = lo;
                Vol->AvoidHi = hi;
            }
            return st;
        }
        UdfUnmount(Vol);
        UdfMemZero(Vol, sizeof(*Vol));
        Vol->HostContext = host; Vol->SectorSize = ss; Vol->DeviceBytes = db; Vol->Optical = opt;
        Vol->BufMax = bm; Vol->InoMax = im;
        if (st != UDF_UNRECOGNIZED) return st;
    }
    return st;
}

/* ---- the label ---------------------------------------------------------------------------------- */
static UDF_STATUS
Rewrite(PUDF_VOL V, ULONG Block, USHORT Ident, ULONG Off, ULONG FieldLen, const char *Utf8, ULONG Len, ULONG DescLen)
{
    PUCHAR b = V->Scratch;
    UDF_STATUS st;
    ULONG crcLen;
    if (!Block) return UDF_OK;
    st = UdfReadAbs(V, Block, 1, b);
    if (!UDF_SUCCESS(st)) return st;
    if (!UdfTagCheck(b, V->BlockSize, Ident, Block)) return UDF_OK;
    crcLen = UdfGet16(b + 10);
    if (!UdfUtf8ToDstring(Utf8, Len, b + Off, FieldLen)) return UDF_BAD_NAME;
    UdfTagMake(b, Ident, UdfGet16(b + 2), UdfGet16(b + 6), Block, DescLen ? DescLen : UDF_TAG_SIZE + crcLen);
    return UdfWriteAbs(V, Block, 1, b);
}

UDF_STATUS
UdfSetLabel(PUDF_VOL Vol, const char *Utf8, ULONG Len)
{
    UCHAR test[128];
    ULONG i, pvdLen = Len;
    UDF_STATUS st;
    PUDF_BUF b;
    if (Vol->ReadOnly) return UDF_READ_ONLY;
    if (!UdfUtf8ToDstring(Utf8, Len, test, 128)) return UDF_BAD_NAME;
    st = UdfChangeBegin(Vol);
    if (!UDF_SUCCESS(st)) return st;
    /* the primary volume identifier is 32 bytes: cut at a character boundary */
    while (pvdLen && !UdfUtf8ToDstring(Utf8, pvdLen, test, 32)) {
        pvdLen--;
        while (pvdLen && ((UCHAR)Utf8[pvdLen] & 0xC0) == 0x80) pvdLen--;
    }
    for (i = 0; i < 2; i++) {
        st = Rewrite(Vol, Vol->LvdBlock[i], UDF_TAG_LVD, UDF_LVD_IDENT, 128, Utf8, Len, 0);
        if (UDF_SUCCESS(st)) st = Rewrite(Vol, Vol->PvdBlock[i], UDF_TAG_PVD, UDF_PVD_VOLID, 32, Utf8, pvdLen, 0);
        if (UDF_SUCCESS(st)) st = Rewrite(Vol, Vol->IuvdBlock[i], UDF_TAG_IUVD, 52 + 64, 128, Utf8, Len, 0);
        if (!UDF_SUCCESS(st)) return st;
    }
    /* the file set descriptor names the logical volume too */
    st = UdfBufGet(Vol, Vol->Fsd.Part, Vol->Fsd.Lbn, TRUE, &b);
    if (!UDF_SUCCESS(st)) return st;
    if (UdfUtf8ToDstring(Utf8, Len, b->Data + UDF_FSD_LVID, 128)) {
        UdfTagMake(b->Data, UDF_TAG_FSD, UdfGet16(b->Data + 2), UdfGet16(b->Data + 6), Vol->Fsd.Lbn, 512);
        UdfBufDirty(Vol, b);
    }
    UdfBufPut(Vol, b);
    UdfMemZero(Vol->Label, sizeof(Vol->Label));
    UdfMemCopy(Vol->Label, Utf8, Len < UDF_MAX_NAME_UTF8 ? Len : UDF_MAX_NAME_UTF8);
    return UDF_OK;
}
