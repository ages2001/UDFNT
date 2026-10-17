/*
 * UDFNT - udftool: the UDF engine on Linux (or Windows), for tests.
 *
 *   udftool IMAGE info
 *   udftool IMAGE ls [PATH]          udftool IMAGE lsr [PATH]
 *   udftool IMAGE cat PATH           udftool IMAGE get PATH LOCAL
 *   udftool IMAGE put LOCAL PATH     udftool IMAGE putr LOCALDIR PATH
 *   udftool IMAGE getr PATH LOCALDIR
 *   udftool IMAGE mkdir PATH         udftool IMAGE rm PATH
 *   udftool IMAGE rmr PATH           udftool IMAGE mv OLD NEW
 *   udftool IMAGE ln OLD NEW         udftool IMAGE symlink TARGET PATH
 *   udftool IMAGE readlink PATH      udftool IMAGE label NEWLABEL
 *   udftool IMAGE truncate PATH SIZE udftool IMAGE write PATH OFFSET LOCAL
 *   udftool IMAGE stress SEED COUNT  (random operations, checked against a model)
 */
#include "udfeng.h"
#include "udfimg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#endif

static UDF_IMAGE Img;
static UDF_VOL Vol;

UDF_STATUS UdfHostRead(PUDF_VOL V, ULONGLONG Off, ULONG Len, PVOID Buf) { (void)V; return UimgRead(&Img, Off, Buf, Len) ? UDF_OK : UDF_IO_ERROR; }
UDF_STATUS UdfHostWrite(PUDF_VOL V, ULONGLONG Off, ULONG Len, const VOID *Buf) { (void)V; return UimgWrite(&Img, Off, Buf, Len) ? UDF_OK : UDF_IO_ERROR; }
UDF_STATUS UdfHostFlush(PUDF_VOL V) { (void)V; return UDF_OK; }
PVOID UdfHostAlloc(ULONG Bytes) { return malloc(Bytes ? Bytes : 1); }
VOID UdfHostFree(PVOID P) { free(P); }
ULONGLONG
UdfHostNowNt(VOID)
{
#ifdef _WIN32
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return 116444736000000000ULL + (ULONGLONG)tv.tv_sec * 10000000ULL + (ULONGLONG)tv.tv_usec * 10;
#endif
}

static void
Die(const char *What, UDF_STATUS St)
{
    fprintf(stderr, "%s: status %08lX\n", What, (unsigned long)St);
    exit(1);
}

/* walks a path from the root: the inode (held) and, for the last component, its parent */
static UDF_STATUS
Walk(const char *Path, PUDF_INODE *Out, PUDF_INODE *Parent, const char **Leaf)
{
    PUDF_INODE cur, par = NULL;
    UDF_STATUS st = UdfIget(&Vol, &Vol.Root, &cur);
    const char *p = Path;
    UDF_DIRENT e;
    if (!UDF_SUCCESS(st)) return st;
    if (Leaf) *Leaf = "";
    while (*p == '/') p++;
    while (*p) {
        const char *s = p;
        ULONG len;
        while (*p && *p != '/') p++;
        len = (ULONG)(p - s);
        while (*p == '/') p++;
        if (par) UdfIput(&Vol, par);
        par = cur;
        cur = NULL;
        if (Leaf) *Leaf = s;
        st = UdfDirLookup(&Vol, par, s, len, &e);
        if (!UDF_SUCCESS(st)) {
            if (*p == 0 && Parent) { *Parent = par; *Out = NULL; return st; }
            UdfIput(&Vol, par);
            return st;
        }
        st = UdfIget(&Vol, &e.Icb, &cur);
        if (!UDF_SUCCESS(st)) { UdfIput(&Vol, par); return st; }
    }
    *Out = cur;
    if (Parent) *Parent = par;
    else if (par) UdfIput(&Vol, par);
    return UDF_OK;
}

