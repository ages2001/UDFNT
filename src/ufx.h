/*
 * UDFNT - the engine interface the driver layer uses (ufx.c implements it
 * on the UDF engine).
 *
 * The driver layer was built around an inode-number, block-mapped file
 * system interface: inodes are read into an in-memory copy, directories are
 * looked up and enumerated by name, and file data moves non-cached between
 * the Cache Manager and the disk through a file-block -> disk-block map.
 * ufx.c gives UDF that shape:
 *
 *   - inode numbers: a file entry in the partition the file set uses at
 *     block L is inode L + 16 (the root directory is UFX_ROOT_INO); file
 *     entries elsewhere get numbers of their own (with the top bit set);
 *   - the data map: recorded extents map to the volume's blocks; holes and
 *     "allocated, not recorded" extents read as zeros and get blocks (or are
 *     recorded, zeroed) on first write;
 *   - files with their data embedded in the file entry carry
 *     UFX_IF_INLINE_DATA: they are read through the engine, and the first
 *     write moves the data into blocks (UfxUninline);
 *   - DOS attributes: hidden is the entry's "hidden" flag, system and archive
 *     the file entry's ICB flags (offered to the layer as user.DOSATTRIB).
 */
#ifndef _UFX_H_
#define _UFX_H_

#include "udfeng.h"

typedef LONG UFX_STATUS;

#define UFX_OK                  ((UFX_STATUS)0x00000000L)
#define UFX_NO_MORE_ENTRIES     ((UFX_STATUS)0x8000001AL)
#define UFX_NOT_FOUND           ((UFX_STATUS)0xC0000034L)
#define UFX_EXISTS              ((UFX_STATUS)0xC0000035L)
#define UFX_DISK_FULL           ((UFX_STATUS)0xC000007FL)
#define UFX_NO_MEMORY           ((UFX_STATUS)0xC000009AL)
#define UFX_CORRUPT             ((UFX_STATUS)0xC0000032L)
#define UFX_FILE_CORRUPT        ((UFX_STATUS)0xC0000102L)
#define UFX_BAD_NAME            ((UFX_STATUS)0xC0000033L)
#define UFX_NOT_EMPTY           ((UFX_STATUS)0xC0000101L)
#define UFX_IO_ERROR            ((UFX_STATUS)0xC0000185L)
#define UFX_READ_ONLY           ((UFX_STATUS)0xC00000A2L)
#define UFX_UNRECOGNIZED        ((UFX_STATUS)0xC000014FL)
#define UFX_TOO_LARGE           ((UFX_STATUS)0xC0000904L)
#define UFX_NOT_SUPPORTED       ((UFX_STATUS)0xC00000BBL)
#define UFX_NOT_A_DIRECTORY     ((UFX_STATUS)0xC0000103L)
#define UFX_IS_A_DIRECTORY      ((UFX_STATUS)0xC00000BAL)
#define UFX_NAME_TOO_LONG       ((UFX_STATUS)0xC0000106L)
#define UFX_NO_SUCH_EA          ((UFX_STATUS)0xC0000052L)
#define UFX_EA_CORRUPT          ((UFX_STATUS)0x80000013L)
#define UFX_BUFFER_TOO_SMALL    ((UFX_STATUS)0xC0000023L)
#define UFX_ACCESS_DENIED       ((UFX_STATUS)0xC0000022L)
#define UFX_TOO_MANY_LINKS      ((UFX_STATUS)0xC0000265L)
#define UFX_NO_SPACE            ((UFX_STATUS)0xC0000907L)

#define UFX_MAX_NAME            255
#define UFX_ROOT_INO            2           /* the root directory */
#define UFX_INO_BIAS            16          /* inode = block + 16 */
#define UFX_VIRTUAL_INO         0x80000000UL /* file entries elsewhere: numbered by a table */

