/*
 * UDFNT - FCBs, CCBs, names, times, attributes and the glue to the engine
 *
 * A file has one FCB, found by its inode number, so every open of a file -
 * under any of its hard-link names - shares one set of section object
 * pointers. An FCB holds a reference on the directory it was opened in.
 *
 * Names: ext names are bytes, normally UTF-8. They map to NT names without
 * loss, both ways:
 *   - valid UTF-8 becomes the same characters (4-byte forms as surrogates);
 *   - a byte that is not valid UTF-8 becomes U+DC80..U+DCFF;
 *   - characters NT names cannot hold (0x01-0x1F " * : < > ? \ |, and a
 *     trailing dot or space) become U+F000 + the character (as Cygwin does).
 */

#include "udn.h"

/* ------------------------------------------------------------------ */
/* Times: ext keeps UTC seconds since 1970 (+ nanoseconds and epoch    */
/* bits in the inode's extra fields)                                   */
/* ------------------------------------------------------------------ */

LARGE_INTEGER
UdnToNtTime (
    ULONG Seconds,
    ULONG Extra
    )
{
    LARGE_INTEGER Time;
    Time.QuadPart = (LONGLONG)UfxGetTime(Seconds, Extra);
    return Time;
}

VOID
UdnFromNtTime (
    LARGE_INTEGER Time,
    PULONG Seconds,
    PULONG Extra
    )
{
    UfxSetTime(Seconds, Extra, (ULONGLONG)Time.QuadPart);
}

/* Is the 32-bit inode field at Offset covered by i_extra_isize? */
static BOOLEAN
UdnExtraField (
    PUDN_VCB Vcb,
    PUFX_INODE Inode,
    ULONG Offset
    )
{
    return (BOOLEAN)(Vcb->Vol.InodeSize > UFX_GOOD_OLD_INODE_SIZE &&
                     UFX_GOOD_OLD_INODE_SIZE + (ULONG)Inode->ExtraIsize >= Offset + 4);
}

/* ------------------------------------------------------------------ */
/* Names                                                               */
/* ------------------------------------------------------------------ */

static BOOLEAN
UdnNtSpecial (
    ULONG C
    )
{
    return (BOOLEAN)((C >= 0x01 && C <= 0x1F) || C == '"' || C == '*' || C == ':' || C == '<' ||
                     C == '>' || C == '?' || C == '\\' || C == '|');
}

BOOLEAN
UdnIsValidUnicodeName (
    PCUNICODE_STRING Name
    )
{
    USHORT Count = Name->Length / sizeof(WCHAR);
    USHORT i;
    WCHAR C;

    if (Count == 0 || Count > UDN_MAX_NAME || (Name->Length & 1) != 0) {
        return FALSE;
    }
    if ((Count == 1 && Name->Buffer[0] == L'.') ||
        (Count == 2 && Name->Buffer[0] == L'.' && Name->Buffer[1] == L'.')) {
        return FALSE;
    }
    for (i = 0; i < Count; i++) {
        C = Name->Buffer[i];
        if (C < 0x20 || C == L'"' || C == L'*' || C == L'/' || C == L':' || C == L'<' ||
            C == L'>' || C == L'?' || C == L'\\' || C == L'|') {
            return FALSE;
        }
    }
    return TRUE;
}

/*
 * NT name -> ext bytes. Fails for a name that would not come back the same
 * (a lone surrogate outside the escape range) or that is longer than 255
 * bytes once encoded.
 */
