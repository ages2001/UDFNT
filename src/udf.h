/*
 * UDFNT - the on-disk format: ECMA-167 (3rd edition) as the OSTA Universal
 * Disk Format profiles it, revisions 1.02, 1.50, 2.00, 2.01, 2.50 and 2.60.
 *
 * Structures are read and written through byte offsets (no packed structs),
 * little-endian, so the same code builds for i386, amd64 and the tools.
 */
#ifndef _UDF_H_
#define _UDF_H_

/* ---- descriptor tags (ECMA-167 3/7.2, 4/7.2) ---------------------------- */
#define UDF_TAG_PVD             1       /* primary volume descriptor */
#define UDF_TAG_AVDP            2       /* anchor volume descriptor pointer */
#define UDF_TAG_VDP             3       /* volume descriptor pointer */
#define UDF_TAG_IUVD            4       /* implementation use volume descriptor */
#define UDF_TAG_PD              5       /* partition descriptor */
#define UDF_TAG_LVD             6       /* logical volume descriptor */
#define UDF_TAG_USD             7       /* unallocated space descriptor */
#define UDF_TAG_TD              8       /* terminating descriptor */
#define UDF_TAG_LVID            9       /* logical volume integrity descriptor */
#define UDF_TAG_FSD             256     /* file set descriptor */
#define UDF_TAG_FID             257     /* file identifier descriptor */
#define UDF_TAG_AED             258     /* allocation extent descriptor */
#define UDF_TAG_IE              259     /* indirect entry */
#define UDF_TAG_TE              260     /* terminal entry */
#define UDF_TAG_FE              261     /* file entry */
#define UDF_TAG_EAHD            262     /* extended attribute header descriptor */
#define UDF_TAG_USE             263     /* unallocated space entry */
#define UDF_TAG_SBD             264     /* space bitmap descriptor */
#define UDF_TAG_PIE             265     /* partition integrity entry */
#define UDF_TAG_EFE             266     /* extended file entry */
#define UDF_TAG_SPARING         0       /* UDF sparing table (tag 0, "*UDF Sparing Table") */

#define UDF_TAG_SIZE            16

/* ---- the volume recognition sequence (ECMA-167 2/9) --------------------- */
#define UDF_VRS_OFFSET          32768   /* byte offset of the first descriptor */
#define UDF_VSD_SIZE            2048    /* each descriptor takes max(2048, sector) */

/* ---- the anchor ------------------------------------------------------- */
#define UDF_ANCHOR_LBN          256

/* ---- extents and addresses ---------------------------------------------- */
#define UDF_EXT_RECORDED        0       /* allocated and recorded */
#define UDF_EXT_ALLOCATED       1       /* allocated, not recorded (reads as zeros) */
#define UDF_EXT_SPARSE          2       /* neither (a hole) */
#define UDF_EXT_NEXT            3       /* the next extent of allocation descriptors */
#define UDF_EXT_LEN_MASK        0x3FFFFFFF
#define UDF_EXT_MAX             0x3FFFF800  /* the largest block-aligned length (2 KB blocks; trimmed per volume) */

#define UDF_SHORT_AD            8
#define UDF_LONG_AD             16
#define UDF_EXT_AD              20

/* ICB tag flags: allocation descriptor type (bits 0-2) */
#define UDF_ICB_AD_SHORT        0
#define UDF_ICB_AD_LONG         1
#define UDF_ICB_AD_EXT          2
#define UDF_ICB_AD_IN_ICB       3
#define UDF_ICB_AD_MASK         7
#define UDF_ICB_SORTED          0x0008
#define UDF_ICB_NONRELOC        0x0010
#define UDF_ICB_ARCHIVE         0x0020
#define UDF_ICB_SETUID          0x0040
#define UDF_ICB_SETGID          0x0080
#define UDF_ICB_STICKY          0x0100
#define UDF_ICB_CONTIGUOUS      0x0200
#define UDF_ICB_SYSTEM          0x0400
#define UDF_ICB_TRANSFORMED     0x0800
#define UDF_ICB_MULTIVERSIONS   0x1000
#define UDF_ICB_STREAM          0x2000

