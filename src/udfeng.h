/*
 * UDFNT - the UDF engine: everything that understands the on-disk format,
 * shared by the driver (through ufx.c), the tools and the Linux test tool.
 *
 * One thread at a time (the host serializes). The engine keeps a write-back
 * cache of metadata blocks (descriptors, directories, bitmaps) and of the
 * file entries it has read; file data moves straight between the caller and
 * the disk. A commit writes what is dirty and closes the logical volume
 * integrity descriptor; the first change after it opens the descriptor again.
 */
#ifndef _UDFENG_H_
#define _UDFENG_H_

#ifdef UDF_KERNEL
#include "udfnt.h"
#else
#include "udfusr.h"
#endif
#include "udf.h"

typedef LONG UDF_STATUS;

#define UDF_OK                  ((UDF_STATUS)0x00000000L)
#define UDF_NO_MORE             ((UDF_STATUS)0x8000001AL)
#define UDF_NOT_FOUND           ((UDF_STATUS)0xC0000034L)
#define UDF_EXISTS              ((UDF_STATUS)0xC0000035L)
#define UDF_DISK_FULL           ((UDF_STATUS)0xC000007FL)
#define UDF_NO_MEMORY           ((UDF_STATUS)0xC000009AL)
#define UDF_CORRUPT             ((UDF_STATUS)0xC0000032L)
#define UDF_FILE_CORRUPT        ((UDF_STATUS)0xC0000102L)
#define UDF_BAD_NAME            ((UDF_STATUS)0xC0000033L)
#define UDF_NOT_EMPTY           ((UDF_STATUS)0xC0000101L)
#define UDF_IO_ERROR            ((UDF_STATUS)0xC0000185L)
#define UDF_READ_ONLY           ((UDF_STATUS)0xC00000A2L)
#define UDF_UNRECOGNIZED        ((UDF_STATUS)0xC000014FL)
#define UDF_TOO_LARGE           ((UDF_STATUS)0xC0000904L)
#define UDF_NOT_SUPPORTED       ((UDF_STATUS)0xC00000BBL)
#define UDF_NOT_A_DIRECTORY     ((UDF_STATUS)0xC0000103L)
#define UDF_IS_A_DIRECTORY      ((UDF_STATUS)0xC00000BAL)
#define UDF_NAME_TOO_LONG       ((UDF_STATUS)0xC0000106L)
#define UDF_BUFFER_SMALL        ((UDF_STATUS)0xC0000023L)
#define UDF_TOO_MANY_LINKS      ((UDF_STATUS)0xC0000265L)

#define UDF_SUCCESS(s)          ((LONG)(s) >= 0)

#define UDF_MAX_MAPS            8
#define UDF_MAX_NAME_UTF8       (254 * 3)   /* a name: up to 254 8-bit or 127 16-bit characters */

/* kinds of partition maps */
#define UDF_KIND_PHYS           1           /* type 1 */
#define UDF_KIND_SPAR           2           /* sparable (type 2) */
#define UDF_KIND_VIRT           3           /* virtual, a VAT (type 2, or 1.50's type 2) */
#define UDF_KIND_META           4           /* metadata partition (2.50+) */

/* time: NT time (100 ns since 1601 UTC) plus the recorded time zone */
typedef struct _UDF_TIME {
    ULONGLONG   Nt;
    SHORT       Tz;                         /* minutes east of UTC, or -2047: none */
} UDF_TIME, *PUDF_TIME;

typedef struct _UDF_LOC {
    ULONG       Lbn;
    USHORT      Part;                       /* the partition reference (map index) */
} UDF_LOC, *PUDF_LOC;

/* one extent of a file, in file order */
typedef struct _UDF_EXT {
    ULONGLONG   Off;                        /* file offset of the extent */
    ULONG       Len;                        /* bytes */
    ULONG       Lbn;
    USHORT      Part;
    UCHAR       Type;                       /* UDF_EXT_RECORDED, _ALLOCATED, _SPARSE */
    UCHAR       Pad;
} UDF_EXT, *PUDF_EXT;