/* the in-memory inode (fields as the driver layer knows them) */
typedef struct _UFX_INODE {
    USHORT      Mode;
    USHORT      UidLo;
    ULONG       SizeLo;
    ULONG       Atime;
    ULONG       Ctime;
    ULONG       Mtime;
    ULONG       Dtime;
    USHORT      GidLo;
    USHORT      LinksCount;
    ULONG       BlocksLo;               /* 512-byte sectors */
    ULONG       Flags;                  /* UFX_IF_* */
    ULONG       Version;
    ULONG       Block[15];
    ULONG       Generation;
    ULONG       FileAclLo;
    ULONG       SizeHi;
    ULONG       ObsoFaddr;
    USHORT      BlocksHi;
    USHORT      FileAclHi;
    USHORT      UidHi;
    USHORT      GidHi;
    USHORT      ChecksumLo;
    USHORT      Reserved;
    USHORT      ExtraIsize;
    USHORT      ChecksumHi;
    ULONG       CtimeExtra;             /* nanoseconds << 2 | epoch bits */
    ULONG       MtimeExtra;
    ULONG       AtimeExtra;
    ULONG       Crtime;
    ULONG       CrtimeExtra;
    ULONG       VersionHi;
    ULONG       ProjId;
    /* UDF */
    ULONGLONG   UniqueId;
} UFX_INODE, *PUFX_INODE;

#define UFX_GOOD_OLD_INODE_SIZE 128

#define UFX_S_IFMT              0xF000
#define UFX_S_IFSOCK            0xC000
#define UFX_S_IFLNK             0xA000
#define UFX_S_IFREG             0x8000
#define UFX_S_IFBLK             0x6000
#define UFX_S_IFDIR             0x4000
#define UFX_S_IFCHR             0x2000
#define UFX_S_IFIFO             0x1000

#define UFX_IF_SYNC             0x00000008
#define UFX_IF_IMMUTABLE        0x00000010
#define UFX_IF_APPEND           0x00000020
#define UFX_IF_NODUMP           0x00000040
#define UFX_IF_NOATIME          0x00000080
#define UFX_IF_INLINE_DATA      0x10000000  /* read through the engine; rewritten on first write */
#define UFX_IF_NEEDS_PREP       0x20000000  /* (not used on UDF) */
#define UFX_IF_SUBVOL_ROOT      0x40000000  /* (not used on UDF) */

#define UFX_FT_UNKNOWN          0
#define UFX_FT_REG_FILE         1
#define UFX_FT_DIR              2
#define UFX_FT_SYMLINK          7

#define UFX_XATTR_INDEX_USER            1
#define UFX_XATTR_INDEX_POSIX_ACCESS    2
#define UFX_XATTR_INDEX_POSIX_DEFAULT   3
#define UFX_XATTR_INDEX_TRUSTED         4
#define UFX_XATTR_INDEX_SECURITY        6
#define UFX_XATTR_INDEX_SYSTEM          7

typedef struct _UFX_ENTRY {
    ULONG       Inode;
    UCHAR       FileType;
    USHORT      NameLength;
    char        Name[UFX_MAX_NAME + 1];
    ULONGLONG   Cookie;
} UFX_ENTRY, *PUFX_ENTRY;

/* a file entry the driver knows by a number of its own */
typedef struct _UFX_VINO {
    UDF_LOC     Loc;
} UFX_VINO;

/* where an inode's name was last seen: for its "hidden" flag */
typedef struct _UFX_SEEN {
    ULONG       Ino;
    ULONG       DirIno;
    ULONGLONG   Offset;
    UCHAR       Hidden;
} UFX_SEEN;

#define UFX_SEEN_SIZE           1024