NTSTATUS
UdnUnicodeToExt (
    PCUNICODE_STRING Name,
    PUCHAR Out,
    PULONG OutLength
    )
{
    ULONG Count = Name->Length / sizeof(WCHAR), i, n = 0;
    const WCHAR *P = Name->Buffer;

    for (i = 0; i < Count; i++) {
        ULONG C = P[i];
        if (C >= 0xF001 && C <= 0xF07F && (UdnNtSpecial(C - 0xF000) ||
            ((C == 0xF02E || C == 0xF020) && i == Count - 1))) {
            C -= 0xF000;                               /* mapped NT-illegal character */
            if (n + 1 > UDN_MAX_NAME) return STATUS_OBJECT_NAME_INVALID;
            Out[n++] = (UCHAR)C;
            continue;
        }
        if (C >= 0xDC80 && C <= 0xDCFF) {                 /* escaped raw byte */
            if (n + 1 > UDN_MAX_NAME) return STATUS_OBJECT_NAME_INVALID;
            Out[n++] = (UCHAR)(C - 0xDC00);
            continue;
        }
        if (C >= 0xD800 && C <= 0xDBFF) {                 /* surrogate pair */
            ULONG L;
            if (i + 1 >= Count) return STATUS_OBJECT_NAME_INVALID;
            L = P[i + 1];
            if (L < 0xDC00 || L > 0xDFFF) return STATUS_OBJECT_NAME_INVALID;
            C = 0x10000 + ((C - 0xD800) << 10) + (L - 0xDC00);
            i++;
        } else if (C >= 0xDC00 && C <= 0xDFFF) {
            return STATUS_OBJECT_NAME_INVALID;
        }
        if (C == 0 || C == '/') return STATUS_OBJECT_NAME_INVALID;
        if (C < 0x80) {
            if (n + 1 > UDN_MAX_NAME) return STATUS_OBJECT_NAME_INVALID;
            Out[n++] = (UCHAR)C;
        } else if (C < 0x800) {
            if (n + 2 > UDN_MAX_NAME) return STATUS_OBJECT_NAME_INVALID;
            Out[n++] = (UCHAR)(0xC0 | (C >> 6));
            Out[n++] = (UCHAR)(0x80 | (C & 0x3F));
        } else if (C < 0x10000) {
            if (n + 3 > UDN_MAX_NAME) return STATUS_OBJECT_NAME_INVALID;
            Out[n++] = (UCHAR)(0xE0 | (C >> 12));
            Out[n++] = (UCHAR)(0x80 | ((C >> 6) & 0x3F));
            Out[n++] = (UCHAR)(0x80 | (C & 0x3F));
        } else {
            if (n + 4 > UDN_MAX_NAME) return STATUS_OBJECT_NAME_INVALID;
            Out[n++] = (UCHAR)(0xF0 | (C >> 18));
            Out[n++] = (UCHAR)(0x80 | ((C >> 12) & 0x3F));
            Out[n++] = (UCHAR)(0x80 | ((C >> 6) & 0x3F));
            Out[n++] = (UCHAR)(0x80 | (C & 0x3F));
        }
    }
    if (n == 0) return STATUS_OBJECT_NAME_INVALID;
    Out[n] = 0;
    *OutLength = n;
    return STATUS_SUCCESS;
}

/* One UTF-8 sequence at In[0..Left): its code point and length, or 0 */
static ULONG
UdnDecodeUtf8 (
    const UCHAR *In,
    ULONG Left,
    PULONG Cp
    )
{
    UCHAR B = In[0];
    ULONG C, Need, i, Min;
    if (B < 0x80) { *Cp = B; return 1; }
    if ((B & 0xE0) == 0xC0) { C = B & 0x1F; Need = 1; Min = 0x80; }
    else if ((B & 0xF0) == 0xE0) { C = B & 0x0F; Need = 2; Min = 0x800; }
    else if ((B & 0xF8) == 0xF0) { C = B & 0x07; Need = 3; Min = 0x10000; }
    else return 0;
    if (Need >= Left) return 0;
    for (i = 1; i <= Need; i++) {
        if ((In[i] & 0xC0) != 0x80) return 0;
        C = (C << 6) | (In[i] & 0x3F);
    }
    if (C < Min || C > 0x10FFFF || (C >= 0xD800 && C <= 0xDFFF)) return 0;
    /* a decoded character in our private escape ranges would not round-trip */
    if ((C >= 0xF001 && C <= 0xF07F) || (C >= 0xDC80 && C <= 0xDCFF)) return 0;
    *Cp = C;
    return Need + 1;
}