typedef struct _UDF_INODE {
    struct _UDF_INODE *HashNext;
    struct _UDF_INODE *LruPrev, *LruNext;
    UDF_LOC     Loc;                        /* the (extended) file entry */
    ULONG       Refs;
    BOOLEAN     Dirty;
    BOOLEAN     Extended;                   /* an extended file entry */
    BOOLEAN     Gone;                       /* deleted: never written again */
    UCHAR       FileType;
    USHORT      IcbFlags;
    UCHAR       IcbTag[UDF_ICBTAG_SIZE];    /* as read (strategy, parent, ...) */
    ULONG       Uid, Gid, Perm;
    USHORT      Links;
    UCHAR       RecFmt, RecAttr;
    ULONG       RecLen;
    ULONGLONG   Size;                       /* information length */
    ULONGLONG   ObjSize;                    /* EFE: object size (the streams too) */
    UDF_TIME    Atime, Mtime, Ctime, Btime; /* access, modification, attribute, creation */
    ULONG       Checkpoint;
    UCHAR       EaIcb[UDF_LONG_AD];
    UCHAR       StreamIcb[UDF_LONG_AD];
    UCHAR       Impl[32];
    ULONGLONG   UniqueId;
    PUCHAR      Ea;                         /* the extended attributes, as recorded */
    ULONG       EaLen;
    UCHAR       AdType;                     /* UDF_ICB_AD_* */
    PUCHAR      Inline;                     /* AdType == UDF_ICB_AD_IN_ICB */
    ULONG       InlineCap;
    PUDF_EXT    Ext;
    ULONG       NExt, CapExt;
    PUDF_LOC    Aed;                        /* blocks of allocation extent descriptors */
    ULONG       NAed, CapAed;
    ULONG       DirVersion;                 /* changes when a directory's FIDs change */
} UDF_INODE, *PUDF_INODE;

typedef struct _UDF_PMAP {
    UCHAR       Kind;
    USHORT      Number;                     /* the partition number */
    USHORT      Under;                      /* the map of the physical partition below */
    ULONG       Start, Length;              /* the partition descriptor's extent, in blocks */
    ULONG       Access;
    ULONG       PdBlock[2];                 /* where the partition descriptor is (main, reserve) */
    /* free space: an unallocated space bitmap */
    BOOLEAN     HasBitmap;
    ULONG       BitmapLbn;                  /* (PHYS/SPAR) partition block of the bitmap descriptor */
    ULONG       BitmapBits;
    ULONG       Free;
    ULONG       Hint;
    BOOLEAN     FreeKnown;
    /* sparable */
    USHORT      Packet;
    ULONG       SpareCount;
    PULONG      SpareOrig, SpareMap;
    /* virtual */
    PULONG      Vat;
    ULONG       VatCount;
    /* metadata */
    PUDF_INODE  MetaFile, MetaMirror, MetaBitmap;
    ULONG       MetaFileLbn, MetaMirrorLbn, MetaBitmapLbn;
    ULONG       AllocUnit;
    USHORT      AlignUnit;
    UCHAR       MetaFlags;
} UDF_PMAP, *PUDF_PMAP;

/* the metadata block cache */
typedef struct _UDF_BUF {
    struct _UDF_BUF *HashNext;
    struct _UDF_BUF *Prev, *Next;           /* LRU */
    ULONGLONG   Abs;                        /* volume block */
    ULONGLONG   Mirror;                     /* the metadata mirror's copy, or ~0 */
    ULONG       Refs;
    BOOLEAN     Dirty;
    PUCHAR      Data;
} UDF_BUF, *PUDF_BUF;

#define UDF_BUF_HASH            1024
#define UDF_INODE_HASH          512
#define UDF_NO_MIRROR           (~(ULONGLONG)0)