/* ICB file types (ECMA-167 4/14.6.6, UDF 2.x) */
#define UDF_FT_UNSPECIFIED      0
#define UDF_FT_USE              1
#define UDF_FT_PIE              2
#define UDF_FT_IE               3
#define UDF_FT_DIRECTORY        4
#define UDF_FT_FILE             5
#define UDF_FT_BLOCKDEV         6
#define UDF_FT_CHARDEV          7
#define UDF_FT_EA               8
#define UDF_FT_FIFO             9
#define UDF_FT_SOCKET           10
#define UDF_FT_TE               11
#define UDF_FT_SYMLINK          12
#define UDF_FT_STREAMDIR        13
#define UDF_FT_VAT20            248
#define UDF_FT_REALTIME         249
#define UDF_FT_METADATA         250
#define UDF_FT_METAMIRROR       251
#define UDF_FT_METABITMAP       252

/* File identifier characteristics */
#define UDF_FID_HIDDEN          0x01    /* "existence" */
#define UDF_FID_DIRECTORY       0x02
#define UDF_FID_DELETED         0x04
#define UDF_FID_PARENT          0x08
#define UDF_FID_METADATA        0x10

/* Permissions (4/14.9.5): other bits 0-4, group 5-9, owner 10-14 */
#define UDF_PERM_EXEC           0x01
#define UDF_PERM_WRITE          0x02
#define UDF_PERM_READ           0x04
#define UDF_PERM_CHATTR         0x08
#define UDF_PERM_DELETE         0x10

/* Partition access types */
#define UDF_ACCESS_POW          0       /* pseudo-overwritable (2.60) */
#define UDF_ACCESS_RO           1
#define UDF_ACCESS_WORM         2
#define UDF_ACCESS_REWRITE      3
#define UDF_ACCESS_OVERWRITE    4

/* LVID integrity types */
#define UDF_LVID_OPEN           0
#define UDF_LVID_CLOSE          1

/* Domain flags (in the domain identifier suffix) */
#define UDF_DOMAIN_HARD_WP      0x01
#define UDF_DOMAIN_SOFT_WP      0x02

/* ---- sizes and offsets ------------------------------------------------- */
/* Partition descriptor */
#define UDF_PD_FLAGS            20
#define UDF_PD_NUMBER           22
#define UDF_PD_CONTENTS         24
#define UDF_PD_PHD              56      /* partition header descriptor */
#define UDF_PD_ACCESS           184
#define UDF_PD_START            188
#define UDF_PD_LENGTH           192
#define UDF_PD_IMPL             196
/* Partition header descriptor (relative to UDF_PD_PHD) */
#define UDF_PHD_UST             0       /* unallocated space table */
#define UDF_PHD_USB             8       /* unallocated space bitmap */
#define UDF_PHD_PIT             16
#define UDF_PHD_FST             24      /* freed space table */
#define UDF_PHD_FSB             32      /* freed space bitmap */

/* Logical volume descriptor */
#define UDF_LVD_CHARSET         20
#define UDF_LVD_IDENT           84      /* dstring[128] */
#define UDF_LVD_BLOCKSIZE       212
#define UDF_LVD_DOMAIN          216
#define UDF_LVD_FSD             248     /* long_ad */
#define UDF_LVD_MAPLEN          264
#define UDF_LVD_NMAPS           268
#define UDF_LVD_IMPL            272
#define UDF_LVD_IMPLUSE         304
#define UDF_LVD_INTEGRITY       432     /* extent_ad */
#define UDF_LVD_MAPS            440

/* Partition maps */
#define UDF_PM_TYPE1            1
#define UDF_PM_TYPE2            2
#define UDF_PM2_IDENT           4       /* regid */
#define UDF_PM2_VOLSEQ          36
#define UDF_PM2_PARTNUM         38
#define UDF_PM_SPAR_PACKET      40
#define UDF_PM_SPAR_NTABLES     42
#define UDF_PM_SPAR_TABLESIZE   44
#define UDF_PM_SPAR_TABLES      48
#define UDF_PM_META_FILE        40
#define UDF_PM_META_MIRROR      44
#define UDF_PM_META_BITMAP      48
#define UDF_PM_META_ALLOCUNIT   52
#define UDF_PM_META_ALIGNUNIT   56
#define UDF_PM_META_FLAGS       58