static ULONG
LeafLen(const char *Leaf)
{
    ULONG n = 0;
    while (Leaf[n] && Leaf[n] != '/') n++;
    return n;
}

static void
Commit(void)
{
    UDF_STATUS st = UdfCommit(&Vol);
    if (!UDF_SUCCESS(st)) Die("commit", st);
}

static void
List(PUDF_INODE Dir, const char *Prefix, int Recurse)
{
    UDF_DIRENT e;
    ULONGLONG cookie = 0;
    while (UDF_SUCCESS(UdfDirNext(&Vol, Dir, cookie, &e))) {
        PUDF_INODE I;
        cookie = e.Offset + e.FidLen;
        if (e.Chars & UDF_FID_PARENT) continue;
        if (!UDF_SUCCESS(UdfIget(&Vol, &e.Icb, &I))) { printf("?? %s%s\n", Prefix, e.Name); continue; }
        printf("%06o %3u %10llu %s%s%s\n", (unsigned)UdfIMode(I), I->Links, (unsigned long long)I->Size, Prefix, e.Name,
               (e.Chars & UDF_FID_HIDDEN) ? " [hidden]" : "");
        if (Recurse && UdfIsDir(I)) {
            char p[4096];
            snprintf(p, sizeof(p), "%s%s/", Prefix, e.Name);
            List(I, p, 1);
        }
        UdfIput(&Vol, I);
    }
}