typedef struct _UDF_VOL {
    PVOID       HostContext;
    /* the device: set by the host before UdfMount */
    ULONG       SectorSize;                 /* 0: try 512, 1024, 2048, 4096 */
    ULONGLONG   DeviceBytes;                /* the size of the volume (partition) */
    BOOLEAN     Optical;                    /* CD/DVD/BD media */

    ULONG       BlockSize, BlockShift;
    ULONGLONG   Blocks;                     /* the volume, in blocks */
    ULONG       LastBlock;                  /* the last block (the anchor's) */
    ULONG       ExtMax;                     /* the longest extent, block-aligned */
    USHORT      Revision;                   /* UDF revision of the domain */
    USHORT      MinRead, MinWrite, MaxWrite;
    UCHAR       TagVersion;                 /* 2 (NSR02) or 3 (NSR03) */
    USHORT      TagSerial;
    BOOLEAN     ReadOnly;                   /* no writes (mounted so, or it has to be) */
    BOOLEAN     ReadOnlyForced;             /* the volume cannot be written by UDFNT */
    const char *ReadOnlyWhy;

    UDF_PMAP    Map[UDF_MAX_MAPS];
    ULONG       NMaps;
    USHORT      IcbPart;                    /* where file entries and directories go */
    USHORT      DataPart;                   /* where file data goes */

    /* volume descriptors: main and reserve copies, rewritten for a new label */
    ULONG       PvdBlock[2], LvdBlock[2], IuvdBlock[2];
    ULONG       LvidBlock;                  /* the descriptor in use */
    PUCHAR      Lvid;                       /* its block */
    BOOLEAN     LvidOpen;                   /* as recorded */
    BOOLEAN     WasOpen;                    /* open (dirty) when mounted */
    ULONGLONG   NextUniqueId;
    ULONG       NumFiles, NumDirs;
    UDF_LOC     Fsd, Root, SysStreamDir;
    BOOLEAN     HasSysStreamDir;
    char        Label[UDF_MAX_NAME_UTF8 + 1];    /* logical volume identifier, UTF-8 */
    char        VolId[100];                 /* primary volume identifier, UTF-8 */
    UCHAR       VolSetId[128];              /* the dstring as recorded */
    ULONG       Serial;                     /* the NT volume serial number */
    BOOLEAN     UseEfe;                     /* new files get extended file entries */
    BOOLEAN     SoftWriteProtect, HardWriteProtect;

    /* state */
    ULONG       ModCount;                   /* changes made */
    BOOLEAN     Changed;                    /* since the last commit */
    ULONGLONG   Now;                        /* the time stamp of the running operation */
    ULONG       DirtyBlocks;

    /* caches */
    PUDF_BUF    BufHash[UDF_BUF_HASH];
    PUDF_BUF    BufLruHead, BufLruTail;     /* head: most recent */
    ULONG       BufCount, BufMax;
    PUDF_INODE  InoHash[UDF_INODE_HASH];
    PUDF_INODE  InoLruHead, InoLruTail;     /* unreferenced, clean */
    ULONG       InoCount, InoMax;
    USHORT      AvoidPart;                  /* the boot view's window: allocated last */
    ULONG       AvoidLo, AvoidHi;
    PUCHAR      Scratch;                    /* one block */
    PUCHAR      Zero;                       /* one block of zeros */
} UDF_VOL, *PUDF_VOL;

/* a directory entry */
typedef struct _UDF_DIRENT {
    ULONGLONG   Offset;                     /* of the FID in the directory */
    ULONG       FidLen;
    UCHAR       Chars;                      /* UDF_FID_* */
    UDF_LOC     Icb;
    ULONG       UniqueId32;                 /* from the FID's implementation use */
    USHORT      NameLen;                    /* UTF-8 bytes */
    char        Name[UDF_MAX_NAME_UTF8 + 1];
} UDF_DIRENT, *PUDF_DIRENT;

/* ---- the host (driver: udnhost.c; tools: their own) ---------------------- */
UDF_STATUS  UdfHostRead(PUDF_VOL Vol, ULONGLONG Offset, ULONG Length, PVOID Buffer);
UDF_STATUS  UdfHostWrite(PUDF_VOL Vol, ULONGLONG Offset, ULONG Length, const VOID *Buffer);
UDF_STATUS  UdfHostFlush(PUDF_VOL Vol);
PVOID       UdfHostAlloc(ULONG Bytes);
VOID        UdfHostFree(PVOID P);
ULONGLONG   UdfHostNowNt(VOID);

