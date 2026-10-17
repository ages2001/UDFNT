/*
 * UDFNT - support: byte order, CRCs and descriptor tags, OSTA compressed
 * unicode, time stamps, entity identifiers.
 */
#include "udfeng.h"

USHORT    UdfGet16(const VOID *P) { const UCHAR *b = (const UCHAR *)P; return (USHORT)(b[0] | (b[1] << 8)); }
ULONG     UdfGet32(const VOID *P) { const UCHAR *b = (const UCHAR *)P; return b[0] | ((ULONG)b[1] << 8) | ((ULONG)b[2] << 16) | ((ULONG)b[3] << 24); }
ULONGLONG UdfGet64(const VOID *P) { const UCHAR *b = (const UCHAR *)P; return UdfGet32(b) | ((ULONGLONG)UdfGet32(b + 4) << 32); }
VOID UdfPut16(PVOID P, USHORT V) { PUCHAR b = (PUCHAR)P; b[0] = (UCHAR)V; b[1] = (UCHAR)(V >> 8); }
VOID UdfPut32(PVOID P, ULONG V) { PUCHAR b = (PUCHAR)P; b[0] = (UCHAR)V; b[1] = (UCHAR)(V >> 8); b[2] = (UCHAR)(V >> 16); b[3] = (UCHAR)(V >> 24); }
VOID UdfPut64(PVOID P, ULONGLONG V) { UdfPut32(P, (ULONG)V); UdfPut32((PUCHAR)P + 4, (ULONG)(V >> 32)); }

VOID UdfMemZero(PVOID P, ULONG N) { PUCHAR b = (PUCHAR)P; while (N--) *b++ = 0; }
VOID UdfMemCopy(PVOID D, const VOID *S, ULONG N) { PUCHAR d = (PUCHAR)D; const UCHAR *s = (const UCHAR *)S; while (N--) *d++ = *s++; }
VOID
UdfMemMove(PVOID D, const VOID *S, ULONG N)
{
    PUCHAR d = (PUCHAR)D;
    const UCHAR *s = (const UCHAR *)S;
    if (d < s) { while (N--) *d++ = *s++; }
    else { d += N; s += N; while (N--) *--d = *--s; }
}
int
UdfMemCmp(const VOID *A, const VOID *B, ULONG N)
{
    const UCHAR *a = (const UCHAR *)A, *b = (const UCHAR *)B;
    for (; N; N--, a++, b++) if (*a != *b) return *a < *b ? -1 : 1;
    return 0;
}

/* ---- CRC-ITU-T (x^16 + x^12 + x^5 + 1), initial 0 ---------------------- */
static USHORT CrcTable[256];
static BOOLEAN CrcReady;

USHORT
UdfCrc(const VOID *Data, ULONG Length)
{
    const UCHAR *p = (const UCHAR *)Data;
    USHORT crc = 0;
    if (!CrcReady) {
        ULONG i, j;
        for (i = 0; i < 256; i++) {
            USHORT c = (USHORT)(i << 8);
            for (j = 0; j < 8; j++) c = (USHORT)((c & 0x8000) ? (c << 1) ^ 0x1021 : (c << 1));
            CrcTable[i] = c;
        }
        CrcReady = TRUE;
    }
    while (Length--) crc = (USHORT)((crc << 8) ^ CrcTable[((crc >> 8) ^ *p++) & 0xFF]);
    return crc;
}

/* ---- descriptor tags ---------------------------------------------------- */
static UCHAR
TagSum(const UCHAR *D)
{
    ULONG i;
    UCHAR s = 0;
    for (i = 0; i < 16; i++) if (i != 4) s = (UCHAR)(s + D[i]);
    return s;
}

USHORT UdfTagIdent(const UCHAR *D) { return UdfGet16(D); }

BOOLEAN
UdfTagCheck(const UCHAR *D, ULONG Avail, USHORT Ident, ULONG Loc)
{
    ULONG crcLen;
    if (Avail < UDF_TAG_SIZE) return FALSE;
    if (UdfGet16(D) != Ident) return FALSE;
    if (TagSum(D) != D[4]) return FALSE;
    if (Loc != (ULONG)~0UL && UdfGet32(D + 12) != Loc) return FALSE;
    crcLen = UdfGet16(D + 10);
    if (crcLen > Avail - UDF_TAG_SIZE) return FALSE;
    if (crcLen && UdfCrc(D + UDF_TAG_SIZE, crcLen) != UdfGet16(D + 8)) return FALSE;
    return TRUE;
}