/* ext bytes -> NT name. Allocates Name->Buffer (paged pool) */
NTSTATUS
UdnExtToUnicode (
    const UCHAR *In,
    ULONG InLength,
    PUNICODE_STRING Name
    )
{
    ULONG i = 0, n = 0;
    PWSTR W;

    W = (PWSTR)ExAllocatePoolWithTag(PagedPool, (InLength + 1) * 2 * sizeof(WCHAR), UDN_TAG_NAME);
    if (W == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    while (i < InLength) {
        ULONG C, L = UdnDecodeUtf8(In + i, InLength - i, &C);
        if (L == 0) {
            W[n++] = (WCHAR)(0xDC00 | In[i]);           /* not UTF-8: escape the byte */
            i++;
            continue;
        }
        if (C < 0x80 && (UdnNtSpecial(C) || ((C == '.' || C == ' ') && i + 1 == InLength))) {
            C += 0xF000;
        }
        if (C >= 0x10000) {
            C -= 0x10000;
            W[n++] = (WCHAR)(0xD800 + (C >> 10));
            W[n++] = (WCHAR)(0xDC00 + (C & 0x3FF));
        } else {
            W[n++] = (WCHAR)C;
        }
        i += L;
    }
    Name->Buffer = W;
    Name->Length = (USHORT)(n * sizeof(WCHAR));
    Name->MaximumLength = (USHORT)((InLength + 1) * 2 * sizeof(WCHAR));
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Attributes                                                          */
/* ------------------------------------------------------------------ */

/* user.DOSATTRIB as Samba writes it in its plain form: "0x" + hex */
ULONG
UdnLoadDosAttributes (
    PUDN_VCB Vcb,
    ULONG Ino,
    PUFX_INODE Inode
    )
{
    UCHAR V[64];
    ULONG Len = 0, i, A = 0;

    if (!NT_SUCCESS(UfxGetXattr(&Vcb->Vol, Ino, Inode, UFX_XATTR_INDEX_USER, "DOSATTRIB", 9,
                                V, sizeof(V) - 1, &Len)) || Len < 3) {
        return 0;
    }
    if (V[0] != '0' || (V[1] != 'x' && V[1] != 'X')) {
        return 0;               /* Samba's binary forms: not ours to parse */
    }
    for (i = 2; i < Len && V[i]; i++) {
        UCHAR c = V[i];
        ULONG d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        A = (A << 4) | d;
    }
    return A & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE);
}

NTSTATUS
UdnStoreDosAttributes (
    PUDN_VCB Vcb,
    PUDN_FCB Fcb,
    ULONG Attributes
    )
{
    static const char Hex[] = "0123456789abcdef";
    char V[8];
    UFX_STATUS S;

    Attributes &= FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE;
    if (Attributes == Fcb->DosAttributes) {
        return STATUS_SUCCESS;
    }
    if (Attributes == 0) {
        S = UfxSetXattr(&Vcb->Vol, Fcb->Ino, &Fcb->Inode, UFX_XATTR_INDEX_USER, "DOSATTRIB", 9, NULL, 0);
    } else {
        V[0] = '0'; V[1] = 'x';
        V[2] = Hex[(Attributes >> 4) & 0xF];
        V[3] = Hex[Attributes & 0xF];
        S = UfxSetXattr(&Vcb->Vol, Fcb->Ino, &Fcb->Inode, UFX_XATTR_INDEX_USER, "DOSATTRIB", 9,
                        (const UCHAR *)V, 4);
    }
    if (S == UFX_NOT_SUPPORTED) {
        S = UFX_OK;             /* revision 0 / ea_inode: keep them in memory only */
    }
    if (NT_SUCCESS(S)) {
        Fcb->DosAttributes = Attributes;
    }
    return S;
}

ULONG
UdnComputeAttributes (
    PUDN_VCB Vcb,
    PUFX_INODE Inode,
    const UCHAR *Name,
    ULONG NameLength,
    ULONG DosAttributes
    )
{
    ULONG A = DosAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE);
    USHORT Type = Inode->Mode & UFX_S_IFMT;

    if (Type == UFX_S_IFDIR) {
        A |= FILE_ATTRIBUTE_DIRECTORY;
    } else if (!(Inode->Mode & 0200)) {
        A |= FILE_ATTRIBUTE_READONLY;          /* no owner write permission */
    }
    if (Type != UFX_S_IFDIR && Type != UFX_S_IFREG && Type != UFX_S_IFLNK) {
        A |= FILE_ATTRIBUTE_SYSTEM;            /* device, fifo, socket */
    }
    if (Inode->Flags & UFX_IF_IMMUTABLE) {
        A |= FILE_ATTRIBUTE_READONLY;
    }
    if (UdnData.HideDotFiles && NameLength > 1 && Name[0] == '.' &&
        !(NameLength == 2 && Name[1] == '.')) {
        A |= FILE_ATTRIBUTE_HIDDEN;
    }
    if (Vcb->VcbState & VCB_STATE_READ_ONLY) {
        /* nothing on a read-only volume can be written */
    }
    if (A == 0) {
        A = FILE_ATTRIBUTE_NORMAL;
    }
    return A;
}

