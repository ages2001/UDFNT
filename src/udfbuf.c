/*
 * UDFNT - the metadata block cache and plain block I/O.
 *
 * Blocks are keyed by their volume block number. A block of the metadata
 * partition also knows where its mirror copy is: writing it writes both.
 * Dirty blocks stay until a commit (or until the cache must give room, when
 * they are written at once - UDF has no ordering rule a later commit keeps).
 */
#include "udfeng.h"

#define HASH(a)     ((ULONG)((a) ^ ((a) >> 10)) & (UDF_BUF_HASH - 1))

UDF_STATUS
UdfReadAbs(PUDF_VOL Vol, ULONGLONG Abs, ULONG Count, PVOID Buf)
{
    if (Abs + Count > Vol->Blocks && Vol->Blocks) return UDF_IO_ERROR;
    return UdfHostRead(Vol, Abs << Vol->BlockShift, Count << Vol->BlockShift, Buf);
}

UDF_STATUS
UdfWriteAbs(PUDF_VOL Vol, ULONGLONG Abs, ULONG Count, const VOID *Buf)
{
    if (Vol->ReadOnly) return UDF_READ_ONLY;
    if (Abs + Count > Vol->Blocks && Vol->Blocks) return UDF_IO_ERROR;
    return UdfHostWrite(Vol, Abs << Vol->BlockShift, Count << Vol->BlockShift, Buf);
}

static VOID
LruUnlink(PUDF_VOL V, PUDF_BUF B)
{
    if (B->Prev) B->Prev->Next = B->Next; else V->BufLruHead = B->Next;
    if (B->Next) B->Next->Prev = B->Prev; else V->BufLruTail = B->Prev;
    B->Prev = B->Next = NULL;
}

static VOID
LruFront(PUDF_VOL V, PUDF_BUF B)
{
    B->Prev = NULL;
    B->Next = V->BufLruHead;
    if (V->BufLruHead) V->BufLruHead->Prev = B; else V->BufLruTail = B;
    V->BufLruHead = B;
}

static UDF_STATUS
WriteBuf(PUDF_VOL V, PUDF_BUF B)
{
    UDF_STATUS st = UdfWriteAbs(V, B->Abs, 1, B->Data);
    if (UDF_SUCCESS(st) && B->Mirror != UDF_NO_MIRROR) st = UdfWriteAbs(V, B->Mirror, 1, B->Data);
    if (UDF_SUCCESS(st)) {
        B->Dirty = FALSE;
        V->DirtyBlocks--;
    }
    return st;
}

static VOID
Unhash(PUDF_VOL V, PUDF_BUF B)
{
    PUDF_BUF *pp = &V->BufHash[HASH(B->Abs)];
    while (*pp && *pp != B) pp = &(*pp)->HashNext;
    if (*pp) *pp = B->HashNext;
}

/* frees unreferenced blocks over the limit (writing dirty ones) */
static VOID
Trim(PUDF_VOL V)
{
    PUDF_BUF B = V->BufLruTail, P;
    while (V->BufCount > V->BufMax && B) {
        P = B->Prev;
        if (B->Refs == 0) {
            if (B->Dirty && !UDF_SUCCESS(WriteBuf(V, B))) { B = P; continue; }
            LruUnlink(V, B);
            Unhash(V, B);
            UdfHostFree(B->Data);
            UdfHostFree(B);
            V->BufCount--;
        }
        B = P;
    }
}