/* Length: the whole descriptor (tag included) */
VOID
UdfTagMake(PUCHAR D, USHORT Ident, USHORT Version, USHORT Serial, ULONG Loc, ULONG Length)
{
    ULONG crcLen = Length > UDF_TAG_SIZE ? Length - UDF_TAG_SIZE : 0;
    if (crcLen > 0xFFFF) crcLen = 0xFFFF;
    UdfPut16(D, Ident);
    UdfPut16(D + 2, Version);
    D[5] = 0;
    UdfPut16(D + 6, Serial);
    UdfPut16(D + 8, UdfCrc(D + UDF_TAG_SIZE, crcLen));
    UdfPut16(D + 10, (USHORT)crcLen);
    UdfPut32(D + 12, Loc);
    D[4] = TagSum(D);
}

/* ---- OSTA compressed unicode -------------------------------------------- */
static ULONG
PutUtf8(ULONG C, char *Out, ULONG Pos, ULONG Max)
{
    if (C < 0x80) { if (Pos + 1 > Max) return 0; Out[Pos] = (char)C; return 1; }
    if (C < 0x800) { if (Pos + 2 > Max) return 0; Out[Pos] = (char)(0xC0 | (C >> 6)); Out[Pos + 1] = (char)(0x80 | (C & 0x3F)); return 2; }
    if (C < 0x10000) {
        if (Pos + 3 > Max) return 0;
        Out[Pos] = (char)(0xE0 | (C >> 12)); Out[Pos + 1] = (char)(0x80 | ((C >> 6) & 0x3F)); Out[Pos + 2] = (char)(0x80 | (C & 0x3F));
        return 3;
    }
    if (Pos + 4 > Max) return 0;
    Out[Pos] = (char)(0xF0 | (C >> 18)); Out[Pos + 1] = (char)(0x80 | ((C >> 12) & 0x3F));
    Out[Pos + 2] = (char)(0x80 | ((C >> 6) & 0x3F)); Out[Pos + 3] = (char)(0x80 | (C & 0x3F));
    return 4;
}

/* the d-characters of a name: compression ID, then the characters. Returns UTF-8 bytes. */
ULONG
UdfCs0ToUtf8(const UCHAR *In, ULONG InLen, char *Out, ULONG OutMax)
{
    ULONG i, n = 0, k;
    UCHAR id;
    if (InLen == 0) return 0;
    id = In[0];
    if (id == 8 || id == 254) {
        for (i = 1; i < InLen; i++) {
            k = PutUtf8(In[i], Out, n, OutMax);
            if (!k) break;
            n += k;
        }
    } else if (id == 16 || id == 255) {
        for (i = 1; i + 1 < InLen; i += 2) {
            ULONG c = ((ULONG)In[i] << 8) | In[i + 1];
            if (c >= 0xD800 && c < 0xDC00 && i + 3 < InLen) {
                ULONG d = ((ULONG)In[i + 2] << 8) | In[i + 3];
                if (d >= 0xDC00 && d < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); i += 2; }
            }
            k = PutUtf8(c, Out, n, OutMax);
            if (!k) break;
            n += k;
        }
    }
    if (n < OutMax) Out[n] = 0;
    return n;
}

static ULONG
GetUtf8(const char *S, ULONG Len, ULONG *Pos, PULONG C)
{
    const UCHAR *s = (const UCHAR *)S + *Pos;
    ULONG left = Len - *Pos, c, need, i;
    if (!left) return 0;
    if (s[0] < 0x80) { *C = s[0]; (*Pos)++; return 1; }
    if ((s[0] & 0xE0) == 0xC0) { need = 1; c = s[0] & 0x1F; }
    else if ((s[0] & 0xF0) == 0xE0) { need = 2; c = s[0] & 0x0F; }
    else if ((s[0] & 0xF8) == 0xF0) { need = 3; c = s[0] & 0x07; }
    else return 0;
    if (left < need + 1) return 0;
    for (i = 1; i <= need; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        c = (c << 6) | (s[i] & 0x3F);
    }
    *C = c;
    *Pos += need + 1;
    return 1;
}

