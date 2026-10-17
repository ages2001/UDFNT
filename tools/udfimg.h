/* UDFNT - udfimg: the device the tools work on (see udfimg.c) */
#ifndef _UDFIMG_H_
#define _UDFIMG_H_

#ifdef _WIN32
#include <windows.h>
#endif

typedef struct {
#ifdef _WIN32
    HANDLE              Handle;
    int                 IsDrive;
#else
    int                 Fd;
#endif
    unsigned long long  Offset;     /* bytes: start of the file system */
    unsigned long long  Size;       /* bytes */
    int                 Slot;       /* MBR slot of the partition, -1 = none */
    int                 ReadOnly;
    unsigned char      *Bounce;     /* sector-aligned staging buffer */
    unsigned            Sector;     /* the device sector size */
} UDF_IMAGE;

/* Path: "E:" (Windows), \\.\PhysicalDriveN, an image file or a device.
   Slot: MBR partition 0..3, or -1 for "the first partition holding a UDF volume if
   the target holds a partition table, else the whole target". */
int  UimgOpen(UDF_IMAGE *I, const char *Path, int Slot, int ReadOnly);
void UimgClose(UDF_IMAGE *I);
int  UimgRead(UDF_IMAGE *I, unsigned long long Off, void *Buf, unsigned Len);
int  UimgWrite(UDF_IMAGE *I, unsigned long long Off, const void *Buf, unsigned Len);
/* Windows drives: lock + dismount before writing; set the partition type */
int  UimgLock(UDF_IMAGE *I);
void UimgUnlock(UDF_IMAGE *I);
int  UimgSetType(UDF_IMAGE *I, unsigned char Type);
/* boot: where the partition starts and the geometry; mark the partition active (disk images) */
void UimgGeometry(UDF_IMAGE *I, unsigned long *Start, unsigned long *Spt, unsigned long *Heads);
int  UimgSetActive(UDF_IMAGE *I);

#endif