UDF_STATUS
UdfBufGetAbs(PUDF_VOL Vol, ULONGLONG Abs, BOOLEAN Read, PUDF_BUF *Out)
{
    PUDF_BUF B;
    UDF_STATUS st;
    for (B = Vol->BufHash[HASH(Abs)]; B; B = B->HashNext) {
        if (B->Abs == Abs) {
            B->Refs++;
            LruUnlink(Vol, B);
            LruFront(Vol, B);
            *Out = B;
            return UDF_OK;
        }
    }
    B = (PUDF_BUF)UdfHostAlloc(sizeof(UDF_BUF));
    if (!B) return UDF_NO_MEMORY;
    UdfMemZero(B, sizeof(*B));
    B->Data = (PUCHAR)UdfHostAlloc(Vol->BlockSize);
    if (!B->Data) { UdfHostFree(B); return UDF_NO_MEMORY; }
    if (Read) {
        st = UdfReadAbs(Vol, Abs, 1, B->Data);
        if (!UDF_SUCCESS(st)) { UdfHostFree(B->Data); UdfHostFree(B); return st; }
    } else {
        UdfMemZero(B->Data, Vol->BlockSize);
    }
    B->Abs = Abs;
    B->Mirror = UDF_NO_MIRROR;
    B->Refs = 1;
    B->HashNext = Vol->BufHash[HASH(Abs)];
    Vol->BufHash[HASH(Abs)] = B;
    LruFront(Vol, B);
    Vol->BufCount++;
    Trim(Vol);
    *Out = B;
    return UDF_OK;
}

UDF_STATUS
UdfBufGet(PUDF_VOL Vol, USHORT Part, ULONG Lbn, BOOLEAN Read, PUDF_BUF *Out)
{
    ULONGLONG Abs, Mirror;
    ULONG Run;
    UDF_STATUS st = UdfTranslate(Vol, Part, Lbn, &Abs, &Run, &Mirror);
    if (!UDF_SUCCESS(st)) return st;
    st = UdfBufGetAbs(Vol, Abs, Read, Out);
    if (UDF_SUCCESS(st)) (*Out)->Mirror = Mirror;
    return st;
}

VOID
UdfBufDirty(PUDF_VOL Vol, PUDF_BUF B)
{
    if (!B->Dirty) {
        B->Dirty = TRUE;
        Vol->DirtyBlocks++;
    }
    Vol->Changed = TRUE;
}

VOID
UdfBufPut(PUDF_VOL Vol, PUDF_BUF B)
{
    if (B && B->Refs) B->Refs--;
    (VOID)Vol;
}

/* blocks freed: their cached copies (dirty or not) must not reach the disk later */
VOID
UdfBufForget(PUDF_VOL Vol, USHORT Part, ULONG Lbn, ULONG Count)
{
    ULONG i;
    for (i = 0; i < Count; i++) {
        ULONGLONG Abs, Mirror;
        ULONG Run;
        PUDF_BUF B;
        if (!UDF_SUCCESS(UdfTranslate(Vol, Part, Lbn + i, &Abs, &Run, &Mirror))) continue;
        for (B = Vol->BufHash[HASH(Abs)]; B; B = B->HashNext) {
            if (B->Abs != Abs) continue;
            if (B->Dirty) { B->Dirty = FALSE; Vol->DirtyBlocks--; }
            if (B->Refs == 0) {
                LruUnlink(Vol, B);
                Unhash(Vol, B);
                UdfHostFree(B->Data);
                UdfHostFree(B);
                Vol->BufCount--;
            }
            break;
        }
    }
}

UDF_STATUS
UdfBufFlush(PUDF_VOL Vol)
{
    PUDF_BUF B;
    UDF_STATUS st = UDF_OK, s;
    ULONG h;
    for (h = 0; h < UDF_BUF_HASH; h++)
        for (B = Vol->BufHash[h]; B; B = B->HashNext)
            if (B->Dirty) {
                s = WriteBuf(Vol, B);
                if (!UDF_SUCCESS(s) && UDF_SUCCESS(st)) st = s;
            }
    return st;
}

VOID
UdfBufDropAll(PUDF_VOL Vol)
{
    PUDF_BUF B, N;
    ULONG h;
    for (h = 0; h < UDF_BUF_HASH; h++) {
        for (B = Vol->BufHash[h]; B; B = N) {
            N = B->HashNext;
            UdfHostFree(B->Data);
            UdfHostFree(B);
        }
        Vol->BufHash[h] = NULL;
    }
    Vol->BufLruHead = Vol->BufLruTail = NULL;
    Vol->BufCount = 0;
    Vol->DirtyBlocks = 0;
}
