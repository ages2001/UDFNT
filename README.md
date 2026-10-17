# UDFNT - UDF File System Driver for Windows NT

UDFNT is a from-scratch driver for the **Universal Disk Format** - every
revision from 1.02 to 2.60 - on Windows NT 3.1, 3.5, 3.51, 4.0, 2000, XP and
XP x64. It mounts UDF on hard disks, removable disks and CD / DVD / BD drives,
read/write where the medium allows it, formats and checks UDF volumes, and can
**start Windows NT from a UDF volume**.

It is built like its siblings [EXTNT](https://github.com/ages2001/EXTNT),
[BTRFSNT](https://github.com/ages2001/BTRFSNT),
[NTFS3NT](https://github.com/ages2001/NTFS3NT),
[FATNT](https://github.com/ages2001/FATNT) and
[HPFSNT](https://github.com/ages2001/HPFSNT): one kernel-free engine that
understands the on-disk format - built into the driver, the tools and a Linux
test harness - with an NT driver layer on top.

## At a glance

| | |
|---|---|
| **Version** | 1.0 |
| **File system** | UDF 1.02, 1.50, 2.00, 2.01, 2.50, 2.60 |
| **Media** | hard and removable disks (512 - 4096-byte sectors), CD / DVD / BD drives, ISO / UDF bridge discs |
| **Windows** | NT 3.1, 3.5, 3.51, 4.0, 2000, XP (x86); XP x64 / Server 2003 x64 (amd64) |
| **Access** | read/write (write-once media read-only) |
| **Paging file** | yes |
| **Starting Windows from it** | yes, NT 3.1 - XP x64 |
| **Tools** | `udfinst`, `udffmt`, `udfchk`, `udfboot` |

## Tested

On Windows NT 3.1, 3.51, 4.0, 2000 and XP x64 under QEMU: copying hundreds of
files in and out, renaming and deleting, Explorer, a paging file on UDF under
memory pressure, formatting from Windows, DVD and ISO / UDF bridge discs in a
CD drive. Every one of these systems started from a UDF volume (revisions
1.02, 1.50, 2.01 and 2.50), was used, shut down and started again.

After every run `udfchk` finds the volume clean and Linux 6.8 reads what
Windows wrote byte for byte - and the other way round. Windows XP's own UDF
reader reads volumes UDFNT formatted.

Volumes formatted by Windows 7 - 11 (`format /FS:UDF /R:2.50` or `/R:2.60`)
are read and written too: their metadata bitmap kept inside its file entry,
the metadata mirror sharing the metadata file's blocks. `udfchk /F` repairs
what an interrupted Windows 11 2.60 format leaves ("Format failed").

## Features

**The engine** (`src/udf*.c`)

- Volume recognition, anchors, the main and reserve volume descriptor
  sequences.
- The logical volume integrity descriptor: open while the volume is in use,
  closed with free space, file and directory counts at every commit.
- Space bitmaps; physical, sparable, virtual (VAT 1.50 / 2.00) and metadata
  partitions (2.50 and later: the metadata file, its mirror and bitmap file,
  grown as needed).
- File entries and extended file entries; short, long and embedded allocation
  descriptors with allocation extent chains; file identifiers; OSTA
  compressed Unicode.
- Block sizes from 512 bytes to 64 KB, devices with 512 - 4096-byte sectors.

**Files**

- Create, write, truncate, delete, rename, hard links, symbolic links,
  timestamps (creation time with extended file entries), sparse and embedded
  files.
- The hidden, system and archive attributes, as UDF keeps them.
- Named streams recorded by Linux or other systems are kept, and freed with
  their file.

**The driver**

- Cache Manager integration (cached reads and writes, fast I/O, memory-mapped
  files and executables), paging I/O and a paging file on UDF.
- Change notification (see *Limitations*), byte-range locks, the volume label
  (`label X:`), lock and dismount for the tools.
- "Safely Remove Hardware" on Windows 2000 and XP: the volume is written out
  and let go, or the removal is refused while a file on it is open; a stick
  pulled without it is let go as well.
- Drive letters for UDF partitions of any type.
- Discs in CD / DVD / BD drives are mounted too - read-only unless the drive
  says it can write. Windows 2000 and XP's own `udfs.sys` would take UDF
  volumes first, so `udfinst` turns it off in every control set, Last Known
  Good too (and `udfinst /U` gives it back).
- One i386 binary for NT 3.1 - XP: every import is checked against NT 3.1's
  kernel at build time.

## Installing

`bin\i386` holds the driver and tools for Windows NT 3.1 - XP (32-bit),
`bin\amd64` the same for XP x64 / Server 2003 x64. Copy `udfnt.sys` and
`udfinst.exe` of the matching architecture to one folder and run, from an
administrator account:

```
udfinst              install, started with the system (drive letters at boot,
                     paging file possible)
udfinst /BOOT        boot start, for starting Windows from a UDF volume
udfinst /READONLY    mount UDF volumes read-only
udfinst /NOLETTERS   no drive letters for UDF partitions of other types
udfinst /NOOPTICAL   leave CD / DVD / BD drives alone
udfinst /CASE        case-sensitive names
udfinst /HIDEDOT     hide ".names"
udfinst /UID:n /GID:n   owner of new files
udfinst /U           uninstall, and give Windows' own UDF driver back
```

A restart applies the change. On 64-bit Windows run the amd64 `udfinst`; the
32-bit one refuses to run there.

## Settings

Under `HKLM\SYSTEM\CurrentControlSet\Services\Udfnt` (all `REG_DWORD`;
`udfinst` sets them):

| Value | Default | Meaning |
|---|---|---|
| `EnableWriteSupport` | 1 | 0 mounts every UDF volume read-only |
| `MountOptical` | 1 | 0 leaves discs in CD / DVD / BD drives to other drivers |
| `HideDotFiles` | 0 | 1 gives names starting with `.` the hidden attribute |
| `FollowSymlinks` | 1 | opening a symbolic link opens its target |
| `CaseSensitive` | 0 | exact-case lookups only |
| `DefaultUid` / `DefaultGid` | 0 | owner of new files |
| `FileMode` / `DirMode` | 0644 / 0755 | permissions of new files and directories |
| `AssignDriveLetters` | 1 | give UDF partitions of any type drive letters |

## Tools

```
udffmt  X: [/R:1.02|1.50|2.00|2.01|2.50|2.60] [/B:512..64K] [/L:label]
           [/META|/NOMETA] [/SPARE] [/FE] [/BOOT] [/T:xx] [/Y] [/Q]
udfchk  X: [/F] [/V] [/Q]
udfboot X: [/S:windir] [/T:xx] [/Q]
```

Every tool also takes a partition of a disk (`\\.\PhysicalDrive1 /P:2 ...`) or
an image file, on Windows and on Linux.

- **udffmt** formats a partition, a whole disk or an image: any revision (2.01
  by default), any block size (by default the sector size), a metadata
  partition (the default for 2.50 and 2.60) or a sparable one for rewritable
  discs, file entries instead of extended ones (`/FE`), a label of up to 126
  Latin-1 or 63 other characters, and a boot area (`/BOOT`). The result is
  read back through the engine; the partition type becomes `07h` unless `/T:`
  says otherwise.
- **udfchk** checks the descriptors, the integrity descriptor, the partitions
  and the metadata mirror, every file entry and directory, link counts and the
  space bitmaps. `/F` repairs the space bitmaps, link counts and the integrity
  descriptor. Exit codes: 0 clean, 1 repaired, 2 problems left, 8 could not
  check.
- **udfboot** makes a UDF volume start Windows NT (below).

## Starting Windows NT from UDF

NTLDR reads only FAT and NTFS, so the files it needs also get a **FAT16 view**
kept inside the UDF volume:

- the UDFNT boot loader sits in the 32 KB at the start that UDF leaves unused;
- the FAT and its directories are in the hidden file `UDFBOOT.SYS`;
- the FAT's clusters (4 KB, or one block if blocks are larger) **are** the
  UDF blocks of the files themselves, so the view always shows their current
  contents.

Boot files lying outside the 65518 clusters the FAT reaches, or not on the
cluster grid, are moved; UDFNT keeps that window for the boot files and brings
the view up to date at every shutdown.

1. `udffmt X: /BOOT` (any revision; 512-byte sectors, no sparable partition).
2. Copy NTLDR, NTDETECT.COM, BOOT.INI and the Windows tree onto it.
3. Install UDFNT into that Windows as a boot driver: `udfinst /BOOT` on it
   before copying, or the `Udfnt` service with Start 0, ImagePath
   `System32\drivers\udfnt.sys`, in its own group "UDFNT" appended to
   `ServiceGroupOrder`.
4. `udfboot X:` - writes the boot loader and the view; on disk images it also
   marks the partition active.

Run `udfboot` again after NTLDR, the kernel, a HAL or a boot driver was changed
by a system without UDFNT. Linux mounts the volume with `mount -t udf`.

## Limitations

- Mounted read-only: write-once media (VAT), free space tables, unknown
  domains and revisions newer than 2.60.
- The universal i386 binary holds change notification pending and completes
  it at cleanup (NT 3.1 has no `FsRtlNotifyFull` package), so File Manager and
  Explorer refresh on demand but never spin; the amd64 build delivers live
  notification.
- POSIX permissions are stored and shown, not enforced; NT applies its own
  security.
- Opportunistic locks are not granted.

## Building

On Linux with `gcc-mingw-w64-i686` / `gcc-mingw-w64-x86-64` (which ship the
DDK headers), `binutils` and `python3` - no Visual C++ or WDK:

```
sh build/build_cross.sh  OUTDIR     # i386: udfnt.sys, udfinst.exe, udffmt.exe, udfboot.exe, udfchk.exe
sh build/build_amd64.sh  OUTDIR     # amd64: the same for XP x64 / Server 2003 x64
bash test/buildlinux.sh             # Linux: udftool (engine test tool), udffmt, udfboot, udfchk
bash test/boottest.sh               # the boot view on many revisions and block sizes
```

Ready binaries are in `bin\i386` and `bin\amd64`; the i386 programs run on
NT 3.1 and later.

## Source layout

| Path | What |
|---|---|
| `src/udf.h`, `src/udfsup.c`, `udfvol.c` | on-disk structures, descriptors, partitions, volume |
| `src/udfino.c`, `udfdir.c`, `udfbuf.c` | file entries, directories, block cache |
| `src/udn*.c` | the NT driver: IRP handlers, Cache Manager, drive letters |
| `boot/udfboot.S` | the boot loader |
| `fmt/`, `chk/`, `inst/`, `tools/` | the tools and the Linux test tool |

## License

GNU General Public License v3.0, see [LICENSE](LICENSE).