static unsigned char *
Slurp(const char *Local, size_t *Len)
{
    FILE *f = fopen(Local, "rb");
    unsigned char *b;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (unsigned char *)malloc(n ? n : 1);
    if (n && fread(b, 1, n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    *Len = (size_t)n;
    return b;
}

static UDF_STATUS
PutFile(const char *Local, const char *Path)
{
    PUDF_INODE I, P;
    const char *leaf;
    size_t len;
    unsigned char *data = Slurp(Local, &len);
    UDF_STATUS st;
    if (!data) { fprintf(stderr, "cannot read %s\n", Local); exit(1); }
    st = Walk(Path, &I, &P, &leaf);
    if (UDF_SUCCESS(st)) { UdfIput(&Vol, I); UdfIput(&Vol, P); free(data); return UDF_EXISTS; }
    if (st != UDF_NOT_FOUND || !P) { free(data); return st; }
    st = UdfCreate(&Vol, P, leaf, LeafLen(leaf), UDF_CREATE_FILE, 0644, 0, 0, NULL, 0, &I);
    UdfIput(&Vol, P);
    if (!UDF_SUCCESS(st)) { free(data); return st; }
    /* as the driver does: blocks first, then the data straight to them, then the size */
    if (len) {
        ULONGLONG blocks = (len + Vol.BlockSize - 1) >> Vol.BlockShift, b = 0;
        st = UdfIallocRange(&Vol, I, 0, blocks, FALSE);
        while (UDF_SUCCESS(st) && b < blocks) {
            ULONGLONG abs;
            ULONG run;
            UCHAR kind;
            st = UdfImap(&Vol, I, b, &abs, &run, &kind);
            if (!UDF_SUCCESS(st)) break;
            if (kind != UDF_MAP_RECORDED) { st = UDF_CORRUPT; break; }
            if (run > blocks - b) run = (ULONG)(blocks - b);
            {
                size_t off = (size_t)(b << Vol.BlockShift), n = (size_t)run << Vol.BlockShift;
                unsigned char *tmp = (unsigned char *)calloc(1, n);
                memcpy(tmp, data + off, (off + n > len) ? len - off : n);
                st = UdfWriteAbs(&Vol, abs, run, tmp);
                free(tmp);
            }
            b += run;
        }
        if (UDF_SUCCESS(st)) st = UdfIsetSize(&Vol, I, len);
    }
    UdfIput(&Vol, I);
    free(data);
    return st;
}

#ifndef _WIN32
static void
PutTree(const char *Local, const char *Path)
{
    DIR *d = opendir(Local);
    struct dirent *de;
    UDF_STATUS st;
    if (!d) return;
    while ((de = readdir(d)) != NULL) {
        char lp[4096], up[4096];
        struct stat sb;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        snprintf(lp, sizeof(lp), "%s/%s", Local, de->d_name);
        snprintf(up, sizeof(up), "%s/%s", Path, de->d_name);
        if (lstat(lp, &sb) != 0) continue;
        if (S_ISDIR(sb.st_mode)) {
            PUDF_INODE I, P;
            const char *leaf;
            st = Walk(up, &I, &P, &leaf);
            if (st == UDF_NOT_FOUND && P) {
                st = UdfCreate(&Vol, P, leaf, LeafLen(leaf), UDF_CREATE_DIR, 0755, 0, 0, NULL, 0, &I);
                UdfIput(&Vol, P);
                if (!UDF_SUCCESS(st)) Die(up, st);
            } else if (UDF_SUCCESS(st)) {
                UdfIput(&Vol, P);
            } else Die(up, st);
            UdfIput(&Vol, I);
            PutTree(lp, up);
        } else if (S_ISREG(sb.st_mode)) {
            st = PutFile(lp, up);
            if (!UDF_SUCCESS(st)) Die(up, st);
        } else if (S_ISLNK(sb.st_mode)) {
            char t[4096];
            ssize_t n = readlink(lp, t, sizeof(t) - 1);
            PUDF_INODE I, P;
            const char *leaf;
            if (n < 0) continue;
            t[n] = 0;
            st = Walk(up, &I, &P, &leaf);
            if (st == UDF_NOT_FOUND && P) {
                st = UdfCreate(&Vol, P, leaf, LeafLen(leaf), UDF_CREATE_SYMLINK, 0777, 0, 0, t, (ULONG)n, &I);
                UdfIput(&Vol, P);
                if (UDF_SUCCESS(st)) UdfIput(&Vol, I);
            }
        }
        if (Vol.DirtyBlocks > 4096) Commit();
    }
    closedir(d);
}

static void
GetTree(PUDF_INODE Dir, const char *Local)
{
    UDF_DIRENT e;
    ULONGLONG cookie = 0;
    mkdir(Local, 0755);
    while (UDF_SUCCESS(UdfDirNext(&Vol, Dir, cookie, &e))) {
        PUDF_INODE I;
        char lp[4096];
        cookie = e.Offset + e.FidLen;
        if (e.Chars & UDF_FID_PARENT) continue;
        snprintf(lp, sizeof(lp), "%s/%s", Local, e.Name);
        if (!UDF_SUCCESS(UdfIget(&Vol, &e.Icb, &I))) { fprintf(stderr, "cannot read %s\n", lp); continue; }
        if (UdfIsDir(I)) GetTree(I, lp);
        else if (I->FileType == UDF_FT_SYMLINK) {
            char t[4096];
            ULONG n;
            if (UDF_SUCCESS(UdfReadLink(&Vol, I, t, sizeof(t) - 1, &n))) { t[n] = 0; symlink(t, lp); }
        } else {
            FILE *f = fopen(lp, "wb");
            ULONGLONG off = 0;
            unsigned char *buf = (unsigned char *)malloc(1 << 20);
            while (f && off < I->Size) {
                ULONG got;
                if (!UDF_SUCCESS(UdfIread(&Vol, I, off, 1 << 20, buf, &got)) || !got) break;
                fwrite(buf, 1, got, f);
                off += got;
            }
            if (f) fclose(f);
            free(buf);
        }
        UdfIput(&Vol, I);
    }
}
#endif

static UDF_STATUS
RemoveTree(PUDF_INODE Parent, const char *Name, ULONG Len)
{
    UDF_DIRENT e;
    PUDF_INODE I;
    UDF_STATUS st = UdfDirLookup(&Vol, Parent, Name, Len, &e);
    if (!UDF_SUCCESS(st)) return st;
    st = UdfIget(&Vol, &e.Icb, &I);
    if (!UDF_SUCCESS(st)) return st;
    if (UdfIsDir(I)) {
        UDF_DIRENT c;
        for (;;) {
            ULONGLONG cookie = 0;
            int found = 0;
            while (UDF_SUCCESS(UdfDirNext(&Vol, I, cookie, &c))) {
                cookie = c.Offset + c.FidLen;
                if (c.Chars & UDF_FID_PARENT) continue;
                found = 1;
                break;
            }
            if (!found) break;
            st = RemoveTree(I, c.Name, c.NameLen);
            if (!UDF_SUCCESS(st)) { UdfIput(&Vol, I); return st; }
        }
    }
    UdfIput(&Vol, I);
    return UdfUnlink(&Vol, Parent, Name, Len, FALSE);
}

/* ---- stress: random operations on a model ---------------------------------------- */
typedef struct {
    char Path[256];
    int IsDir;
    unsigned Seed;          /* the content: a generator seed */
    unsigned long Size;
    int Alive;
} MODEL;

static MODEL *M;
static int NM;
static unsigned Rng;
static unsigned Rand(void) { Rng = Rng * 1103515245u + 12345u; return (Rng >> 8) & 0xFFFFFF; }

static void
Fill(unsigned char *B, unsigned long Len, unsigned Seed)
{
    unsigned long i;
    unsigned x = Seed;
    for (i = 0; i < Len; i++) { x = x * 1664525u + 1013904223u; B[i] = (unsigned char)(x >> 24); }
}

static int
Check(void)
{
    int i, bad = 0;
    for (i = 0; i < NM; i++) {
        PUDF_INODE I;
        UDF_STATUS st;
        if (!M[i].Alive) continue;
        st = Walk(M[i].Path, &I, NULL, NULL);
        if (!UDF_SUCCESS(st)) { printf("MISSING %s (%08lX)\n", M[i].Path, (unsigned long)st); bad++; continue; }
        if (!M[i].IsDir) {
            unsigned char *want = (unsigned char *)malloc(M[i].Size + 1), *got = (unsigned char *)malloc(M[i].Size + 1);
            ULONG g = 0;
            Fill(want, M[i].Size, M[i].Seed);
            if (I->Size != M[i].Size) { printf("SIZE %s %llu != %lu\n", M[i].Path, (unsigned long long)I->Size, M[i].Size); bad++; }
            else if (!UDF_SUCCESS(UdfIread(&Vol, I, 0, M[i].Size, got, &g)) || g != M[i].Size || memcmp(want, got, M[i].Size)) {
                printf("DATA %s\n", M[i].Path);
                bad++;
            }
            free(want);
            free(got);
        }
        UdfIput(&Vol, I);
    }
    return bad;
}

static void
Stress(unsigned Seed, int Count)
{
    int op;
    Rng = Seed;
    M = (MODEL *)calloc(Count + 16, sizeof(MODEL));
    NM = 0;
    for (op = 0; op < Count; op++) {
        int kind = Rand() % 10;
        int target = NM ? (int)(Rand() % NM) : -1;
        char path[256], parent[256];
        int p = -1, k;
        UDF_STATUS st = UDF_OK;
        /* a parent directory: the root or a live directory */
        for (k = 0; k < 8 && NM; k++) {
            int c = (int)(Rand() % NM);
            if (M[c].Alive && M[c].IsDir && strlen(M[c].Path) < 180) { p = c; break; }
        }
        strcpy(parent, p >= 0 ? M[p].Path : "");
        if (kind <= 3 || target < 0) {
            /* a new file */
            unsigned long size = (Rand() % 4 == 0) ? Rand() % 300000 : Rand() % 5000;
            unsigned seed = Rand();
            unsigned char *b = (unsigned char *)malloc(size + 1);
            FILE *f;
            sprintf(path, "%s/f%d_%u", parent, op, Rand() % 1000);
            Fill(b, size, seed);
            f = fopen("/tmp/udfstress.tmp", "wb");
            fwrite(b, 1, size, f);
            fclose(f);
            free(b);
            st = PutFile("/tmp/udfstress.tmp", path);
            if (UDF_SUCCESS(st)) { strcpy(M[NM].Path, path); M[NM].Seed = seed; M[NM].Size = size; M[NM].Alive = 1; NM++; }
        } else if (kind == 4) {
            PUDF_INODE I, P;
            const char *leaf;
            sprintf(path, "%s/d%d", parent, op);
            st = Walk(path, &I, &P, &leaf);
            if (st == UDF_NOT_FOUND && P) {
                st = UdfCreate(&Vol, P, leaf, LeafLen(leaf), UDF_CREATE_DIR, 0755, 0, 0, NULL, 0, &I);
                UdfIput(&Vol, P);
                if (UDF_SUCCESS(st)) { UdfIput(&Vol, I); strcpy(M[NM].Path, path); M[NM].IsDir = 1; M[NM].Alive = 1; NM++; }
            }
        } else if (kind <= 6 && M[target].Alive && !M[target].IsDir) {
            /* delete a file */
            PUDF_INODE I, P;
            const char *leaf;
            st = Walk(M[target].Path, &I, &P, &leaf);
            if (UDF_SUCCESS(st)) {
                UdfIput(&Vol, I);
                st = UdfUnlink(&Vol, P, leaf, LeafLen(leaf), FALSE);
                UdfIput(&Vol, P);
                if (UDF_SUCCESS(st)) M[target].Alive = 0;
            }
        } else if (kind == 7 && M[target].Alive && !M[target].IsDir) {
            /* rename a file into a directory */
            PUDF_INODE I, P, NP, NI;
            const char *leaf, *nleaf;
            sprintf(path, "%s/r%d", parent, op);
            st = Walk(M[target].Path, &I, &P, &leaf);
            if (UDF_SUCCESS(st)) {
                UdfIput(&Vol, I);
                st = Walk(path, &NI, &NP, &nleaf);
                if (st == UDF_NOT_FOUND && NP) {
                    st = UdfRename(&Vol, P, leaf, LeafLen(leaf), NP, nleaf, LeafLen(nleaf));
                    UdfIput(&Vol, NP);
                    if (UDF_SUCCESS(st)) strcpy(M[target].Path, path);
                } else if (UDF_SUCCESS(st)) { UdfIput(&Vol, NI); UdfIput(&Vol, NP); }
                UdfIput(&Vol, P);
            }
        } else if (kind == 8 && M[target].Alive && !M[target].IsDir) {
            /* truncate or extend */
            PUDF_INODE I;
            st = Walk(M[target].Path, &I, NULL, NULL);
            if (UDF_SUCCESS(st)) {
                unsigned long ns = Rand() % (M[target].Size + 2);
                st = UdfIsetSize(&Vol, I, ns);
                if (UDF_SUCCESS(st)) {
                    /* the model: the old content up to the new size */
                    unsigned char *b = (unsigned char *)malloc(M[target].Size + 1);
                    FILE *f;
                    PUDF_INODE P;
                    const char *leaf;
                    Fill(b, M[target].Size, M[target].Seed);
                    UdfIput(&Vol, I);
                    /* rewrite as a new file with the shortened content so the model stays a seed */
                    st = Walk(M[target].Path, &I, &P, &leaf);
                    if (UDF_SUCCESS(st)) {
                        UdfIput(&Vol, I);
                        st = UdfUnlink(&Vol, P, leaf, LeafLen(leaf), FALSE);
                        UdfIput(&Vol, P);
                    }
                    M[target].Size = ns < M[target].Size ? ns : M[target].Size;
                    f = fopen("/tmp/udfstress.tmp", "wb");
                    fwrite(b, 1, M[target].Size, f);
                    fclose(f);
                    free(b);
                    if (UDF_SUCCESS(st)) st = PutFile("/tmp/udfstress.tmp", M[target].Path);
                    I = NULL;
                }
                if (I) UdfIput(&Vol, I);
            }
        } else {
            if (Rand() % 4 == 0) Commit();
        }
        if (!UDF_SUCCESS(st) && st != UDF_NOT_FOUND && st != UDF_EXISTS && st != UDF_DISK_FULL) {
            printf("op %d kind %d failed %08lX\n", op, kind, (unsigned long)st);
            exit(1);
        }
    }
    Commit();
    printf("stress: %d objects, %d problems\n", NM, Check());
}

int
main(int argc, char **argv)
{
    UDF_STATUS st;
    const char *cmd;
    int ro;
    if (argc < 3) { fprintf(stderr, "usage: udftool IMAGE COMMAND ...\n"); return 2; }
    cmd = argv[2];
    ro = !strcmp(cmd, "info") || !strcmp(cmd, "ls") || !strcmp(cmd, "lsr") || !strcmp(cmd, "cat") ||
         !strcmp(cmd, "get") || !strcmp(cmd, "getr") || !strcmp(cmd, "readlink") || !strcmp(cmd, "stat");
    if (!UimgOpen(&Img, argv[1], -1, ro)) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    Vol.DeviceBytes = Img.Size;
    Vol.SectorSize = 512;
    st = UdfMount(&Vol, (BOOLEAN)ro);
    if (!UDF_SUCCESS(st)) Die("mount", st);
    if (!ro && Vol.ReadOnly) { fprintf(stderr, "read-only: %s\n", Vol.ReadOnlyWhy ? Vol.ReadOnlyWhy : "?"); return 1; }

    if (!strcmp(cmd, "info")) {
        ULONGLONG total, free_;
        ULONG i;
        UdfFreeSpace(&Vol, &total, &free_);
        printf("UDF %x.%02x, block %lu, label '%s', serial %08lX\n", Vol.Revision >> 8, Vol.Revision & 0xFF,
               (unsigned long)Vol.BlockSize, Vol.Label, (unsigned long)Vol.Serial);
        printf("blocks %llu, data partition %llu blocks, %llu free; files %lu dirs %lu; next unique id %llu; integrity %s\n",
               (unsigned long long)Vol.Blocks, (unsigned long long)total, (unsigned long long)free_, (unsigned long)Vol.NumFiles,
               (unsigned long)Vol.NumDirs, (unsigned long long)Vol.NextUniqueId, Vol.WasOpen ? "open" : "closed");
        for (i = 0; i < Vol.NMaps; i++) {
            PUDF_PMAP m = &Vol.Map[i];
            static const char *k[] = { "?", "physical", "sparable", "virtual", "metadata" };
            printf("map %lu: %s partition %u start %lu length %lu%s%s\n", (unsigned long)i, k[m->Kind], m->Number,
                   (unsigned long)m->Start, (unsigned long)m->Length, m->HasBitmap ? " bitmap" : "",
                   m->Kind == UDF_KIND_SPAR && m->SpareCount ? " (sparing table)" : "");
        }
        printf("icb partition %u, data partition %u, %s, %s\n", Vol.IcbPart, Vol.DataPart, Vol.UseEfe ? "EFE" : "FE",
               Vol.ReadOnlyForced ? Vol.ReadOnlyWhy : "writable");
    } else if (!strcmp(cmd, "ls") || !strcmp(cmd, "lsr")) {
        PUDF_INODE I;
        st = Walk(argc > 3 ? argv[3] : "/", &I, NULL, NULL);
        if (!UDF_SUCCESS(st)) Die("walk", st);
        List(I, "", cmd[2] == 'r');
        UdfIput(&Vol, I);
    } else if (!strcmp(cmd, "cat") || !strcmp(cmd, "get")) {
        PUDF_INODE I;
        FILE *out = stdout;
        ULONGLONG off = 0;
        unsigned char *buf = (unsigned char *)malloc(1 << 20);
        st = Walk(argv[3], &I, NULL, NULL);
        if (!UDF_SUCCESS(st)) Die("walk", st);
        if (cmd[0] == 'g') out = fopen(argv[4], "wb");
        while (off < I->Size) {
            ULONG got;
            st = UdfIread(&Vol, I, off, 1 << 20, buf, &got);
            if (!UDF_SUCCESS(st)) Die("read", st);
            if (!got) break;
            fwrite(buf, 1, got, out);
            off += got;
        }
        if (out != stdout) fclose(out);
        UdfIput(&Vol, I);
    } else if (!strcmp(cmd, "put")) {
        st = PutFile(argv[3], argv[4]);
        if (!UDF_SUCCESS(st)) Die("put", st);
        Commit();
#ifndef _WIN32
    } else if (!strcmp(cmd, "putr")) {
        PUDF_INODE I, P;
        const char *leaf;
        st = Walk(argv[4], &I, &P, &leaf);
        if (st == UDF_NOT_FOUND && P) {
            st = UdfCreate(&Vol, P, leaf, LeafLen(leaf), UDF_CREATE_DIR, 0755, 0, 0, NULL, 0, &I);
            UdfIput(&Vol, P);
            if (!UDF_SUCCESS(st)) Die("mkdir", st);
            UdfIput(&Vol, I);
        } else if (UDF_SUCCESS(st)) { UdfIput(&Vol, I); if (P) UdfIput(&Vol, P); }
        PutTree(argv[3], argv[4]);
        Commit();
    } else if (!strcmp(cmd, "getr")) {
        PUDF_INODE I;
        st = Walk(argv[3], &I, NULL, NULL);
        if (!UDF_SUCCESS(st)) Die("walk", st);
        GetTree(I, argv[4]);
        UdfIput(&Vol, I);
#endif
    } else if (!strcmp(cmd, "mkdir") || !strcmp(cmd, "symlink")) {
        PUDF_INODE I, P;
        const char *leaf, *path = cmd[0] == 'm' ? argv[3] : argv[4];
        st = Walk(path, &I, &P, &leaf);
        if (UDF_SUCCESS(st)) Die("exists", UDF_EXISTS);
        if (st != UDF_NOT_FOUND || !P) Die("walk", st);
        if (cmd[0] == 'm') st = UdfCreate(&Vol, P, leaf, LeafLen(leaf), UDF_CREATE_DIR, 0755, 0, 0, NULL, 0, &I);
        else st = UdfCreate(&Vol, P, leaf, LeafLen(leaf), UDF_CREATE_SYMLINK, 0777, 0, 0, argv[3], (ULONG)strlen(argv[3]), &I);
        if (!UDF_SUCCESS(st)) Die("create", st);
        UdfIput(&Vol, I);
        UdfIput(&Vol, P);
        Commit();
    } else if (!strcmp(cmd, "stat")) {
        PUDF_INODE I;
        ULONG i;
        st = Walk(argv[3], &I, NULL, NULL);
        if (!UDF_SUCCESS(st)) Die("walk", st);
        printf("icb %u:%lu %s type %u ad %u size %llu links %u uid %llu aeds %lu\n", I->Loc.Part, (unsigned long)I->Loc.Lbn,
               I->Extended ? "EFE" : "FE", I->FileType, I->AdType, (unsigned long long)I->Size, I->Links,
               (unsigned long long)I->UniqueId, (unsigned long)I->NAed);
        for (i = 0; i < I->NExt; i++)
            printf("  ext %lu: off %llu len %lu type %u at %u:%lu\n", (unsigned long)i, (unsigned long long)I->Ext[i].Off,
                   (unsigned long)I->Ext[i].Len, I->Ext[i].Type, I->Ext[i].Part, (unsigned long)I->Ext[i].Lbn);
        UdfIput(&Vol, I);
    } else if (!strcmp(cmd, "readlink")) {
        PUDF_INODE I;
        char t[4096];
        ULONG n;
        st = Walk(argv[3], &I, NULL, NULL);
        if (!UDF_SUCCESS(st)) Die("walk", st);
        st = UdfReadLink(&Vol, I, t, sizeof(t) - 1, &n);
        if (!UDF_SUCCESS(st)) Die("readlink", st);
        t[n] = 0;
        printf("%s\n", t);
    } else if (!strcmp(cmd, "rm") || !strcmp(cmd, "rmr")) {
        PUDF_INODE I, P;
        const char *leaf;
        st = Walk(argv[3], &I, &P, &leaf);
        if (!UDF_SUCCESS(st)) Die("walk", st);
        UdfIput(&Vol, I);
        st = cmd[2] == 'r' ? RemoveTree(P, leaf, LeafLen(leaf)) : UdfUnlink(&Vol, P, leaf, LeafLen(leaf), FALSE);
        if (!UDF_SUCCESS(st)) Die("unlink", st);
        UdfIput(&Vol, P);
        Commit();
    } else if (!strcmp(cmd, "mv") || !strcmp(cmd, "ln")) {
        PUDF_INODE I, P, NI, NP;
        const char *leaf, *nleaf;
        st = Walk(argv[3], &I, &P, &leaf);
        if (!UDF_SUCCESS(st)) Die("walk", st);
        st = Walk(argv[4], &NI, &NP, &nleaf);
        if (UDF_SUCCESS(st)) Die("exists", UDF_EXISTS);
        if (!NP) Die("walk", st);
        if (cmd[0] == 'm') st = UdfRename(&Vol, P, leaf, LeafLen(leaf), NP, nleaf, LeafLen(nleaf));
        else st = UdfLink(&Vol, NP, nleaf, LeafLen(nleaf), I);
        if (!UDF_SUCCESS(st)) Die(cmd, st);
        UdfIput(&Vol, I);
        UdfIput(&Vol, P);
        UdfIput(&Vol, NP);
        Commit();
    } else if (!strcmp(cmd, "label")) {
        st = UdfSetLabel(&Vol, argv[3], (ULONG)strlen(argv[3]));
        if (!UDF_SUCCESS(st)) Die("label", st);
        Commit();
    } else if (!strcmp(cmd, "truncate")) {
        PUDF_INODE I;
        st = Walk(argv[3], &I, NULL, NULL);
        if (!UDF_SUCCESS(st)) Die("walk", st);
        st = UdfChangeBegin(&Vol);
        if (UDF_SUCCESS(st)) st = UdfIsetSize(&Vol, I, strtoull(argv[4], NULL, 0));
        if (!UDF_SUCCESS(st)) Die("truncate", st);
        UdfIput(&Vol, I);
        Commit();
    } else if (!strcmp(cmd, "write")) {
        PUDF_INODE I;
        size_t len;
        unsigned char *data = Slurp(argv[5], &len);
        st = Walk(argv[3], &I, NULL, NULL);
        if (!UDF_SUCCESS(st)) Die("walk", st);
        st = UdfChangeBegin(&Vol);
        if (UDF_SUCCESS(st)) st = UdfIwriteData(&Vol, I, strtoull(argv[4], NULL, 0), (ULONG)len, data);
        if (!UDF_SUCCESS(st)) Die("write", st);
        UdfIput(&Vol, I);
        Commit();
    } else if (!strcmp(cmd, "stress")) {
        st = UdfChangeBegin(&Vol);
        if (!UDF_SUCCESS(st)) Die("begin", st);
        Stress((unsigned)atoi(argv[3]), atoi(argv[4]));
    } else {
        fprintf(stderr, "unknown command %s\n", cmd);
        return 2;
    }
    UdfUnmount(&Vol);
    UimgClose(&Img);
    return 0;
}
