/*
 * UDFNT - private file system controls (driver and tools)
 *
 *   UDN_FSCTL_QUERY_VOLUME  any handle of the volume: UDN_VOLUME_INFO
 *   UDN_FSCTL_GET_POSIX     any file handle: UDN_POSIX_INFO (mode, owner,
 *                           links, inode number, flags)
 *   UDN_FSCTL_SET_POSIX     file handle opened with FILE_WRITE_ATTRIBUTES:
 *                           input UDN_POSIX_INFO, Valid says which fields
 *   UDN_FSCTL_READ_LINK     a symbolic link opened as itself (DELETE access
 *                           or FILE_OPEN_REPARSE_POINT): the target text
 *   UDN_FSCTL_CREATE_LINK   a directory handle: input UDN_LINK_REQUEST
 *                           makes a symbolic link Name -> Target in it
 *   UDN_FSCTL_COMMIT        any handle: commit the running transaction now
 */

#ifndef _UDNCTL_H_
#define _UDNCTL_H_

#define UDN_FSCTL_QUERY_VOLUME  CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 0x8B0, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define UDN_FSCTL_GET_POSIX     CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 0x8B1, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define UDN_FSCTL_SET_POSIX     CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 0x8B2, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define UDN_FSCTL_READ_LINK     CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 0x8B3, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define UDN_FSCTL_CREATE_LINK   CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 0x8B4, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define UDN_FSCTL_COMMIT        CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 0x8B5, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _UDN_VOLUME_INFO {
    ULONG       Version;            /* 1 */
    ULONG       BlockSize;          /* the logical block size */
    ULONGLONG   BlocksCount;        /* the volume */
    ULONGLONG   FreeBlocks;         /* in the partition file data goes to */
    ULONG       Revision;           /* UDF revision, e.g. 0x0250 */
    ULONG       Maps;               /* partition maps */
    ULONGLONG   DataBlocks;         /* the partition file data goes to */
    ULONG       Files;              /* as the integrity descriptor counts them */
    ULONG       Directories;
    ULONG       ReadOnly;           /* mounted read-only (asked or forced) */
    ULONG       ReadOnlyForced;     /* a feature or state UDFNT cannot write */
    ULONG       Commits;            /* transactions written */
    UCHAR       Uuid[16];
    char        Label[64];
} UDN_VOLUME_INFO, *PUDN_VOLUME_INFO;

#define UDN_POSIX_MODE          0x01
#define UDN_POSIX_UID           0x02
#define UDN_POSIX_GID           0x04

typedef struct _UDN_POSIX_INFO {
    ULONG       Valid;              /* SET: which fields to apply */
    ULONG       Mode;               /* type and permission bits (st_mode) */
    ULONG       Uid;
    ULONG       Gid;
    ULONG       Links;
    ULONG       Inode;
    ULONG       Flags;              /* ICB flags */
    ULONG       Generation;
} UDN_POSIX_INFO, *PUDN_POSIX_INFO;

typedef struct _UDN_LINK_REQUEST {
    USHORT      NameLength;         /* bytes of UTF-16 */
    USHORT      TargetLength;       /* bytes of the UTF-8 target that follows the name */
    WCHAR       Name[1];            /* then the target (UTF-8, not terminated) */
} UDN_LINK_REQUEST, *PUDN_LINK_REQUEST;

#endif