/* ---- support (udfsup.c) ------------------------------------------------ */
USHORT      UdfGet16(const VOID *P);
ULONG       UdfGet32(const VOID *P);
ULONGLONG   UdfGet64(const VOID *P);
VOID        UdfPut16(PVOID P, USHORT V);
VOID        UdfPut32(PVOID P, ULONG V);
VOID        UdfPut64(PVOID P, ULONGLONG V);
VOID        UdfMemZero(PVOID P, ULONG N);
VOID        UdfMemCopy(PVOID D, const VOID *S, ULONG N);
VOID        UdfMemMove(PVOID D, const VOID *S, ULONG N);
int         UdfMemCmp(const VOID *A, const VOID *B, ULONG N);
USHORT      UdfCrc(const VOID *Data, ULONG Length);
/* a descriptor tag: verify (Loc ~0: any location) / make one */
BOOLEAN     UdfTagCheck(const UCHAR *D, ULONG Avail, USHORT Ident, ULONG Loc);
USHORT      UdfTagIdent(const UCHAR *D);
VOID        UdfTagMake(PUCHAR D, USHORT Ident, USHORT Version, USHORT Serial, ULONG Loc, ULONG Length);
/* OSTA compressed unicode (CS0) <-> UTF-8 */
ULONG       UdfCs0ToUtf8(const UCHAR *In, ULONG InLen, char *Out, ULONG OutMax);
ULONG       UdfUtf8ToCs0(const char *In, ULONG InLen, PUCHAR Out, ULONG OutMax);  /* 0: cannot */
ULONG       UdfDstringToUtf8(const UCHAR *D, ULONG FieldLen, char *Out, ULONG OutMax);
BOOLEAN     UdfUtf8ToDstring(const char *In, ULONG InLen, PUCHAR D, ULONG FieldLen);
/* times */
VOID        UdfTimeGet(const UCHAR *Ts, PUDF_TIME T);
VOID        UdfTimePut(PUCHAR Ts, const UDF_TIME *T);
VOID        UdfTimeNow(PUDF_VOL Vol, PUDF_TIME T);
/* entity identifiers */
VOID        UdfRegid(PUCHAR R, const char *Ident, const UCHAR *Suffix, ULONG SuffixLen);
VOID        UdfRegidUdf(PUDF_VOL Vol, PUCHAR R, const char *Ident);    /* UDF suffix: revision, OS */
VOID        UdfRegidImpl(PUCHAR R);                                    /* *UDFNT, OS class Windows NT */
BOOLEAN     UdfRegidIs(const UCHAR *R, const char *Ident);
VOID        UdfCharspec(PUCHAR C);

/* long_ad / short_ad */
VOID        UdfLongAdGet(const UCHAR *A, PULONG Len, PUDF_LOC Loc);
VOID        UdfLongAdPut(PUCHAR A, ULONG Len, const UDF_LOC *Loc);