/* UTF-8 -> d-characters (compression ID first). Returns bytes, 0 if it cannot be represented. */
ULONG
UdfUtf8ToCs0(const char *In, ULONG InLen, PUCHAR Out, ULONG OutMax)
{
    ULONG pos = 0, c, n, wide = 0;
    while (pos < InLen) {
        if (!GetUtf8(In, InLen, &pos, &c)) return 0;
        if (c > 0xFF) wide = 1;
        if (c == 0) return 0;
    }
    if (OutMax < 1) return 0;
    Out[0] = (UCHAR)(wide ? 16 : 8);
    n = 1;
    pos = 0;
    while (pos < InLen) {
        GetUtf8(In, InLen, &pos, &c);
        if (!wide) {
            if (n + 1 > OutMax) return 0;
            Out[n++] = (UCHAR)c;
        } else if (c >= 0x10000) {
            ULONG hi = 0xD800 + ((c - 0x10000) >> 10), lo = 0xDC00 + ((c - 0x10000) & 0x3FF);
            if (n + 4 > OutMax) return 0;
            Out[n++] = (UCHAR)(hi >> 8); Out[n++] = (UCHAR)hi;
            Out[n++] = (UCHAR)(lo >> 8); Out[n++] = (UCHAR)lo;
        } else {
            if (n + 2 > OutMax) return 0;
            Out[n++] = (UCHAR)(c >> 8); Out[n++] = (UCHAR)c;
        }
    }
    return n;
}

/* a dstring: the used length in its last byte */
ULONG
UdfDstringToUtf8(const UCHAR *D, ULONG FieldLen, char *Out, ULONG OutMax)
{
    ULONG used = D[FieldLen - 1];
    if (used > FieldLen - 1) used = FieldLen - 1;
    if (used == 0) { if (OutMax) Out[0] = 0; return 0; }
    return UdfCs0ToUtf8(D, used, Out, OutMax);
}

BOOLEAN
UdfUtf8ToDstring(const char *In, ULONG InLen, PUCHAR D, ULONG FieldLen)
{
    ULONG n;
    UdfMemZero(D, FieldLen);
    if (InLen == 0) return TRUE;
    n = UdfUtf8ToCs0(In, InLen, D, FieldLen - 1);
    if (n == 0) return FALSE;
    D[FieldLen - 1] = (UCHAR)n;
    return TRUE;
}

/* ---- time stamps ---------------------------------------------------------- */
#define NT_PER_SEC      10000000ULL
#define NT_DAY          (86400ULL * NT_PER_SEC)

/* days from 1601-01-01 to y-m-d (proleptic Gregorian) */
static LONGLONG
DaysFrom1601(LONG y, ULONG m, ULONG d)
{
    LONGLONG era, yoe, doy, doe, days1970;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    days1970 = era * 146097 + doe - 719468;
    return days1970 + 134774;           /* 1601 -> 1970 */
}

static VOID
CivilFrom1601(LONGLONG Days, LONG *Y, ULONG *M, ULONG *D)
{
    LONGLONG z = Days - 134774 + 719468, era, doe, yoe, doy, mp, y;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *D = (ULONG)(doy - (153 * mp + 2) / 5 + 1);
    *M = (ULONG)(mp < 10 ? mp + 3 : mp - 9);
    *Y = (LONG)(y + (*M <= 2));
}

VOID
UdfTimeGet(const UCHAR *Ts, PUDF_TIME T)
{
    USHORT tt = UdfGet16(Ts);
    SHORT year = (SHORT)UdfGet16(Ts + 2);
    ULONG mon = Ts[4], day = Ts[5], hr = Ts[6], min = Ts[7], sec = Ts[8];
    ULONG cs = Ts[9], hus = Ts[10], us = Ts[11];
    SHORT tz = (SHORT)(tt & 0xFFF);
    LONGLONG t;
    if (tz & 0x800) tz = (SHORT)(tz | 0xF000);
    T->Tz = tz;
    if (year < 1601 || mon < 1 || mon > 12 || day < 1 || day > 31 || hr > 23 || min > 59 || sec > 60) {
        T->Nt = 0;
        return;
    }
    t = DaysFrom1601(year, mon, day) * (LONGLONG)NT_DAY +
        ((LONGLONG)hr * 3600 + min * 60 + sec) * (LONGLONG)NT_PER_SEC +
        (LONGLONG)(cs > 99 ? 0 : cs) * 100000 + (LONGLONG)(hus > 99 ? 0 : hus) * 1000 + (LONGLONG)(us > 99 ? 0 : us) * 10;
    if ((tt >> 12) == 1 && tz >= -1440 && tz <= 1440) t -= (LONGLONG)tz * 60 * (LONGLONG)NT_PER_SEC;
    T->Nt = t < 0 ? 0 : (ULONGLONG)t;
}