/* ------------------------------------------------------------------ */
/* FCBs                                                                */
/* ------------------------------------------------------------------ */

static PUDN_FCB
UdnAllocateFcb (
    PUDN_VCB Vcb,
    CSHORT NodeType
    )
{
    PUDN_FCB Fcb;

    Fcb = (PUDN_FCB)ExAllocatePoolWithTag(NonPagedPool, sizeof(UDN_FCB), UDN_TAG_FCB);
    if (Fcb == NULL) {
        return NULL;
    }

    RtlZeroMemory(Fcb, sizeof(UDN_FCB));

    Fcb->Header.NodeTypeCode = NodeType;
    Fcb->Header.NodeByteSize = (CSHORT)sizeof(UDN_FCB);
    Fcb->Header.IsFastIoPossible = FastIoIsNotPossible;

    ExInitializeResourceLite(&Fcb->Resource);
    ExInitializeResourceLite(&Fcb->PagingIoResource);

    Fcb->Header.Resource = &Fcb->Resource;
    Fcb->Header.PagingIoResource = &Fcb->PagingIoResource;

    FsRtlInitializeFileLock(&Fcb->FileLock, NULL, NULL);

    Fcb->Vcb = Vcb;
    InitializeListHead(&Fcb->FcbLinks);

    return Fcb;
}

VOID
UdnDeleteFcb (
    PUDN_FCB Fcb
    )
{
    RemoveEntryList(&Fcb->FcbLinks);

    FsRtlUninitializeFileLock(&Fcb->FileLock);

    ExDeleteResourceLite(&Fcb->Resource);
    ExDeleteResourceLite(&Fcb->PagingIoResource);

    if (Fcb->Name.Buffer != NULL) {
        ExFreePool(Fcb->Name.Buffer);
    }
    if (Fcb->FullName.Buffer != NULL) {
        ExFreePool(Fcb->FullName.Buffer);
    }
    if (Fcb->Runs != NULL) {
        ExFreePool(Fcb->Runs);
    }
    UdnIndexFree(Fcb);

    ExFreePool(Fcb);
}

PUDN_FCB
UdnCreateVolumeFcb (
    PUDN_VCB Vcb
    )
{
    PUDN_FCB Fcb;

    Fcb = UdnAllocateFcb(Vcb, UDN_NTC_VFCB);
    if (Fcb == NULL) {
        return NULL;
    }

    Fcb->Header.AllocationSize.QuadPart = Vcb->PartitionBytes;
    Fcb->Header.FileSize.QuadPart = Vcb->PartitionBytes;
    Fcb->Header.ValidDataLength.QuadPart = Vcb->PartitionBytes;

    return Fcb;
}