typedef struct _UFX_VOL {
    PVOID       HostContext;            /* the VCB */
    UDF_VOL     Udf;                    /* the UDF volume */
    /* set by the layer before UfxMount */
    ULONGLONG   DeviceBytes;
    ULONG       DeviceSector;
    BOOLEAN     Optical;

    ULONG       BlockSize;              /* the logical block size */
    ULONG       BlockShift;
    ULONG       SectorsPerBlock;        /* BlockSize / the device sector */
    ULONGLONG   BlocksCount;            /* the volume, in blocks */
    ULONGLONG   DataBlocks;             /* the partition file data goes to */
    ULONGLONG   FreeBlocksCount;
    ULONG       InodesCount, FreeInodesCount;
    ULONG       InodeSize;              /* reports 256: every extra field present */
    ULONG       GroupCount;
    ULONG       NodeSize;
    char        VolumeName[256];        /* the label (UTF-8) */
    ULONG       SerialNumber;
    UCHAR       Uuid[16];               /* the volume set identifier's first 16 bytes */
    BOOLEAN     ReadOnly;
    BOOLEAN     ReadOnlyForced;         /* the volume cannot be written by UDFNT */
    BOOLEAN     Dirty;
    BOOLEAN     DataCopies;             /* never on UDF */

    UFX_VINO   *Vino;
    ULONG       VinoCount, VinoCap;
    UFX_SEEN    Seen[UFX_SEEN_SIZE];
} UFX_VOL, *PUFX_VOL;

/* the host (udnhost.c) */
PVOID       UfxHostAlloc(ULONG Bytes);
VOID        UfxHostFree(PVOID P);
ULONG       UfxHostNow(VOID);

/* volume */
BOOLEAN     UfxIsVrs(const UCHAR *Buffer, ULONG Length, ULONG Step);   /* a UDF recognition sequence */
BOOLEAN     UfxSameVolume(PUFX_VOL Vol);                            /* still the volume mounted? */
UFX_STATUS  UfxMount(PUFX_VOL Vol, BOOLEAN ReadOnly);
VOID        UfxUnmountFree(PUFX_VOL Vol);
UFX_STATUS  UfxStartWriting(PUFX_VOL Vol);
UFX_STATUS  UfxStopWriting(PUFX_VOL Vol);
UFX_STATUS  UfxTxnCommit(PUFX_VOL Vol);
BOOLEAN     UfxTxnPending(PUFX_VOL Vol);
ULONG       UfxDirtyBytes(PUFX_VOL Vol);
UFX_STATUS  UfxSetLabel(PUFX_VOL Vol, const char *Label);
UFX_STATUS  UfxProcessOrphans(PUFX_VOL Vol);
VOID        UfxCountFree(PUFX_VOL Vol);
/* operation boundaries: the engine's time stamp, and failure handling */
VOID        UfxOpBegin(PUFX_VOL Vol);
UFX_STATUS  UfxOpEnd(PUFX_VOL Vol, UFX_STATUS Status);

/* times */
VOID        UfxSetTime(ULONG *Field, ULONG *Extra, ULONGLONG Nt100ns);
ULONGLONG   UfxGetTime(ULONG Field, ULONG Extra);

/* inodes */
UFX_STATUS  UfxReadInode(PUFX_VOL Vol, ULONG Ino, PUFX_INODE Inode);
UFX_STATUS  UfxWriteInode(PUFX_VOL Vol, ULONG Ino, PUFX_INODE Inode);
ULONGLONG   UfxInodeSize(PUFX_VOL Vol, PUFX_INODE Inode);
VOID        UfxInodeSetSize(PUFX_VOL Vol, PUFX_INODE Inode, ULONGLONG Size);
ULONGLONG   UfxInodeBlocks(PUFX_VOL Vol, PUFX_INODE Inode);
ULONGLONG   UfxInodeNumber(PUFX_VOL Vol, ULONG Ino);        /* the unique ID */
ULONG       UfxInoOfLoc(PUFX_VOL Vol, const UDF_LOC *Loc);  /* a file entry's inode number */