VOID
UdfTimePut(PUCHAR Ts, const UDF_TIME *T)
{
    SHORT tz = T->Tz;
    LONGLONG t = (LONGLONG)T->Nt, days, rem;
    LONG y;
    ULONG m, d;
    if (tz < -1440 || tz > 1440) tz = 0;
    t += (LONGLONG)tz * 60 * (LONGLONG)NT_PER_SEC;
    if (t < 0) t = 0;
    days = t / (LONGLONG)NT_DAY;
    rem = t % (LONGLONG)NT_DAY;
    CivilFrom1601(days, &y, &m, &d);
    UdfPut16(Ts, (USHORT)(0x1000 | ((USHORT)tz & 0xFFF)));
    UdfPut16(Ts + 2, (USHORT)y);
    Ts[4] = (UCHAR)m;
    Ts[5] = (UCHAR)d;
    Ts[6] = (UCHAR)(rem / (3600 * (LONGLONG)NT_PER_SEC));
    Ts[7] = (UCHAR)(rem / (60 * (LONGLONG)NT_PER_SEC) % 60);
    Ts[8] = (UCHAR)(rem / (LONGLONG)NT_PER_SEC % 60);
    rem %= (LONGLONG)NT_PER_SEC;
    Ts[9] = (UCHAR)(rem / 100000);
    Ts[10] = (UCHAR)(rem / 1000 % 100);
    Ts[11] = (UCHAR)(rem / 10 % 100);
}

VOID
UdfTimeNow(PUDF_VOL Vol, PUDF_TIME T)
{
    T->Nt = Vol->Now ? Vol->Now : UdfHostNowNt();
    T->Tz = 0;
}

/* ---- entity identifiers ------------------------------------------------------- */
VOID
UdfRegid(PUCHAR R, const char *Ident, const UCHAR *Suffix, ULONG SuffixLen)
{
    ULONG i;
    UdfMemZero(R, 32);
    for (i = 0; Ident[i] && i < 23; i++) R[1 + i] = (UCHAR)Ident[i];
    if (Suffix) UdfMemCopy(R + 24, Suffix, SuffixLen > 8 ? 8 : SuffixLen);
}

VOID
UdfRegidUdf(PUDF_VOL Vol, PUCHAR R, const char *Ident)
{
    UCHAR s[8];
    UdfMemZero(s, 8);
    UdfPut16(s, Vol->Revision);
    s[2] = 6;               /* OS class: Windows NT */
    s[3] = 0;
    UdfRegid(R, Ident, s, 8);
}

VOID
UdfRegidImpl(PUCHAR R)
{
    UCHAR s[8];
    UdfMemZero(s, 8);
    s[0] = 6;               /* Windows NT */
    s[1] = 0;
    UdfRegid(R, UDF_ID_IMPL, s, 8);
}

BOOLEAN
UdfRegidIs(const UCHAR *R, const char *Ident)
{
    ULONG i;
    for (i = 0; Ident[i]; i++) if (R[1 + i] != (UCHAR)Ident[i]) return FALSE;
    return (BOOLEAN)(i >= 23 || R[1 + i] == 0);
}

VOID
UdfCharspec(PUCHAR C)
{
    ULONG i;
    const char *s = UDF_CS0_INFO;
    UdfMemZero(C, 64);
    C[0] = 0;
    for (i = 0; s[i]; i++) C[1 + i] = (UCHAR)s[i];
}

/* ---- allocation descriptors ------------------------------------------------- */
VOID
UdfLongAdGet(const UCHAR *A, PULONG Len, PUDF_LOC Loc)
{
    *Len = UdfGet32(A);
    Loc->Lbn = UdfGet32(A + 4);
    Loc->Part = UdfGet16(A + 8);
}

VOID
UdfLongAdPut(PUCHAR A, ULONG Len, const UDF_LOC *Loc)
{
    UdfPut32(A, Len);
    UdfPut32(A + 4, Loc->Lbn);
    UdfPut16(A + 8, Loc->Part);
}