/* Primary volume descriptor */
#define UDF_PVD_VDSNUM          16
#define UDF_PVD_NUMBER          20
#define UDF_PVD_VOLID           24      /* dstring[32] */
#define UDF_PVD_VOLSEQ          56
#define UDF_PVD_MAXVOLSEQ       58
#define UDF_PVD_ILEVEL          60
#define UDF_PVD_MAXILEVEL       62
#define UDF_PVD_CSLIST          64
#define UDF_PVD_MAXCSLIST       68
#define UDF_PVD_VOLSETID        72      /* dstring[128] */
#define UDF_PVD_DESCCS          200
#define UDF_PVD_EXPLCS          264
#define UDF_PVD_ABSTRACT        328
#define UDF_PVD_COPYRIGHT       336
#define UDF_PVD_APPID           344
#define UDF_PVD_RECTIME         376
#define UDF_PVD_IMPL            388
#define UDF_PVD_IMPLUSE         420
#define UDF_PVD_PREDVDS         484
#define UDF_PVD_FLAGS           488

/* Logical volume integrity descriptor */
#define UDF_LVID_TIME           16
#define UDF_LVID_TYPE           28
#define UDF_LVID_NEXT           32      /* extent_ad */
#define UDF_LVID_UNIQUEID       40      /* logical volume header descriptor: u64 */
#define UDF_LVID_NPART          72
#define UDF_LVID_IMPLLEN        76
#define UDF_LVID_TABLES         80      /* free space table, then size table */
/* the implementation use (after the two tables) */
#define UDF_LVIU_IMPL           0       /* regid */
#define UDF_LVIU_FILES          32
#define UDF_LVIU_DIRS           36
#define UDF_LVIU_MINREAD        40
#define UDF_LVIU_MINWRITE       42
#define UDF_LVIU_MAXWRITE       44
#define UDF_LVIU_SIZE           46

/* File set descriptor */
#define UDF_FSD_TIME            16
#define UDF_FSD_ILEVEL          28
#define UDF_FSD_MAXILEVEL       30
#define UDF_FSD_CSLIST          32
#define UDF_FSD_MAXCSLIST       36
#define UDF_FSD_FSNUM           40
#define UDF_FSD_FSDNUM          44
#define UDF_FSD_LVICS           48
#define UDF_FSD_LVID            112     /* dstring[128] */
#define UDF_FSD_FSCS            240
#define UDF_FSD_FSID            304     /* dstring[32] */
#define UDF_FSD_COPYRIGHT       336
#define UDF_FSD_ABSTRACT        368
#define UDF_FSD_ROOT            400     /* long_ad */
#define UDF_FSD_DOMAIN          416
#define UDF_FSD_NEXT            448     /* long_ad */
#define UDF_FSD_SSDIR           464     /* long_ad: system stream directory (2.00+) */

/* ICB tag (at 16 in FE/EFE) */
#define UDF_ICBTAG              16
#define UDF_ICBTAG_PRIOR        0
#define UDF_ICBTAG_STRATEGY     4
#define UDF_ICBTAG_STRATPARAM   6
#define UDF_ICBTAG_MAXENTRIES   8
#define UDF_ICBTAG_FILETYPE     11
#define UDF_ICBTAG_PARENT       12      /* lb_addr */
#define UDF_ICBTAG_FLAGS        18
#define UDF_ICBTAG_SIZE         20

/* File entry */
#define UDF_FE_UID              36
#define UDF_FE_GID              40
#define UDF_FE_PERM             44
#define UDF_FE_LINKS            48
#define UDF_FE_RECFMT           50
#define UDF_FE_RECATTR          51
#define UDF_FE_RECLEN           52
#define UDF_FE_INFOLEN          56
#define UDF_FE_BLOCKS           64
#define UDF_FE_ATIME            72
#define UDF_FE_MTIME            84
#define UDF_FE_ATTRTIME         96
#define UDF_FE_CHECKPOINT       108
#define UDF_FE_EAICB            112
#define UDF_FE_IMPL             128
#define UDF_FE_UNIQUEID         160
#define UDF_FE_LEA              168
#define UDF_FE_LAD              172
#define UDF_FE_EA               176     /* the extended attributes, then the ADs */