/* ---- the volume (udfvol.c) --------------------------------------------- */
BOOLEAN     UdfProbe(PUDF_VOL Vol);                 /* a UDF volume recognition sequence is there */
UDF_STATUS  UdfMount(PUDF_VOL Vol, BOOLEAN ReadOnly);
VOID        UdfUnmount(PUDF_VOL Vol);
UDF_STATUS  UdfCommit(PUDF_VOL Vol);                /* everything to the disk, integrity closed */
BOOLEAN     UdfDirty(PUDF_VOL Vol);
ULONG       UdfDirtyBytes(PUDF_VOL Vol);
UDF_STATUS  UdfChangeBegin(PUDF_VOL Vol);           /* the integrity descriptor goes open */
UDF_STATUS  UdfSetLabel(PUDF_VOL Vol, const char *Utf8, ULONG Len);
VOID        UdfFreeSpace(PUDF_VOL Vol, PULONGLONG Total, PULONGLONG Free);
/* block addressing */
UDF_STATUS  UdfTranslate(PUDF_VOL Vol, USHORT Part, ULONG Lbn, PULONGLONG Abs, PULONG Run, PULONGLONG Mirror);
/* metadata blocks through the cache */
UDF_STATUS  UdfBufGet(PUDF_VOL Vol, USHORT Part, ULONG Lbn, BOOLEAN Read, PUDF_BUF *Buf);
UDF_STATUS  UdfBufGetAbs(PUDF_VOL Vol, ULONGLONG Abs, BOOLEAN Read, PUDF_BUF *Buf);
VOID        UdfBufDirty(PUDF_VOL Vol, PUDF_BUF Buf);
VOID        UdfBufPut(PUDF_VOL Vol, PUDF_BUF Buf);
VOID        UdfBufForget(PUDF_VOL Vol, USHORT Part, ULONG Lbn, ULONG Count);
UDF_STATUS  UdfBufFlush(PUDF_VOL Vol);
VOID        UdfBufDropAll(PUDF_VOL Vol);
UDF_STATUS  UdfReadAbs(PUDF_VOL Vol, ULONGLONG Abs, ULONG Count, PVOID Buf);
UDF_STATUS  UdfWriteAbs(PUDF_VOL Vol, ULONGLONG Abs, ULONG Count, const VOID *Buf);
/* space */
UDF_STATUS  UdfAlloc(PUDF_VOL Vol, USHORT Part, ULONG Goal, ULONG Want, PULONG Lbn, PULONG Got);
UDF_STATUS  UdfFree(PUDF_VOL Vol, USHORT Part, ULONG Lbn, ULONG Count);
UDF_STATUS  UdfAllocAligned(PUDF_VOL Vol, USHORT Part, ULONG Lo, ULONG Hi, ULONG Count, ULONG Align, ULONG AlignBase, PULONG Lbn);
UDF_STATUS  UdfRunOf(PUDF_VOL Vol, USHORT Part, ULONG Lbn, ULONG Max, BOOLEAN Free, PULONG Len);
ULONGLONG   UdfNewUniqueId(PUDF_VOL Vol);

/* ---- inodes (udfino.c) --------------------------------------------------- */
UDF_STATUS  UdfIget(PUDF_VOL Vol, const UDF_LOC *Loc, PUDF_INODE *Inode);
VOID        UdfIput(PUDF_VOL Vol, PUDF_INODE Inode);
VOID        UdfIdirty(PUDF_VOL Vol, PUDF_INODE Inode);
UDF_STATUS  UdfIwrite(PUDF_VOL Vol, PUDF_INODE Inode);          /* to the cache */
UDF_STATUS  UdfIflushAll(PUDF_VOL Vol);
VOID        UdfIdropAll(PUDF_VOL Vol);
UDF_STATUS  UdfInew(PUDF_VOL Vol, UCHAR FileType, const UDF_LOC *Near, PUDF_INODE *Inode);
UDF_STATUS  UdfIdelete(PUDF_VOL Vol, PUDF_INODE Inode);         /* frees everything; the caller puts it */
/* data */
#define UDF_MAP_RECORDED        0
#define UDF_MAP_UNRECORDED      1           /* allocated, not recorded: zeros */
#define UDF_MAP_HOLE            2
#define UDF_MAP_INLINE          3
UDF_STATUS  UdfImap(PUDF_VOL Vol, PUDF_INODE Inode, ULONGLONG FileBlk, PULONGLONG Abs, PULONG Run, PUCHAR Kind);
UDF_STATUS  UdfIread(PUDF_VOL Vol, PUDF_INODE Inode, ULONGLONG Off, ULONG Len, PVOID Buf, PULONG Got);
UDF_STATUS  UdfIwriteData(PUDF_VOL Vol, PUDF_INODE Inode, ULONGLONG Off, ULONG Len, const VOID *Buf);
UDF_STATUS  UdfIallocRange(PUDF_VOL Vol, PUDF_INODE Inode, ULONGLONG FirstBlk, ULONGLONG Count, BOOLEAN Zero);
UDF_STATUS  UdfIfreeFrom(PUDF_VOL Vol, PUDF_INODE Inode, ULONGLONG Blocks);
UDF_STATUS  UdfIsetSize(PUDF_VOL Vol, PUDF_INODE Inode, ULONGLONG Size);
UDF_STATUS  UdfIuninline(PUDF_VOL Vol, PUDF_INODE Inode);
UDF_STATUS  UdfIappend(PUDF_VOL Vol, PUDF_INODE Inode, USHORT Part, ULONG Lbn, ULONG Bytes, UCHAR Type);
ULONGLONG   UdfIblocks(PUDF_VOL Vol, PUDF_INODE Inode);          /* recorded + allocated blocks */
/* moves a file's data into [Lo, Hi) of the data partition in Align-block pieces aligned on AlignBase (In),
   or out of that range (!In); Moved: blocks moved */