/* directories */
UFX_STATUS  UfxLookup(PUFX_VOL Vol, ULONG DirIno, PUFX_INODE Dir, const char *Name, ULONG NameLen, PUFX_ENTRY Entry);
UFX_STATUS  UfxEnum(PUFX_VOL Vol, ULONG DirIno, PUFX_INODE Dir, ULONGLONG Cookie, PUFX_ENTRY Entry);
UFX_STATUS  UfxDirIsEmpty(PUFX_VOL Vol, ULONG DirIno, PUFX_INODE Dir, PBOOLEAN Empty);
UFX_STATUS  UfxGetParent(PUFX_VOL Vol, ULONG DirIno, PUFX_INODE Dir, PULONG Parent);
UFX_STATUS  UfxCreate(PUFX_VOL Vol, ULONG DirIno, PUFX_INODE Dir, const char *Name, ULONG NameLen,
                      USHORT Mode, const char *Target, PULONG NewIno, PUFX_INODE NewInode);
UFX_STATUS  UfxUnlink(PUFX_VOL Vol, ULONG DirIno, PUFX_INODE Dir, const char *Name, ULONG NameLen);
UFX_STATUS  UfxRemoveEntry(PUFX_VOL Vol, ULONG DirIno, PUFX_INODE Dir, const char *Name, ULONG NameLen);
UFX_STATUS  UfxLink(PUFX_VOL Vol, ULONG DirIno, PUFX_INODE Dir, const char *Name, ULONG NameLen,
                    ULONG TargetIno, PUFX_INODE Target);
UFX_STATUS  UfxRename(PUFX_VOL Vol, ULONG OldDir, PUFX_INODE OldDirInode, const char *OldName, ULONG OldLen,
                      ULONG NewDir, PUFX_INODE NewDirInode, const char *NewName, ULONG NewLen);
UFX_STATUS  UfxReadLink(PUFX_VOL Vol, ULONG Ino, PUFX_INODE Inode, char *Buf, ULONG BufSize, PULONG Len);

/* file data */
UFX_STATUS  UfxReadFile(PUFX_VOL Vol, ULONG Ino, PUFX_INODE Inode, ULONGLONG Offset, ULONG Length,
                        PVOID Buffer, PULONG Read);
UFX_STATUS  UfxMapFileBlock(PUFX_VOL Vol, ULONG Ino, PUFX_INODE Inode, ULONGLONG FileBlk,
                            PULONGLONG Disk, PULONG RunLen);
UFX_STATUS  UfxGetFileBlock(PUFX_VOL Vol, ULONG Ino, PUFX_INODE Inode, ULONGLONG FileBlk,
                            PULONGLONG Disk, PBOOLEAN Fresh);
/* gives [FirstBlk, FirstBlk+Count) disk space where it has none (prepared files) */
UFX_STATUS  UfxAllocateRange(PUFX_VOL Vol, ULONG Ino, PUFX_INODE Inode, ULONGLONG FirstBlk, ULONGLONG Count);
UFX_STATUS  UfxUninline(PUFX_VOL Vol, ULONG Ino, PUFX_INODE Inode);
UFX_STATUS  UfxTruncateBlocks(PUFX_VOL Vol, ULONG Ino, PUFX_INODE Inode, ULONGLONG NewBlocks);
/* the device blocks of the other copies of a data block (DUP data) */
ULONG       UfxDataMirrors(PUFX_VOL Vol, ULONGLONG DiskBlock, PULONGLONG Other, ULONG Max);

/* extended attributes (NameIndex: the namespace) */
UFX_STATUS  UfxGetXattr(PUFX_VOL Vol, ULONG Ino, PUFX_INODE Inode, UCHAR NameIndex, const char *Name,
                        ULONG NameLen, PUCHAR Value, ULONG ValueSize, PULONG ValueLen);
UFX_STATUS  UfxSetXattr(PUFX_VOL Vol, ULONG Ino, PUFX_INODE Inode, UCHAR NameIndex, const char *Name,
                        ULONG NameLen, const UCHAR *Value, ULONG ValueLen);
UFX_STATUS  UfxListXattr(PUFX_VOL Vol, ULONG Ino, PUFX_INODE Inode,
                         VOID (*Report)(PVOID Ctx, UCHAR NameIndex, const char *Name, ULONG NameLen,
                                        const UCHAR *Value, ULONG ValueLen), PVOID Ctx);

#endif