/* Extended file entry */
#define UDF_EFE_UID             36
#define UDF_EFE_GID             40
#define UDF_EFE_PERM            44
#define UDF_EFE_LINKS           48
#define UDF_EFE_RECFMT          50
#define UDF_EFE_RECATTR         51
#define UDF_EFE_RECLEN          52
#define UDF_EFE_INFOLEN         56
#define UDF_EFE_OBJSIZE         64
#define UDF_EFE_BLOCKS          72
#define UDF_EFE_ATIME           80
#define UDF_EFE_MTIME           92
#define UDF_EFE_CTIME           104     /* creation */
#define UDF_EFE_ATTRTIME        116
#define UDF_EFE_CHECKPOINT      128
#define UDF_EFE_EAICB           136
#define UDF_EFE_STREAMICB       152
#define UDF_EFE_IMPL            168
#define UDF_EFE_UNIQUEID        200
#define UDF_EFE_LEA             208
#define UDF_EFE_LAD             212
#define UDF_EFE_EA              216

/* File identifier descriptor */
#define UDF_FID_VERSION         16
#define UDF_FID_CHAR            18
#define UDF_FID_LFI             19
#define UDF_FID_ICB             20      /* long_ad */
#define UDF_FID_LIU             36
#define UDF_FID_IU              38
#define UDF_FID_FIXED           38

/* Allocation extent descriptor */
#define UDF_AED_PREV            16
#define UDF_AED_LAD             20
#define UDF_AED_ADS             24

/* Space bitmap descriptor */
#define UDF_SBD_BITS            16
#define UDF_SBD_BYTES           20
#define UDF_SBD_MAP             24

/* Extended attributes */
#define UDF_EAHD_IMPL           16
#define UDF_EAHD_APP            20
#define UDF_EAHD_SIZE           24
#define UDF_EA_TYPE_IMPL        2048
#define UDF_EA_TYPE_APP         65536

/* Sparing table */
#define UDF_SPT_IDENT           16
#define UDF_SPT_COUNT           48
#define UDF_SPT_SEQ             52
#define UDF_SPT_MAP             56

/* VAT 2.00 header */
#define UDF_VAT_HDRLEN          0
#define UDF_VAT_IULEN           2
#define UDF_VAT_LVID            4       /* dstring[128] */
#define UDF_VAT_PREV            132
#define UDF_VAT_FILES           136
#define UDF_VAT_DIRS            140
#define UDF_VAT_MINREAD         144
#define UDF_VAT_MINWRITE        146
#define UDF_VAT_MAXWRITE        148
#define UDF_VAT_HDRSIZE         152

/* Path components of symbolic links (4/14.16.1) */
#define UDF_PC_SIZE             4
#define UDF_PC_ROOT_AGREED      1
#define UDF_PC_ROOT             2
#define UDF_PC_PARENT           3
#define UDF_PC_CURRENT          4
#define UDF_PC_NAME             5

/* Unique IDs: 1-15 are reserved; the root has 0 */
#define UDF_FIRST_UNIQUE_ID     16

/* UDF revisions */
#define UDF_REV_102             0x0102
#define UDF_REV_150             0x0150
#define UDF_REV_200             0x0200
#define UDF_REV_201             0x0201
#define UDF_REV_250             0x0250
#define UDF_REV_260             0x0260

/* Entity identifiers */
#define UDF_ID_COMPLIANT        "*OSTA UDF Compliant"
#define UDF_ID_LV_INFO          "*UDF LV Info"
#define UDF_ID_FREE_EA          "*UDF FreeEASpace"
#define UDF_ID_FREE_AEA         "*UDF FreeAppEASpace"
#define UDF_ID_VIRTUAL          "*UDF Virtual Partition"
#define UDF_ID_SPARABLE         "*UDF Sparable Partition"
#define UDF_ID_METADATA         "*UDF Metadata Partition"
#define UDF_ID_SPARING_TABLE    "*UDF Sparing Table"
#define UDF_ID_VAT150           "*UDF Virtual Alloc Tbl"
#define UDF_ID_NSR02            "+NSR02"
#define UDF_ID_NSR03            "+NSR03"
#define UDF_ID_IMPL             "*UDFNT"
#define UDF_CS0_INFO            "OSTA Compressed Unicode"

#endif