UDF_STATUS  UdfIrelocate(PUDF_VOL Vol, PUDF_INODE Inode, ULONG Lo, ULONG Hi, ULONG Align, ULONG AlignBase, BOOLEAN In, PULONG Moved);
/* is the file's data inside [Lo, Hi) in aligned pieces (In), or outside it (!In)? */
BOOLEAN     UdfIplaced(PUDF_VOL Vol, PUDF_INODE Inode, ULONG Lo, ULONG Hi, ULONG Align, ULONG AlignBase, BOOLEAN In);
BOOLEAN     UdfIsDir(PUDF_INODE Inode);
/* POSIX mode <-> UDF permissions and flags */
ULONG       UdfIMode(PUDF_INODE Inode);
VOID        UdfISetMode(PUDF_INODE Inode, ULONG Mode);

/* ---- directories and names (udfdir.c) ------------------------------------ */
UDF_STATUS  UdfDirNext(PUDF_VOL Vol, PUDF_INODE Dir, ULONGLONG Cookie, PUDF_DIRENT Ent);   /* Cookie: offset to start at */
UDF_STATUS  UdfDirLookup(PUDF_VOL Vol, PUDF_INODE Dir, const char *Name, ULONG Len, PUDF_DIRENT Ent);
UDF_STATUS  UdfDirParent(PUDF_VOL Vol, PUDF_INODE Dir, PUDF_LOC Parent);
UDF_STATUS  UdfDirIsEmpty(PUDF_VOL Vol, PUDF_INODE Dir, PBOOLEAN Empty);
UDF_STATUS  UdfDirSetChars(PUDF_VOL Vol, PUDF_INODE Dir, ULONGLONG Offset, UCHAR Set, UCHAR Clear);
/* operations (keep link counts, unique IDs and the file and directory counts) */
#define UDF_CREATE_FILE         0
#define UDF_CREATE_DIR          1
#define UDF_CREATE_SYMLINK      2
UDF_STATUS  UdfCreate(PUDF_VOL Vol, PUDF_INODE Dir, const char *Name, ULONG Len, ULONG Kind, ULONG Mode,
                      ULONG Uid, ULONG Gid, const char *Target, ULONG TargetLen, PUDF_INODE *New);
UDF_STATUS  UdfUnlink(PUDF_VOL Vol, PUDF_INODE Dir, const char *Name, ULONG Len, BOOLEAN KeepInode);
UDF_STATUS  UdfLink(PUDF_VOL Vol, PUDF_INODE Dir, const char *Name, ULONG Len, PUDF_INODE Target);
UDF_STATUS  UdfRename(PUDF_VOL Vol, PUDF_INODE OldDir, const char *OldName, ULONG OldLen,
                      PUDF_INODE NewDir, const char *NewName, ULONG NewLen);
UDF_STATUS  UdfReleaseOrphan(PUDF_VOL Vol, PUDF_INODE Inode);   /* links 0: free it */
UDF_STATUS  UdfReadLink(PUDF_VOL Vol, PUDF_INODE Inode, char *Out, ULONG OutMax, PULONG Len);

#endif