/* The FCB's sizes, times and attributes from its inode copy */
VOID
UdnInodeToFcb (
    PUDN_VCB Vcb,
    PUDN_FCB Fcb
    )
{
    PUFX_INODE I = &Fcb->Inode;
    LONGLONG Size = (LONGLONG)UfxInodeSize(&Vcb->Vol, I);

    if (UdnIsDcb(Fcb)) {
        Size = 0;
    }
    Fcb->Header.FileSize.QuadPart = Size;
    Fcb->Header.ValidDataLength.QuadPart = Size;
    Fcb->Header.AllocationSize.QuadPart = UdnRoundUp(Size, Vcb->Vol.BlockSize);

    Fcb->LastWriteTime = UdnToNtTime(I->Mtime, UdnExtraField(Vcb, I, 0x88) ? I->MtimeExtra : 0);
    Fcb->LastAccessTime = UdnToNtTime(I->Atime, UdnExtraField(Vcb, I, 0x8C) ? I->AtimeExtra : 0);
    Fcb->ChangeTime = UdnToNtTime(I->Ctime, UdnExtraField(Vcb, I, 0x84) ? I->CtimeExtra : 0);
    if (UdnExtraField(Vcb, I, 0x90) && I->Crtime != 0) {
        Fcb->CreationTime = UdnToNtTime(I->Crtime, UdnExtraField(Vcb, I, 0x94) ? I->CrtimeExtra : 0);
    } else {
        /* no birth time on disk: the oldest time we have */
        Fcb->CreationTime = Fcb->LastWriteTime.QuadPart < Fcb->ChangeTime.QuadPart ?
                            Fcb->LastWriteTime : Fcb->ChangeTime;
    }
    Fcb->Attributes = UdnComputeAttributes(Vcb, I, Fcb->UfxName, Fcb->UfxNameLength, Fcb->DosAttributes);
}

/* Reads the inode into the FCB (engine not held by the caller) */
NTSTATUS
UdnLoadInode (
    PUDN_VCB Vcb,
    PUDN_FCB Fcb
    )
{
    NTSTATUS Status;

    UdnEngineEnter(Vcb);
    Status = UfxReadInode(&Vcb->Vol, Fcb->Ino, &Fcb->Inode);
    if (NT_SUCCESS(Status)) {
        Fcb->DosAttributes = UdnLoadDosAttributes(Vcb, Fcb->Ino, &Fcb->Inode);
    }
    Status = UdnEngineLeave(Vcb, Status);
    if (NT_SUCCESS(Status)) {
        if (Fcb->Inode.LinksCount == 0 && Fcb->Ino != UFX_ROOT_INO) {
            return STATUS_FILE_CORRUPT_ERROR;     /* a deleted inode under a live name */
        }
        UdnInodeToFcb(Vcb, Fcb);
    }
    return Status;
}

PUDN_FCB
UdnCreateRootDcb (
    PUDN_VCB Vcb
    )
{
    PUDN_FCB Dcb;

    Dcb = UdnAllocateFcb(Vcb, UDN_NTC_DCB);
    if (Dcb == NULL) {
        return NULL;
    }

    Dcb->FcbState = FCB_STATE_ROOT;
    Dcb->Ino = UFX_ROOT_INO;
    if (!NT_SUCCESS(UdnLoadInode(Vcb, Dcb))) {
        UdnDeleteFcb(Dcb);
        return NULL;
    }
    Dcb->Attributes = FILE_ATTRIBUTE_DIRECTORY;

    InsertTailList(&Vcb->FcbList, &Dcb->FcbLinks);
    return Dcb;
}

