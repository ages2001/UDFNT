/* UDFNT - udfmkfs: lays out a new UDF volume (see udfmkfs.c) */
#ifndef _UDFMKFS_H_
#define _UDFMKFS_H_

typedef struct {
    unsigned long long  Bytes;          /* the volume */
    unsigned            BlockSize;      /* 512 - 65536 */
    unsigned            Revision;       /* 0x0102 ... 0x0260 */
    int                 Metadata;       /* a metadata partition (2.50+) */
    int                 Sparable;       /* a sparable partition (1.50+): sparing tables */
    unsigned            Packet;         /* sparable: packet length in blocks (32) */
    unsigned            SpareEntries;   /* sparable: spare packets */
    int                 UseFe;          /* file entries even for 2.00+ (not extended) */
    char                Label[128];     /* UTF-8 */
    unsigned            BootBlocks;     /* /BOOT: blocks kept free at the start of the partition */
    unsigned            Uid, Gid, Mode; /* the root directory */
    unsigned            Serial;         /* for the volume set identifier (0: from the time) */
    unsigned long long  NowNt;          /* the time stamps */
    /* where it goes */
    void               *Ctx;
    int               (*Write)(void *Ctx, unsigned long long Off, const void *Buf, unsigned Len);
    /* out */
    unsigned long long  Blocks;
    unsigned long       PartStart, PartLength, MetaLength;
    const char         *Error;
} UDF_MKFS;

int UdfMakeFs(UDF_MKFS *M);

#endif