NTSTATUS
UdnCreateFcb (
    PUDN_VCB Vcb,
    PUDN_FCB ParentDcb,
    PUFX_ENTRY Entry,
    BOOLEAN AsLink,
    PUDN_FCB *NewFcb
    )
{
    PUDN_FCB Fcb;
    UFX_INODE Inode;
    BOOLEAN IsDirectory;
    NTSTATUS Status;

    *NewFcb = NULL;

    UdnEngineEnter(Vcb);
    Status = UdnEngineLeave(Vcb, UfxReadInode(&Vcb->Vol, Entry->Inode, &Inode));
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    IsDirectory = (BOOLEAN)((Inode.Mode & UFX_S_IFMT) == UFX_S_IFDIR);

    Fcb = UdnAllocateFcb(Vcb, (CSHORT)(IsDirectory ? UDN_NTC_DCB : UDN_NTC_FCB));
    if (Fcb == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = UdnExtToUnicode((const UCHAR *)Entry->Name, Entry->NameLength, &Fcb->Name);
    if (!NT_SUCCESS(Status)) {
        UdnDeleteFcb(Fcb);
        return Status;
    }
    Fcb->UfxNameLength = Entry->NameLength;
    RtlCopyMemory(Fcb->UfxName, Entry->Name, Entry->NameLength);
    Fcb->UfxName[Entry->NameLength] = 0;

    Fcb->ParentDcb = ParentDcb;
    Fcb->Ino = Entry->Inode;
    if (AsLink) {
        Fcb->FcbState |= FCB_STATE_SYMLINK;
    }

    Status = UdnLoadInode(Vcb, Fcb);
    if (!NT_SUCCESS(Status)) {
        UdnDeleteFcb(Fcb);
        return Status;
    }

    InsertTailList(&Vcb->FcbList, &Fcb->FcbLinks);
    ParentDcb->RefCount++;

    *NewFcb = Fcb;
    return STATUS_SUCCESS;
}

VOID
UdnDereferenceFcb (
    PUDN_VCB Vcb,
    PUDN_FCB Fcb
    )
{
    PUDN_FCB Parent;

    while (Fcb != NULL) {

        Fcb->RefCount--;

        if (Fcb->RefCount != 0 || Fcb == Vcb->RootDcb || Fcb == Vcb->VolumeFcb) {
            break;
        }

        Parent = Fcb->ParentDcb;
        UdnDeleteFcb(Fcb);
        Fcb = Parent;
    }
}

/* The live FCB of an inode; a symbolic link opened as itself has its own */
PUDN_FCB
UdnFindFcbByIno (
    PUDN_VCB Vcb,
    ULONG Ino,
    BOOLEAN AsLink
    )
{
    PLIST_ENTRY Entry;
    PUDN_FCB Fcb;

    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {
        Fcb = CONTAINING_RECORD(Entry, UDN_FCB, FcbLinks);
        if (Fcb->Ino == Ino && !(Fcb->FcbState & FCB_STATE_DELETED) &&
            (BOOLEAN)((Fcb->FcbState & FCB_STATE_SYMLINK) != 0) == AsLink) {
            return Fcb;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* CCBs                                                                */
/* ------------------------------------------------------------------ */

PUDN_CCB
UdnCreateCcb (
    VOID
    )
{
    PUDN_CCB Ccb;

    Ccb = (PUDN_CCB)ExAllocatePoolWithTag(PagedPool, sizeof(UDN_CCB), UDN_TAG_CCB);
    if (Ccb == NULL) {
        return NULL;
    }

    RtlZeroMemory(Ccb, sizeof(UDN_CCB));
    Ccb->NodeTypeCode = UDN_NTC_CCB;
    Ccb->NodeByteSize = (CSHORT)sizeof(UDN_CCB);

    return Ccb;
}

VOID
UdnDeleteCcb (
    PUDN_CCB Ccb
    )
{
    if (Ccb->Pattern.Buffer != NULL) {
        ExFreePool(Ccb->Pattern.Buffer);
    }
    ExFreePool(Ccb);
}

/* ------------------------------------------------------------------ */
/* Full names                                                          */
/* ------------------------------------------------------------------ */

NTSTATUS
UdnBuildFullName (
    PUDN_FCB Fcb
    )
{
    PUDN_FCB Walk;
    ULONG Length = 0;
    PWSTR Buffer;
    PWSTR Write;

    if (Fcb->FullName.Buffer != NULL) {
        return STATUS_SUCCESS;
    }

    if (Fcb->ParentDcb == NULL) {
        Length = sizeof(WCHAR);
    } else {
        for (Walk = Fcb; Walk->ParentDcb != NULL; Walk = Walk->ParentDcb) {
            Length += sizeof(WCHAR) + Walk->Name.Length;
        }
    }

    if (Length > 0xFFFE) {
        return STATUS_NAME_TOO_LONG;
    }

    Buffer = (PWSTR)ExAllocatePoolWithTag(PagedPool, Length, UDN_TAG_NAME);
    if (Buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (Fcb->ParentDcb == NULL) {
        Buffer[0] = L'\\';
    } else {
        Write = (PWSTR)((PUCHAR)Buffer + Length);
        for (Walk = Fcb; Walk->ParentDcb != NULL; Walk = Walk->ParentDcb) {
            Write = (PWSTR)((PUCHAR)Write - Walk->Name.Length);
            RtlCopyMemory(Write, Walk->Name.Buffer, Walk->Name.Length);
            Write--;
            *Write = L'\\';
        }
    }

    Fcb->FullName.Buffer = Buffer;
    Fcb->FullName.Length = (USHORT)Length;
    Fcb->FullName.MaximumLength = (USHORT)Length;

    return STATUS_SUCCESS;
}

BOOLEAN
UdnIsAncestor (
    PUDN_FCB Ancestor,
    PUDN_FCB Fcb
    )
{
    for (; Fcb != NULL; Fcb = Fcb->ParentDcb) {
        if (Fcb == Ancestor) {
            return TRUE;
        }
    }
    return FALSE;
}

VOID
UdnForgetNames (
    PUDN_VCB Vcb,
    PUDN_FCB Fcb
    )
{
    PLIST_ENTRY Entry;
    PUDN_FCB Walk;

    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {
        Walk = CONTAINING_RECORD(Entry, UDN_FCB, FcbLinks);
        if (Walk->FullName.Buffer != NULL && UdnIsAncestor(Fcb, Walk)) {
            ExFreePool(Walk->FullName.Buffer);
            RtlZeroMemory(&Walk->FullName, sizeof(UNICODE_STRING));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Inode updates and allocation                                        */
/* ------------------------------------------------------------------ */

VOID
UdnForgetMapping (
    PUDN_FCB Fcb
    )
{
    Fcb->MapRun = 0;
}

/* Writes the FCB's size and times into its inode (engine not held) */
NTSTATUS
UdnUpdateInode (
    PUDN_VCB Vcb,
    PUDN_FCB Fcb
    )
{
    PUFX_INODE I = &Fcb->Inode;
    NTSTATUS Status;

    if (Fcb->FcbState & FCB_STATE_DELETED) {
        Fcb->FcbState &= ~FCB_STATE_INODE_DIRTY;
        return STATUS_SUCCESS;
    }
    if (Vcb->VcbState & (VCB_STATE_READ_ONLY | VCB_STATE_DISMOUNTED)) {
        return STATUS_SUCCESS;
    }

    UdnEngineEnter(Vcb);
    if (UdnIsFcb(Fcb) && !(Fcb->FcbState & FCB_STATE_SYMLINK)) {
        UfxInodeSetSize(&Vcb->Vol, I, (ULONGLONG)Fcb->Header.FileSize.QuadPart);
    }
    UdnFromNtTime(Fcb->LastWriteTime, &I->Mtime, UdnExtraField(Vcb, I, 0x88) ? &I->MtimeExtra : NULL);
    UdnFromNtTime(Fcb->LastAccessTime, &I->Atime, UdnExtraField(Vcb, I, 0x8C) ? &I->AtimeExtra : NULL);
    UdnFromNtTime(Fcb->ChangeTime, &I->Ctime, UdnExtraField(Vcb, I, 0x84) ? &I->CtimeExtra : NULL);
    if (UdnExtraField(Vcb, I, 0x90)) {
        UdnFromNtTime(Fcb->CreationTime, &I->Crtime, UdnExtraField(Vcb, I, 0x94) ? &I->CrtimeExtra : NULL);
    }
    Status = UfxWriteInode(&Vcb->Vol, Fcb->Ino, I);
    Status = UdnEngineLeave(Vcb, Status);

    if (NT_SUCCESS(Status)) {
        Fcb->FcbState &= ~FCB_STATE_INODE_DIRTY;
    }
    return Status;
}

/*
 * The allocation of a file: btrfs files are sparse, so growing only moves
 * the reported size (space comes on first write); shrinking frees the
 * extents past the new end. Called with the file exclusive.
 */
NTSTATUS
UdnSetAllocation (
    PUDN_VCB Vcb,
    PUDN_FCB Fcb,
    ULONGLONG Bytes
    )
{
    ULONGLONG Blocks = (Bytes + Vcb->Vol.BlockSize - 1) >> Vcb->Vol.BlockShift;
    ULONGLONG Have = (ULONGLONG)Fcb->Header.AllocationSize.QuadPart >> Vcb->Vol.BlockShift;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Blocks < Have) {
        (VOID)ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
        UdnEngineEnter(Vcb);
        Status = UfxTruncateBlocks(&Vcb->Vol, Fcb->Ino, &Fcb->Inode, Blocks);
        if (NT_SUCCESS(Status)) {
            Status = UfxWriteInode(&Vcb->Vol, Fcb->Ino, &Fcb->Inode);
        }
        UdnForgetMapping(Fcb);          /* under the engine lock, like its readers */
        Status = UdnEngineLeave(Vcb, Status);
        UdnRelease(&Fcb->PagingIoResource);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }
    Fcb->Header.AllocationSize.QuadPart = (LONGLONG)(Blocks << Vcb->Vol.BlockShift);
    return Status;
}

/*
 * A paging file: every block allocated, and the map kept in nonpaged pool so
 * that paging I/O never enters the engine (which allocates paged memory and
 * takes locks a page fault could be waiting on).
 */
NTSTATUS
UdnLoadRuns (
    PUDN_VCB Vcb,
    PUDN_FCB Fcb
    )
{
    ULONGLONG Blocks = (ULONGLONG)(Fcb->Header.AllocationSize.QuadPart + Vcb->Vol.BlockSize - 1) >> Vcb->Vol.BlockShift;
    ULONGLONG Blk;
    ULONG Count = 0, Cap = 64;
    PUDN_RUN Runs, Bigger;
    NTSTATUS Status = STATUS_SUCCESS;

    Runs = (PUDN_RUN)ExAllocatePoolWithTag(NonPagedPool, Cap * sizeof(UDN_RUN), UDN_TAG_RUNS);
    if (Runs == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    UdnEngineEnter(Vcb);
    /* the whole file gets its space first (in as few extents as there is room for) */
    if (Fcb->Inode.Flags & (UFX_IF_INLINE_DATA | UFX_IF_NEEDS_PREP)) {
        Status = UfxUninline(&Vcb->Vol, Fcb->Ino, &Fcb->Inode);
    }
    if (NT_SUCCESS(Status) && Blocks != 0) {
        Status = UfxAllocateRange(&Vcb->Vol, Fcb->Ino, &Fcb->Inode, 0, Blocks);
    }
    for (Blk = 0; Blk < Blocks && NT_SUCCESS(Status); ) {
        ULONGLONG Disk;
        ULONG Run;
        Status = UfxMapFileBlock(&Vcb->Vol, Fcb->Ino, &Fcb->Inode, Blk, &Disk, &Run);
        if (!NT_SUCCESS(Status)) break;
        if (Disk == 0) {
            BOOLEAN Fresh;
            Status = UfxGetFileBlock(&Vcb->Vol, Fcb->Ino, &Fcb->Inode, Blk, &Disk, &Fresh);
            if (!NT_SUCCESS(Status)) break;
            Run = 1;
        }
        if (Run > Blocks - Blk) Run = (ULONG)(Blocks - Blk);
        if (Count && Runs[Count - 1].DiskBlock + Runs[Count - 1].Length == Disk &&
            Runs[Count - 1].FileBlock + Runs[Count - 1].Length == Blk) {
            Runs[Count - 1].Length += Run;
        } else {
            if (Count == Cap) {
                Bigger = (PUDN_RUN)ExAllocatePoolWithTag(NonPagedPool, Cap * 2 * sizeof(UDN_RUN), UDN_TAG_RUNS);
                if (Bigger == NULL) { Status = STATUS_INSUFFICIENT_RESOURCES; break; }
                RtlCopyMemory(Bigger, Runs, Count * sizeof(UDN_RUN));
                ExFreePool(Runs);
                Runs = Bigger;
                Cap *= 2;
            }
            Runs[Count].FileBlock = Blk;
            Runs[Count].DiskBlock = Disk;
            Runs[Count].Length = Run;
            Count++;
        }
        Blk += Run;
    }
    if (NT_SUCCESS(Status)) {
        Status = UfxWriteInode(&Vcb->Vol, Fcb->Ino, &Fcb->Inode);
    }
    Status = UdnEngineLeave(Vcb, Status);

    if (!NT_SUCCESS(Status)) {
        ExFreePool(Runs);
        return Status;
    }
    (VOID)ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
    if (Fcb->Runs != NULL) {
        ExFreePool(Fcb->Runs);
    }
    Fcb->Runs = Runs;
    Fcb->RunCount = Count;
    UdnRelease(&Fcb->PagingIoResource);
    return STATUS_SUCCESS;
}
