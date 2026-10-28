/* Adapted from pinned LAS Android. Copyright (c) 2026 Peiikai.
 * MIT: native/runtime/LICENSE-LASRecomp. Title effects require the LAS adapter. */
// fsp-srv: RomFS as a raw storage image, save data and SD card backed by host directories.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#undef IN   // (windows.h's empty parameter markers; ipc.h defines its own)
#undef OUT
#endif

#include <pthread.h>

#include "../hle.h"
#include "../rando.h"
#include "runtime/guest_path.h"
#include "../ipc.h"
#include "../romfs_build.h"
#include "../settings.h"
#include "../nxpkg.h"
#include "../native_package_android.h"

#define FS_PATH_NOT_FOUND MAKE_RESULT(2, 1)
#define FS_PATH_EXISTS MAKE_RESULT(2, 2)
#define FS_TARGET_LOCKED MAKE_RESULT(2, 7)
#define FS_NOT_EMPTY MAKE_RESULT(2, 8)

const char *g_save_dir = "save";

// ------------------------------------------------------------------ the base RomFS image
// The game reads its data as one raw RomFS image. It's served from the player's romfs/ folder: the header and tables
// laid out in memory (runtime/romfs_build.c), each file's bytes read from its own file, the gaps between files zeros -
// no image file on disk, so the game data isn't kept twice. A romfs.bin from an older version is used only when the
// folder isn't there.
static RomfsLayout g_base;
static bool g_base_open;
static int g_base_fd = -1;  // (an older version's romfs.bin)
static NxRomfs *g_base_package;
static pthread_mutex_t g_base_lock = PTHREAD_MUTEX_INITIALIZER;
#define BASE_FDS 32  // files kept open (macOS allows 256 open files by default)
static struct { int file, fd; uint64_t used; } g_base_fds[BASE_FDS];
static uint64_t g_base_tick;

static int base_file_fd(int i) {  // (g_base_lock held)
    int slot = 0;
    for (int k = 0; k < BASE_FDS; k++) {
        if (g_base_fds[k].fd > 0 && g_base_fds[k].file == i) { g_base_fds[k].used = ++g_base_tick; return g_base_fds[k].fd; }
        if (g_base_fds[k].used < g_base_fds[slot].used) slot = k;
    }
    if (g_base_fds[slot].fd > 0) close(g_base_fds[slot].fd);
    int fd = open(g_base.files[i].path, O_RDONLY);
    g_base_fds[slot].file = i, g_base_fds[slot].fd = fd > 0 ? fd : 0, g_base_fds[slot].used = ++g_base_tick;
    return fd;
}

static uint64_t base_size(void) {
    if (g_base_package) return nx_romfs_size(g_base_package);
    if (g_base_fd >= 0) {
        struct stat sb;
        fstat(g_base_fd, &sb);
        return (uint64_t)sb.st_size;
    }
    return g_base.size;
}

// pread into guest memory the OS writes: plat_commit first (runtime/platform.h). A write-tracking query on another
// thread can protect a page again before or during the read (Android): the read stops there (EFAULT, or short) and
// goes on from where it stopped, committed again.
static ssize_t guest_pread(int fd, void *dst, size_t n, off_t off) {
    size_t done = 0;
    for (int faults = 0; done < n;) {
        plat_commit((uint64_t)(uintptr_t)dst + done, n - done);
        ssize_t r = pread(fd, (uint8_t *)dst + done, n - done, off + (off_t)done);
        if (r < 0) {
            if (errno != EFAULT || ++faults > 16) return done ? (ssize_t)done : -1;
            continue;
        }
        if (r == 0) break;  // (end of file)
        done += (size_t)r;
    }
    return (ssize_t)done;
}

// n bytes of the base image at off (past its end: zeros)
static ssize_t base_pread(void *dst_, uint64_t n, uint64_t off) {
    if (g_base_package) {
        ssize_t got;
        for (unsigned faults = 0;; ++faults) {
            plat_commit((uint64_t)(uintptr_t)dst_, n);
            errno = 0;
            got = nx_romfs_pread(g_base_package, dst_, n, off);
            if (got >= 0 || errno != EFAULT || faults >= 16) break;
        }
        if (got < 0) FATAL("[fs] failed to read the selected game package at %llx\n", (unsigned long long)off);
        return got;
    }
    if (g_base_fd >= 0) return guest_pread(g_base_fd, dst_, n, (off_t)off);
    uint8_t *dst = dst_;
    memset(dst, 0, n);
    uint64_t end = off + n;
    if (off < ROMFS_DATA_START) memcpy(dst, g_base.header + off, (end < ROMFS_DATA_START ? end : ROMFS_DATA_START) - off);
    uint64_t ta = g_base.tables_off, tb = ta + g_base.tables_size;
    if (off < tb && end > ta) {
        uint64_t a = off > ta ? off : ta, b = end < tb ? end : tb;
        memcpy(dst + (a - off), g_base.tables + (a - ta), b - a);
    }
    // the files overlapping [off, end): the first by binary search, then in order
    int lo = 0, hi = g_base.nfiles - 1, first = g_base.nfiles;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_base.files[mid].start + g_base.files[mid].size > off) first = mid, hi = mid - 1;
        else lo = mid + 1;
    }
    pthread_mutex_lock(&g_base_lock);
    for (int i = first; i < g_base.nfiles && g_base.files[i].start < end; i++) {
        const RomfsLayoutFile *f = &g_base.files[i];
        uint64_t a = off > f->start ? off : f->start, b = end < f->start + f->size ? end : f->start + f->size;
        if (a >= b) continue;
        int fd = base_file_fd(i);
        if (fd <= 0 || guest_pread(fd, dst + (a - off), b - a, (off_t)(a - f->start)) != (ssize_t)(b - a)) {
            pthread_mutex_unlock(&g_base_lock);
            LOG("[fs] can't read %s\n", f->path);
            return -1;
        }
    }
    pthread_mutex_unlock(&g_base_lock);
    return (ssize_t)n;
}

static void base_close(void) {
    if (g_base_package) nx_romfs_close(g_base_package);
    g_base_package = NULL;
    for (int k = 0; k < BASE_FDS; k++) if (g_base_fds[k].fd > 0) close(g_base_fds[k].fd);
    memset(g_base_fds, 0, sizeof g_base_fds);
    romfs_layout_free(&g_base);
    if (g_base_fd >= 0) close(g_base_fd);
    g_base_fd = -1;
    g_base_open = false;
}

static void base_open(void) {
    if (g_base_open) return;
    char path[1100], why[256] = "";
    struct stat sb;
    const char *package_uri = getenv("LARECOMP_PACKAGE_URI"), *package_path = getenv("LARECOMP_PACKAGE_PATH");
    if ((package_uri && *package_uri) || (package_path && *package_path)) {
        g_base_package = shared_package_open_romfs(why, sizeof why);
        if (!g_base_package) FATAL("[fs] cannot open the selected game package: %s\n", why);
        LOG("[fs] romfs: %llu bytes read directly from selected package\n", (unsigned long long)nx_romfs_size(g_base_package));
        g_base_open = true;
        return;
    }
    if (stat(g_romfs_dir, &sb) == 0 && S_ISDIR(sb.st_mode) &&
        romfs_layout(g_romfs_dir, &g_base, why, sizeof why) && g_base.nfiles > 0) {
        LOG("[fs] romfs: %d files served from %s\n", g_base.nfiles, g_romfs_dir);
    } else {
        romfs_layout_free(&g_base);
        snprintf(path, sizeof path, "%s.bin", g_romfs_dir);  // (an older version's image)
        g_base_fd = open(path, O_RDONLY);
        if (g_base_fd < 0) FATAL("cannot read the game data in %s %s(set up the game files on the start screen)\n",
                                 g_romfs_dir, why);
    }
    g_base_open = true;
}

// ------------------------------------------------------------------ RomFS file index (offset -> path)
typedef struct { uint64_t start, size; char *path; bool warned; const uint8_t *mem; } Overlay;
// in-image patches (copies of RomFS tables with changes): header, dir meta, file hash, file meta
typedef struct { uint64_t off, size; uint8_t *data; } Patch;
static Patch patches[8];
static int npatches;
static Overlay *ov;
static int ov_n, ov_cap;
static uint8_t *ov_meta;
static uint64_t ov_meta_off, ov_meta_size, ov_end;
// Lets the runtime see which game files are being streamed (e.g. which level is loading).
typedef struct { uint64_t off, size; char *path; uint32_t meta; } RomFile;
static RomFile *rom_files;
static int rom_nfiles;
static uint64_t rom_data_off;

// A seed replaces ~850 files and adds more: each is opened when it's read and kept in a small cache like the base
// files', never all at once. They used to stay open until reboot, which ran into the open-file limit (256 for a
// macOS app) mid-play: saves failed to stage ("could not be saved") and seed-file reads came back as zeros.
#define OV_FDS 64
static struct { int ov, fd; uint64_t used; } g_ov_fds[OV_FDS];
static uint64_t g_ov_tick;
static pthread_mutex_t g_ov_lock = PTHREAD_MUTEX_INITIALIZER;
int fs_overlay_open_fds(void) {  // (tests)
    int n = 0;
    pthread_mutex_lock(&g_ov_lock);
    for (int k = 0; k < OV_FDS; k++) n += g_ov_fds[k].fd > 0;
    pthread_mutex_unlock(&g_ov_lock);
    return n;
}
static void ov_fds_close(void) {  // (g_ov_lock held)
    for (int k = 0; k < OV_FDS; k++) if (g_ov_fds[k].fd > 0) close(g_ov_fds[k].fd);
    memset(g_ov_fds, 0, sizeof g_ov_fds);
}
// replacement file i's fd, opened on demand (-1, logged once per file, if it can't be opened; g_ov_lock held)
static int ov_file_fd(int i) {
    int slot = 0;
    for (int k = 0; k < OV_FDS; k++) {
        if (g_ov_fds[k].fd > 0 && g_ov_fds[k].ov == i) { g_ov_fds[k].used = ++g_ov_tick; return g_ov_fds[k].fd; }
        if (g_ov_fds[k].used < g_ov_fds[slot].used) slot = k;
    }
    if (g_ov_fds[slot].fd > 0) close(g_ov_fds[slot].fd);
    g_ov_fds[slot].fd = 0, g_ov_fds[slot].used = 0;
    int fd = open(ov[i].path, O_RDONLY);
    if (fd < 0 && (errno == EMFILE || errno == ENFILE)) {  // out of descriptors: give back the cache's, try once more
        int error = errno;
        ov_fds_close();
        fd = open(ov[i].path, O_RDONLY);
        if (fd < 0) errno = error;
    }
    if (fd < 0) {
        if (!ov[i].warned) LOG("[fs] ERROR: can't open randomizer file %s (%s): the game reads zeros there\n", ov[i].path,
                               strerror(errno));
        ov[i].warned = true;
        return -1;
    }
    if (fd == 0) {  // (0 marks a free slot: keep stdin's number out of the cache)
        int d = dup(fd);
        close(fd);
        fd = d;
        if (fd < 0) return -1;
    }
    g_ov_fds[slot].ov = i, g_ov_fds[slot].fd = fd, g_ov_fds[slot].used = ++g_ov_tick;
    return fd;
}

// Reboot: the next seed builds its own index / overlay when the game opens the RomFS again.
static void mounts_reset(void);
RT_ON_RESET(fs_reset) {
    pthread_mutex_lock(&g_ov_lock);
    ov_fds_close();
    pthread_mutex_unlock(&g_ov_lock);
    for (int i = 0; i < ov_n; i++) { free(ov[i].path); free((void *)ov[i].mem); }
    for (int i = 0; i < npatches; i++) free(patches[i].data);  // (ov_meta and the extended table are among these)
    for (int i = 0; i < rom_nfiles; i++) free(rom_files[i].path);
    free(ov), free(rom_files);
    ov = NULL, ov_n = ov_cap = 0;
    ov_meta = NULL, ov_meta_off = ov_meta_size = ov_end = 0;
    npatches = 0;
    rom_files = NULL, rom_nfiles = 0, rom_data_off = 0;
    base_close();
    mounts_reset();  // (the old boot's mounts are gone with it)
    g_save_dir = "save";
}

static int rom_cmp(const void *a, const void *b) {
    const RomFile *x = a, *y = b;
    return x->off < y->off ? -1 : x->off > y->off;
}

static void rom_dir_path(const uint8_t *dmeta, uint32_t dsize, uint32_t dir, char *out, size_t cap) {
    char parts[16][256];
    int n = 0;
    while (dir != 0 && dir < dsize && n < 16) {  // root (offset 0) has an empty name
        uint32_t parent, namelen;
        memcpy(&parent, dmeta + dir, 4);
        memcpy(&namelen, dmeta + dir + 0x14, 4);
        if (namelen > 255) namelen = 255;
        memcpy(parts[n], dmeta + dir + 0x18, namelen);
        parts[n][namelen] = 0;
        n++;
        dir = parent;
    }
    out[0] = 0;
    for (int i = n - 1; i >= 0; i--) { strlcat(out, parts[i], cap); strlcat(out, "/", cap); }
}

// ---- files a seed ADDS (names the game's RomFS doesn't have: generated dungeon cells...). The game parses the
// RomFS tables itself, so they're extended: a copy of the file-meta table with the new entries appended (linked
// into their directory's file list and the name hash buckets) is placed past the end of the image, the header,
// directory table and hash table are patched to match. New files must go into existing directories.
static uint32_t romfs_hash(uint32_t parent, const char *name, uint32_t len) {
    uint32_t hash = parent ^ 123456789;
    for (uint32_t i = 0; i < len; i++) { hash = (hash >> 5) | (hash << 27); hash ^= (uint8_t)name[i]; }
    return hash;
}

static int path_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

typedef struct { char rel[1024]; char host[1200]; uint64_t size; } NewFile;

static void walk_new(const char *root, const char *rel, char **known, int nknown, NewFile **out, int *n, int *cap) {
    char dirp[1200];
    snprintf(dirp, sizeof dirp, "%s/%s", root, rel);
    DIR *d = opendir(dirp);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char r2[1024], h2[1200];
        snprintf(r2, sizeof r2, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        snprintf(h2, sizeof h2, "%s/%s", root, r2);
        struct stat sb;
        if (stat(h2, &sb)) continue;
        if (S_ISDIR(sb.st_mode)) { walk_new(root, r2, known, nknown, out, n, cap); continue; }
        const char *key = r2;
        if (bsearch(&key, known, nknown, sizeof *known, path_cmp)) continue;  // replaces an existing file
        if (*n == *cap) { *cap = *cap ? *cap * 2 : 64; *out = realloc(*out, sizeof **out * *cap); }
        strlcpy((*out)[*n].rel, r2, sizeof (*out)[*n].rel);
        strlcpy((*out)[*n].host, h2, sizeof (*out)[*n].host);
        (*out)[*n].size = (uint64_t)sb.st_size;
        (*n)++;
    }
    closedir(d);
}

// directory entry offset for "a/b/c" (NULL-terminated walk from the root, offset 0), -1 if missing
static int64_t dir_lookup(const uint8_t *dm, uint64_t dsz, const char *path) {
    uint32_t cur = 0;
    char buf[1024];
    strlcpy(buf, path, sizeof buf);
    for (char *tok = strtok(buf, "/"); tok; tok = strtok(NULL, "/")) {
        uint32_t child;
        memcpy(&child, dm + cur + 8, 4);
        while (child != 0xFFFFFFFFu && child + 0x18 <= dsz) {
            uint32_t nl;
            memcpy(&nl, dm + child + 0x14, 4);
            if (nl == strlen(tok) && !memcmp(dm + child + 0x18, tok, nl)) break;
            memcpy(&child, dm + child + 4, 4);
        }
        if (child == 0xFFFFFFFFu || child + 0x18 > dsz) return -1;
        cur = child;
    }
    return cur;
}

static void add_new_files(const uint64_t *h, const uint8_t *dmeta, uint64_t dsz, uint64_t fsz) {
    const char *seed = shared_title_las() ? rando_seed_dir() : NULL;
    if (!seed) return;
    char root[1100];
    snprintf(root, sizeof root, "%s/romfs", seed);
    char **known = malloc(sizeof *known * rom_nfiles);
    for (int i = 0; i < rom_nfiles; i++) known[i] = rom_files[i].path;
    qsort(known, rom_nfiles, sizeof *known, path_cmp);
    NewFile *nf = NULL;
    int n = 0, cap = 0;
    walk_new(root, "", known, rom_nfiles, &nf, &n, &cap);
    free(known);
    if (!n) return;
    rom_files = realloc(rom_files, sizeof(RomFile) * (rom_nfiles + n));
    uint64_t hoff = h[5], hsz = h[6];
    uint8_t *dcopy = malloc(dsz), *hcopy = malloc(hsz), *head = malloc(0x50);
    memcpy(dcopy, dmeta, dsz);
    if (base_pread(hcopy, hsz, hoff) != (ssize_t)hsz || base_pread(head, 0x50, 0) != 0x50) {
        free(dcopy), free(hcopy), free(head), free(nf);
        return;
    }
    // the file-meta table to extend: the override-patched copy if there is one
    const uint8_t *base_meta = ov_meta;
    uint8_t *tmp = NULL;
    if (!base_meta) { tmp = malloc(fsz); base_pread(tmp, fsz, h[7]); base_meta = tmp; }
    uint64_t extra = 0;
    for (int i = 0; i < n; i++) {
        const char *name = strrchr(nf[i].rel, '/');
        name = name ? name + 1 : nf[i].rel;
        extra += 0x20 + ((strlen(name) + 3) & ~3u);
    }
    uint64_t nsz = fsz + extra;
    uint8_t *meta = calloc(1, nsz);
    memcpy(meta, base_meta, fsz);
    free(tmp);
    if (!ov_end) ov_end = (base_size() + 0xF) & ~0xFull;
    uint64_t meta_at = ov_end;  // the new table first, then the new files' data
    uint64_t data_at = (meta_at + nsz + 0xF) & ~0xFull;
    uint32_t nb = (uint32_t)(hsz / 4);
    uint64_t o = fsz;
    int added = 0;
    for (int i = 0; i < n; i++) {
        char dirp[1024];
        strlcpy(dirp, nf[i].rel, sizeof dirp);
        char *slash = strrchr(dirp, '/');
        const char *name = slash ? slash + 1 : nf[i].rel;
        if (slash) *slash = 0; else dirp[0] = 0;
        int64_t dir = dir_lookup(dcopy, dsz, dirp);
        if (dir < 0) { LOG("[fs] seed file %s: directory not in the game's RomFS, skipped\n", nf[i].rel); continue; }
        uint32_t nl = (uint32_t)strlen(name), d32 = (uint32_t)dir, entry = (uint32_t)o, child;
        memcpy(&child, dcopy + dir + 0xC, 4);  // dir's first file -> becomes our sibling
        uint64_t rel = data_at - rom_data_off, size = nf[i].size;
        uint32_t bucket = romfs_hash(d32, name, nl) % nb, prev;
        memcpy(&prev, hcopy + 4 * bucket, 4);
        memcpy(meta + o, &d32, 4);
        memcpy(meta + o + 4, &child, 4);
        memcpy(meta + o + 8, &rel, 8);
        memcpy(meta + o + 0x10, &size, 8);
        memcpy(meta + o + 0x18, &prev, 4);
        memcpy(meta + o + 0x1C, &nl, 4);
        memcpy(meta + o + 0x20, name, nl);
        memcpy(dcopy + dir + 0xC, &entry, 4);
        memcpy(hcopy + 4 * bucket, &entry, 4);
        o += 0x20 + ((nl + 3) & ~3u);
        if (ov_n == ov_cap) { ov_cap = ov_cap ? ov_cap * 2 : 256; ov = realloc(ov, sizeof *ov * ov_cap); }
        ov[ov_n++] = (Overlay){data_at, size, strdup(nf[i].host), false, NULL};
        rom_files[rom_nfiles++] = (RomFile){data_at, size, strdup(nf[i].rel), entry};
        data_at = (data_at + size + 0xF) & ~0xFull;
        added++;
    }
    // the new table goes before the new data (keep the overlay list sorted by start)
    if (ov_n == ov_cap) { ov_cap = ov_cap ? ov_cap * 2 : 256; ov = realloc(ov, sizeof *ov * ov_cap); }
    memmove(ov + (ov_n - added) + 1, ov + (ov_n - added), sizeof *ov * added);
    ov[ov_n - added] = (Overlay){meta_at, nsz, NULL, false, meta};
    ov_n++;
    ov_end = data_at;
    uint64_t fmeta_off = meta_at, fmeta_size = o;
    memcpy(head + 7 * 8, &fmeta_off, 8);
    memcpy(head + 8 * 8, &fmeta_size, 8);
    patches[npatches++] = (Patch){0, 0x50, head};
    patches[npatches++] = (Patch){h[3], dsz, dcopy};
    patches[npatches++] = (Patch){hoff, hsz, hcopy};
    free(nf);
    LOG("[fs] randomizer: %d new romfs files\n", added);
}

static void rom_index(void) {
    uint64_t h[10];
    if (base_pread(h, sizeof h, 0) != (ssize_t)sizeof h) return;
    // header: size, dir hash (off,size), dir meta (off,size), file hash (off,size), file meta (off,size), data off
    uint64_t doff = h[3], dsz = h[4], foff = h[7], fsz = h[8];
    rom_data_off = h[9];
    uint8_t *dmeta = malloc(dsz), *fmeta = malloc(fsz);
    if (base_pread(dmeta, dsz, doff) != (ssize_t)dsz || base_pread(fmeta, fsz, foff) != (ssize_t)fsz) return;
    int cap = 4096;
    rom_files = malloc(sizeof(RomFile) * cap);
    for (uint64_t o = 0; o + 0x20 <= fsz;) {
        uint32_t parent, namelen;
        uint64_t data, size;
        memcpy(&parent, fmeta + o, 4);
        memcpy(&data, fmeta + o + 8, 8);
        memcpy(&size, fmeta + o + 0x10, 8);
        memcpy(&namelen, fmeta + o + 0x1C, 4);
        char dir[1024], name[256];
        rom_dir_path(dmeta, (uint32_t)dsz, parent, dir, sizeof dir);
        uint32_t nl = namelen > 255 ? 255 : namelen;
        memcpy(name, fmeta + o + 0x20, nl);
        name[nl] = 0;
        if (rom_nfiles == cap) { cap *= 2; rom_files = realloc(rom_files, sizeof(RomFile) * cap); }
        char *path = malloc(strlen(dir) + nl + 1);
        strcpy(path, dir);
        strcat(path, name);
        rom_files[rom_nfiles++] = (RomFile){rom_data_off + data, size, path, (uint32_t)o};
        o += 0x20 + ((namelen + 3) & ~3u);
    }
    // Randomizer seed: replacement files are served from the seed folder. The file-meta table is patched
    // (in a copy) to point them at data appended past the end of the image; reads there go to host files.
    // The Brazilian Portuguese fan translation (settings_translation_dir) the same way, under a seed's own files.
    const char *tr = shared_title_las() ? settings_translation_dir() : NULL;
    int ntr = 0;
    for (int i = 0; i < rom_nfiles; i++) {
        char host[1200];
        struct stat sb;
        if (!rando_romfs_override(rom_files[i].path, host, sizeof host)) {
            if (!tr) continue;
            snprintf(host, sizeof host, "%s/%s", tr, rom_files[i].path);
            if (stat(host, &sb) || !S_ISREG(sb.st_mode)) continue;
            ntr++;
        }
        if (stat(host, &sb)) continue;
        if (!ov_meta) {
            ov_meta = malloc(fsz);
            memcpy(ov_meta, fmeta, fsz);
            ov_meta_off = foff;
            ov_meta_size = fsz;
            ov_end = (base_size() + 0xF) & ~0xFull;
        }
        uint64_t rel = ov_end - rom_data_off, size = (uint64_t)sb.st_size;
        memcpy(ov_meta + rom_files[i].meta + 8, &rel, 8);
        memcpy(ov_meta + rom_files[i].meta + 0x10, &size, 8);
        if (ov_n == ov_cap) { ov_cap = ov_cap ? ov_cap * 2 : 256; ov = realloc(ov, sizeof *ov * ov_cap); }
        ov[ov_n].start = ov_end;
        ov[ov_n].size = size;
        ov[ov_n].path = strdup(host);
        ov[ov_n].warned = false;
        ov[ov_n].mem = NULL;
        ov_n++;
        ov_end = (ov_end + size + 0xF) & ~0xFull;
    }
    if (ov_n - ntr) LOG("[fs] randomizer: %d romfs files replaced\n", ov_n - ntr);
    if (tr) LOG("[fs] translation %s: %d romfs files replaced\n", tr, ntr);
    if (ov_meta) { patches[npatches++] = (Patch){ov_meta_off, ov_meta_size, ov_meta}; }
    rt_romfs_files = rom_nfiles;  // (the game's own: before a seed's added files; the crash dialog checks it)
    add_new_files(h, dmeta, dsz, fsz);
    qsort(rom_files, rom_nfiles, sizeof *rom_files, rom_cmp);
    free(dmeta);
    free(fmeta);
    LOG("[fs] romfs index: %d files\n", rom_nfiles);
}

// Per-process initialization: selected title state is never reset or switched in place.
static pthread_once_t romfs_once = PTHREAD_ONCE_INIT;
static void initialize_romfs(void) { base_open(); rom_index(); }

// A movie reader owns its duplicate descriptor. Index offsets refer to the raw
// image, while extracted-directory movies are opened directly by the caller.
bool romfs_image_file(const char *path, int *fd, uint64_t *off, uint64_t *size) {
    if (!path || !fd || !off || !size) return false;
    *fd = -1; *off = *size = 0;
    pthread_once(&romfs_once, initialize_romfs);
    if (g_base_fd < 0) return false;
    while (*path == '/') ++path;
    struct stat st;
    if (fstat(g_base_fd, &st) || st.st_size < 0) return false;
    for (int i = 0; i < rom_nfiles; ++i) {
        const RomFile *file = &rom_files[i];
        if (strcmp(file->path, path)) continue;
        if (file->off > (uint64_t)st.st_size || file->size > (uint64_t)st.st_size - file->off) return false;
        int copy = dup(g_base_fd);
        if (copy < 0) return false;
        *fd = copy; *off = file->off; *size = file->size;
        return true;
    }
    return false;
}

// AVIO owns this clone, including its compression metadata and block cache;
// the global filesystem mount can close after the movie obtains its reader.
bool romfs_package_file(const char *path, NxRomfs **reader, uint64_t *off, uint64_t *size) {
    if (!path || !reader || !off || !size) return false;
    *reader = NULL; *off = *size = 0;
    pthread_once(&romfs_once, initialize_romfs);
    if (!g_base_package) return false;
    while (*path == '/') ++path;
    uint64_t total = nx_romfs_size(g_base_package);
    for (int i = 0; i < rom_nfiles; i++) {
        const RomFile *file = &rom_files[i];
        if (strcmp(file->path, path)) continue;
        if (file->off > total || file->size > total - file->off)
            FATAL("[fs] movie file exceeds the selected package RomFS: %s\n", path);
        char why[256] = "";
        NxRomfs *copy = nx_romfs_clone(g_base_package, why, sizeof why);
        if (!copy) FATAL("[fs] cannot clone the selected package movie reader: %s\n", why);
        *reader = copy; *off = file->off; *size = file->size;
        return true;
    }
    return false;
}

// Virtual RomFS image read: original bytes, with the patched file table and appended replacement files.
static ssize_t romfs_read(uint8_t *dst, uint64_t size, uint64_t off) {
    if (!ov_n) return base_pread(dst, size, off);
    uint64_t done = 0;
    while (done < size) {
        uint64_t o = off + done, n = size - done;
        if (o >= ov[0].start) {  // appended region
            int i = 0;
            while (i < ov_n && !(o >= ov[i].start && o < ov[i].start + ((ov[i].size + 0xF) & ~0xFull))) i++;
            if (i == ov_n) { memset(dst + done, 0, n); done += n; break; }
            uint64_t in = o - ov[i].start, avail = ((ov[i].size + 0xF) & ~0xFull) - in;
            if (n > avail) n = avail;
            ssize_t r;
            if (ov[i].mem) {  // in-memory blob (the extended file table)
                r = in < ov[i].size ? (ssize_t)(n < ov[i].size - in ? n : ov[i].size - in) : 0;
                memcpy(dst + done, ov[i].mem + in, (size_t)r);
            } else {
                uint64_t want = in < ov[i].size ? (n < ov[i].size - in ? n : ov[i].size - in) : 0;
                r = 0;
                if (want) {
                    pthread_mutex_lock(&g_ov_lock);
                    int fd = ov_file_fd(i);
                    r = fd < 0 ? -1 : guest_pread(fd, dst + done, want, (off_t)in);
                    if (fd >= 0 && r != (ssize_t)want && !ov[i].warned) {
                        LOG("[fs] ERROR: short read of randomizer file %s at %llu (%lld of %llu bytes, %s): zeros "
                            "there\n", ov[i].path, (unsigned long long)in, (long long)r, (unsigned long long)want,
                            r < 0 ? strerror(errno) : "file changed?");
                        ov[i].warned = true;
                    }
                    pthread_mutex_unlock(&g_ov_lock);
                }
                extern void enemy_random_filter(const char *host, uint64_t off, uint8_t *dst, uint64_t n);
                if (r > 0) enemy_random_filter(ov[i].path, in, dst + done, (uint64_t)r);  // (enemy mode Random)
            }
            if (r < 0) r = 0;
            if ((uint64_t)r < n) memset(dst + done + r, 0, n - r);
        } else {
            if (o + n > ov[0].start) n = ov[0].start - o;
            ssize_t r = base_pread(dst + done, n, o);
            if (r < 0) r = 0;
            if ((uint64_t)r < n) memset(dst + done + r, 0, n - r);
            // overlay the patched tables (file meta, and with added files: header, dir meta, file hash)
            for (int k = 0; k < npatches; k++) {
                uint64_t a = o > patches[k].off ? o : patches[k].off;
                uint64_t b = o + n < patches[k].off + patches[k].size ? o + n : patches[k].off + patches[k].size;
                if (a < b) memcpy(dst + done + (a - o), patches[k].data + (a - patches[k].off), b - a);
            }
        }
        done += n;
    }
    return (ssize_t)done;
}

// Path of the RomFS file containing image offset `off` (NULL if none).
const char *romfs_file_at(uint64_t off) {
    int lo = 0, hi = rom_nfiles - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (off < rom_files[mid].off) hi = mid - 1;
        else if (off >= rom_files[mid].off + rom_files[mid].size) lo = mid + 1;
        else return rom_files[mid].path;
    }
    return NULL;
}

void romfs_file_read_hook(const char *path);

// ------------------------------------------------------------------ IStorage (raw RomFS image)
typedef struct { uint64_t size; } Storage;

static void storage(Service *s, IpcReq *req, IpcResp *resp) {
    Storage *st = s->state;
    switch (req->cmd) {
    case 0: {  // Read(offset, size) -> B buffer
        uint64_t off = IN(uint64_t, 0), size = IN(uint64_t, 8);
        IpcBuf *b = out_buf(req, 0);
        if (!b) { resp->result = MAKE_RESULT(2, 6001); break; }
        if (size > b->size) size = b->size;
        ssize_t n = romfs_read((uint8_t *)(uintptr_t)b->addr, size, off);
        if (n < 0) LOG("[fs] storage read failed at %llx: %s\n", off, strerror(errno));
        {
            const char *path = romfs_file_at(off);
            static const char *last;
            if (path && path != last) {
                last = path;
                if (getenv("LARECOMP_LOG_FILES")) LOG("[fs] read %s\n", path);
                romfs_file_read_hook(path);
            }
        }
        static uint64_t reads, bytes;
        reads++;
        bytes += size;
        if ((reads & (reads - 1)) == 0) LOG("[fs] romfs reads: %llu (%llu KB)\n", reads, bytes >> 10);
        break;
    }
    case 4: OUT(uint64_t, 0) = ov_n ? ov_end : st->size; resp_size(resp, 8); break;  // GetSize
    default: LOG("[fs] IStorage cmd %u (ok)\n", req->cmd);
    }
}

static Service *open_romfs_storage(void) {
    pthread_once(&romfs_once, initialize_romfs);
    Storage *st = calloc(1, sizeof *st);
    st->size = base_size();
    return service_new("IStorage(romfs)", storage, st);
}

// ------------------------------------------------------------------ host-directory filesystem
// Save data is all-or-nothing, as on the Switch (its save storage commits a mount's changes at once): a file opened
// for writing on a save mount works on a copy, <file>.pending, which the mount's Commit syncs and renames over the
// file - so a crash or power loss mid-save leaves the last committed save, never a half-written one (the save
// backups PR #55 was closed in favour of this). Reads see the pending copy, as the Switch's do before a commit. A
// copy left by a crash is dropped at the next mount (uncommitted, as on the Switch). The SD card mount writes in place.
// A save that can't be written (save folder not writable, disk full, ...) is dropped whole and the committed save
// stays as it was, but the game is never told: its SDK aborts on any failed OpenFile / Write / Flush / Commit (the
// crash dialog), and it has no "could not save" path anyway. The player gets one warning (save_failed) instead.
#define PENDING_MAX 16
typedef struct {
    char root[1024];
    bool journaled;
    int npending;
    char pending[PENDING_MAX][1100];
    bool failed[PENDING_MAX];  // a write to this working copy failed: dropped at the Commit, never published
    bool live;                 // a journaled mount the game hasn't closed yet (g_mount_lock)
} HostFs;
// The game mounts the save folder for each save. A new mount drops the working copies a crash left behind - but
// never while another mount of the same folder is still open: those could be its copies, mid-save.
static pthread_mutex_t g_mount_lock = PTHREAD_MUTEX_INITIALIZER;
#define LIVE_MOUNTS_MAX 16
static HostFs *g_live_mounts[LIVE_MOUNTS_MAX];
static void mounts_reset(void) {
    pthread_mutex_lock(&g_mount_lock);
    memset(g_live_mounts, 0, sizeof g_live_mounts);
    pthread_mutex_unlock(&g_mount_lock);
}
typedef struct {
    int fd;
    char path[1100];
    HostFs *fs;    // a save file opened for writing (journaled mount), else NULL
    int slot;      // its working copy in fs->pending (-1: a scratch copy)
    bool scratch;  // staging failed: the writes go to a temporary file outside the save folder, never committed
    bool dropped;  // no scratch copy either: the writes are dropped (fd: the committed file read-only, or -1)
} HostFile;

// Saving failed: say so loudly in the log, and once per run in a dialog (the game itself still says "Game saved").
static void save_failed(HostFs *fs, const char *what, const char *path, int error) {
    LOG("[save] SAVE FAILED: %s %s (%s) - not saved, the last committed save is kept\n", what, path,
        error ? strerror(error) : "earlier error");
    static bool told;
    if (told) return;
    told = true;
    char text[1400];
    snprintf(text, sizeof text, "Your game could not be saved (the game may still say it was). Your previous save "
             "is unchanged.\n\nMake sure this folder can be written to and the disk has free space:\n%s", fs->root);
    plat_notice(text);
}

static void sync_dir_of(const char *path) {
    char d[1100];
    snprintf(d, sizeof d, "%s", path);
    char *slash = strrchr(d, '/');
    if (!slash) return;
    *slash = 0;
#ifndef _WIN32
    int fd = open(d, O_RDONLY);
    if (fd >= 0) { fsync(fd); close(fd); }
#endif
}

// the working copy over the file in one step (Windows' rename doesn't replace an existing file; with the UTF-8
// code page the A function takes our paths as they are)
static int replace_file(const char *from, const char *to) {
#ifdef _WIN32
    if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 0;
    LOG("[save] replace %s failed (Windows error %lu)\n", to, (unsigned long)GetLastError());
    errno = EIO;
    return -1;
#else
    return rename(from, to);
#endif
}

// in -> out from the start (errno set on failure)
static bool copy_fd(int in, int out) {
    char buf[65536];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0)
        for (ssize_t done = 0; done < n;) {
            ssize_t w = write(out, buf + done, (size_t)(n - done));
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) { if (!w) errno = EIO; return false; }
            done += w;
        }
    return n == 0;
}

static bool copy_file(const char *from, const char *to) {
    int in = open(from, O_RDONLY);
    if (in < 0) return false;
    int out = open(to, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    bool ok = out >= 0 && copy_fd(in, out);
    int error = errno;
    close(in);
    if (out >= 0 && close(out)) { error = errno; ok = false; }
    errno = error;
    return ok;
}

// A copy of the save file outside the save folder, deleted when closed: what the game writes when its working copy
// can't be made. Writing over the committed file instead would put the only copy at risk (a crash, a full disk or a
// failed write mid-save leaves it half-written, and a rando slot's flags sidecar would no longer match it).
static int scratch_copy(const char *from) {
    int in = open(from, O_RDONLY);
    if (in < 0) return -1;
#ifdef _WIN32
    char dir[MAX_PATH], name[MAX_PATH];
    int fd = -1;
    if (GetTempPathA(sizeof dir, dir) && GetTempFileNameA(dir, "las", 0, name))
        fd = _open(name, _O_RDWR | _O_BINARY | _O_TEMPORARY);
#else
    char name[1100];
    const char *dir = getenv("TMPDIR");
    snprintf(name, sizeof name, "%s/lasrecomp-save-XXXXXX", dir && *dir ? dir : "/tmp");
    int fd = mkstemp(name);
    if (fd >= 0) unlink(name);
#endif
    if (fd >= 0 && !copy_fd(in, fd)) { close(fd); fd = -1; }
    close(in);
    return fd;
}

// the working copy of a save file (made on its first open for writing): its index in fs->pending; -1 for the SD
// card mount, a file not being saved, or (errno set) when the copy can't be made
static int pending_of(HostFs *fs, const char *p, bool create, char *out, size_t n) {
    if (!fs->journaled) return -1;
    snprintf(out, n, "%s.pending", p);
    for (int i = 0; i < fs->npending; i++) if (!strcmp(fs->pending[i], p)) return i;
    if (!create) return -1;
    if (fs->npending == PENDING_MAX) { errno = ENOSPC; return -1; }
    if (strlen(p) >= sizeof fs->pending[0]) { errno = ENAMETOOLONG; return -1; }
    if (!copy_file(p, out)) { int error = errno; unlink(out); errno = error; return -1; }
    fs->failed[fs->npending] = false;
    snprintf(fs->pending[fs->npending], sizeof fs->pending[0], "%s", p);
    return fs->npending++;
}

static bool sync_save_file(const char *path) {
    int fd = open(path, O_RDWR);
    if (fd < 0) return false;
    bool ok = fsync(fd) == 0;
    int error = errno;
    if (close(fd)) return false;
    if (!ok) errno = error;
    return ok;
}
static bool commit_slot(const char *path) {
    char pending[1200], flags[1200], flags_pending[1200];
    snprintf(pending, sizeof pending, "%s.pending", path);
    snprintf(flags, sizeof flags, "%s.flagsx", path);
    snprintf(flags_pending, sizeof flags_pending, "%s.flagsx.pending", path);
    struct stat st;
    bool has_flags = stat(flags_pending, &st) == 0;
    if (!has_flags && errno != ENOENT) return false;
    if (has_flags && !sync_save_file(flags_pending)) return false;
    if (!stat(pending, &st)) {
        if (!sync_save_file(pending) || replace_file(pending, path)) return false;
        sync_dir_of(path);
    } else if (errno != ENOENT || !has_flags) return false;
    // A missing working copy with pending flags means the data rename already succeeded. Finish that pair
    // on retry; never discard its flags or let a fresh mount read the new data with the old flags.
    if (has_flags) {
        if (replace_file(flags_pending, flags)) return false;
        sync_dir_of(flags);
    }
    return true;
}
// a save that won't be committed: its working copies go (the committed file and sidecar stay as they are)
static void drop_slot(const char *path) {
    char pending[1200], flags_pending[1250];
    snprintf(pending, sizeof pending, "%s.pending", path);
    snprintf(flags_pending, sizeof flags_pending, "%s.flagsx.pending", path);
    unlink(pending);
    unlink(flags_pending);
}
// Commit always succeeds for the game (see save_failed). A save that failed is dropped whole. If only the sidecar's
// rename failed (the data is already replaced), its working copy stays for the next mount to finish the pair.
static void commit(HostFs *fs) {
    extern void pace_hitch(const char *why);  // nvn/pace.c
    if (fs->npending) pace_hitch("a save commit");
    for (int i = 0; i < fs->npending; i++) {
        const char *path = fs->pending[i];
        if (fs->failed[i]) {
            drop_slot(path);
            save_failed(fs, "dropped the failed save", path, 0);
        } else if (!commit_slot(path)) {
            int error = errno;
            char pending[1200];
            struct stat st;
            snprintf(pending, sizeof pending, "%s.pending", path);
            if (!stat(pending, &st)) {
                drop_slot(path);
                save_failed(fs, "could not commit", path, error);
            } else LOG("[save] committed %s, but not its flags sidecar yet (%s): the next mount finishes it\n", path,
                       strerror(error));
        } else LOG("[save] committed %s\n", path);
    }
    fs->npending = 0;
}
// Recover pairs before dropping abandoned working copies: deleting the data copy first would make an
// uncommitted sidecar look committed. No format change: existing .flagsx files still contain just flag bytes.
static bool recover_save_flags(const char *root) {
    DIR *d = opendir(root);
    if (!d) return false;
    struct dirent *e;
    int error = 0;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n <= 15 || strcmp(e->d_name + n - 15, ".flagsx.pending")) continue;
        char path[1200], pending[1250], flags_pending[1250];
        snprintf(path, sizeof path, "%s/%.*s", root, (int)(n - 15), e->d_name);
        snprintf(pending, sizeof pending, "%s.pending", path);
        snprintf(flags_pending, sizeof flags_pending, "%s.flagsx.pending", path);
        struct stat st;
        if (!stat(pending, &st)) {
            if (unlink(flags_pending)) { error = errno; break; }
        } else if (errno != ENOENT || !commit_slot(path)) { error = errno; break; }
    }
    closedir(d);
    if (error) errno = error;
    return !error;
}
typedef struct { DIR *d; char path[1024]; int mode; } HostDir;

// The game closed the mount. (Its HostFs stays allocated: a file opened on it may still point at it.)
static void fs_unmount(Service *s) {
    HostFs *fs = s->state;
    pthread_mutex_lock(&g_mount_lock);
    for (int i = 0; fs && i < LIVE_MOUNTS_MAX; i++) if (g_live_mounts[i] == fs) g_live_mounts[i] = NULL;
    if (fs) fs->live = false;
    pthread_mutex_unlock(&g_mount_lock);
}

// The game closed the file / directory: close the host one too. They used to stay open for the whole run - a leak
// everywhere, and on Windows the open working copy blocked the Commit's replace, so saves never landed (a file can't
// be replaced while it's open there; the game closes its files before it commits, as nn::fs requires).
static void file_close(Service *s) {
    HostFile *f = s->state;
    if (f) { if (f->fd >= 0) close(f->fd); free(f); s->state = NULL; }
}
static void dir_close(Service *s) {
    HostDir *d = s->state;
    if (d) { if (d->d) closedir(d->d); free(d); s->state = NULL; }
}

static const char *in_path(IpcReq *req, int i) {
    if (i < req->nx) return (const char *)(uintptr_t)req->x[i].addr;
    if (i < req->na) return (const char *)(uintptr_t)req->a[i].addr;
    return "";
}

static void host_path(HostFs *fs, const char *p, char *out, size_t n) {
    if (!shared_guest_path(fs->root, p, out, n)) FATAL("guest filesystem path escapes its mount or exceeds its capacity\n");
}

static uint32_t errno_result(void) {
    if (errno == ENOENT || errno == ENOTDIR) return FS_PATH_NOT_FOUND;
    if (errno == EEXIST) return FS_PATH_EXISTS;
    if (errno == ENOTEMPTY) return FS_NOT_EMPTY;
    return MAKE_RESULT(2, 4000);
}

// a save file's write / flush / resize failed: that save is dropped at the Commit (the game is told it worked)
static void file_failed(HostFile *f, const char *what, int error) {
    if (f->slot >= 0) f->fs->failed[f->slot] = true;
    save_failed(f->fs, what, f->path, error);
}

static void file(Service *s, IpcReq *req, IpcResp *resp) {
    HostFile *f = s->state;
    switch (req->cmd) {
    case 0: {  // Read(option, offset, size) -> B
        int64_t off = IN(int64_t, 8), size = IN(int64_t, 16);
        IpcBuf *b = out_buf(req, 0);
        if (!b || off < 0 || size < 0) { resp->result = MAKE_RESULT(2, 6001); break; }
        if ((uint64_t)size > b->size) size = (int64_t)b->size;
        if (f->dropped && f->fd < 0) { OUT(int64_t, 0) = 0; resp_size(resp, 8); break; }  // (nothing open: empty)
        ssize_t n = guest_pread(f->fd, (void *)(uintptr_t)b->addr, (size_t)size, off);
        if (n < 0) resp->result = errno_result();
        if (shared_title_las() && n > 0 && !f->scratch && strstr(f->path, "savedata")) {
            LOG("[save] read %s off %lld size %lld -> guest %llx\n", f->path, (long long)off, (long long)size, (unsigned long long)b->addr);
            extern void save_editor_file_read(const char *path, uint64_t addr, int64_t off, int64_t size);
            save_editor_file_read(f->path, b->addr, off, n);
            extern void flags_ext_save_read(const char *path, uint64_t addr, int64_t off, int64_t size);
            flags_ext_save_read(f->path, b->addr, off, n);
        }
        OUT(int64_t, 0) = n < 0 ? 0 : n;
        resp_size(resp, 8);
        break;
    }
    case 1: {  // Write(option, offset, size) <- A
        int64_t off = IN(int64_t, 8), size = IN(int64_t, 16);
        IpcBuf *a = in_buf(req, 0);
        if (!a || off < 0 || size < 0 || (uint64_t)size > a->size ||
            off > INT64_MAX - size ||
            (size && !session_range_contains(shared_required_layout()->window, a->addr, (uint64_t)size))) {
            resp->result = MAKE_RESULT(2, 6001); break;
        }
        if (strstr(f->path, "savedata"))
            LOG("[save] write %s off %lld size %lld <- guest %llx\n", f->path, (long long)off, (long long)size, (unsigned long long)a->addr);
        if (f->dropped) break;  // (reported when it was opened; the game is never told, see save_failed)
        int error = 0;
        for (int64_t done = 0; done < size;) {  // (a short write goes on from where it stopped)
            ssize_t n = pwrite(f->fd, (void *)(uintptr_t)(a->addr + done), (size_t)(size - done), off + done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { error = n ? errno : EIO; break; }
            done += n;
        }
        if (shared_title_las() && !error && !f->scratch && strstr(f->path, "savedata")) {
            extern bool flags_ext_save_written(const char *path, uint64_t addr, int64_t off, int64_t size);
            errno = 0;
            if (!flags_ext_save_written(f->path, a->addr, off, size)) error = errno ? errno : EIO;
        }
        if (error && f->fs) file_failed(f, "write", error);
        else if (error) { errno = error; resp->result = errno_result(); }
        break;
    }
    case 2:  // Flush
        if (f->dropped) break;
        if (fsync(f->fd) && f->fs) file_failed(f, "flush", errno);
        break;
    case 3:  // SetSize
        if (f->dropped) break;
        if (!ftruncate(f->fd, IN(int64_t, 0))) break;
        if (f->fs) file_failed(f, "resize", errno);
        else resp->result = errno_result();
        break;
    case 4: {
        struct stat sb;
        OUT(int64_t, 0) = f->fd >= 0 && !fstat(f->fd, &sb) ? sb.st_size : 0;
        resp_size(resp, 8);
        break;
    }
    default: LOG("[fs] IFile cmd %u (ok)\n", req->cmd);
    }
}

static void directory(Service *s, IpcReq *req, IpcResp *resp) {
    HostDir *d = s->state;
    switch (req->cmd) {
    case 0: {  // Read -> B buffer of 0x310-byte entries
        IpcBuf *b = out_buf(req, 0);
        uint8_t *out = (uint8_t *)(uintptr_t)b->addr;
        int64_t max = (int64_t)(b->size / 0x310), n = 0;
        struct dirent *e;
        while (n < max && (e = readdir(d->d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
#ifdef _WIN32  // no d_type
            char full[2048];
            struct stat es;
            snprintf(full, sizeof full, "%s/%s", d->path, e->d_name);
            bool isdir = !stat(full, &es) && S_ISDIR(es.st_mode);
#else
            bool isdir = e->d_type == DT_DIR;
#endif
            if ((isdir && !(d->mode & 1)) || (!isdir && !(d->mode & 2))) continue;
            uint8_t *ent = out + n * 0x310;
            memset(ent, 0, 0x310);
            strncpy((char *)ent, e->d_name, 0x300);
            ent[0x304] = isdir ? 0 : 1;
            if (!isdir) {
                char p[2048];
                struct stat sb;
                snprintf(p, sizeof p, "%s/%s", d->path, e->d_name);
                if (!stat(p, &sb)) *(int64_t *)(ent + 0x308) = sb.st_size;
            }
            n++;
        }
        OUT(int64_t, 0) = n;
        resp_size(resp, 8);
        break;
    }
    case 1: {  // GetEntryCount
        long pos = telldir(d->d);
        rewinddir(d->d);
        int64_t n = 0;
        struct dirent *e;
        while ((e = readdir(d->d)))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n++;
        seekdir(d->d, pos);
        OUT(int64_t, 0) = n;
        resp_size(resp, 8);
        break;
    }
    }
}

static int rm_rf(const char *p, bool keep_root) {
    DIR *d = opendir(p);
    if (!d) return unlink(p);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[2048];
        snprintf(c, sizeof c, "%s/%s", p, e->d_name);
        rm_rf(c, false);
    }
    closedir(d);
    return keep_root ? 0 : rmdir(p);
}

static void filesystem(Service *s, IpcReq *req, IpcResp *resp) {
    HostFs *fs = s->state;
    char p[2048], p2[2048];
    host_path(fs, in_path(req, 0), p, sizeof p);
    switch (req->cmd) {
    case 0: {  // CreateFile(option, size)
        int fd = open(p, O_CREAT | O_EXCL | O_RDWR, 0644);
        if (fd < 0) { resp->result = errno_result(); break; }
        if (ftruncate(fd, IN(int64_t, 8))) resp->result = errno_result();
        close(fd);
        break;
    }
    case 1: if (unlink(p)) resp->result = errno_result(); break;
    case 2: if (mkdir(p, 0755)) resp->result = errno_result(); break;
    case 3: if (rmdir(p)) resp->result = errno_result(); break;
    case 4: if (rm_rf(p, false)) resp->result = errno_result(); break;
    case 5: case 6:
        host_path(fs, in_path(req, 1), p2, sizeof p2);
        if (rename(p, p2)) resp->result = errno_result();
        break;
    case 7: {  // GetEntryType
        struct stat sb;
        if (stat(p, &sb)) { resp->result = FS_PATH_NOT_FOUND; break; }
        OUT(uint32_t, 0) = S_ISDIR(sb.st_mode) ? 0 : 1;
        resp_size(resp, 4);
        break;
    }
    case 8: {  // OpenFile(mode)
        uint32_t mode = IN(uint32_t, 0);
        bool save = fs->journaled && (mode & 2);
        char work[2100];
        struct stat sb;
        int slot = -1, fd = -1;
        bool scratch = false, dropped = false;
        bool exists = !stat(p, &sb);
        if (exists) slot = pending_of(fs, p, save, work, sizeof work);
        if (slot >= 0) {
            snprintf(p, sizeof p, "%s", work);
        } else if (save && exists) {  // (its working copy can't be made)
            save_failed(fs, "could not stage", p, errno);
            fd = scratch_copy(p);
            scratch = true;
            if (fd < 0) {  // no scratch copy either: the writes are dropped (never sent to a read-only fd)
                save_failed(fs, "could not make a scratch copy of", p, errno);
                dropped = true;
                fd = open(p, O_RDONLY);  // (reads still see the committed file; -1 if even that can't be opened)
            }
        }
        if (fd < 0 && !scratch) fd = open(p, (mode & 2) ? O_RDWR : O_RDONLY);
        if (fd < 0 && !dropped) { resp->result = errno_result(); break; }
        HostFile *f = calloc(1, sizeof *f);
        f->fd = fd;
        snprintf(f->path, sizeof f->path, "%s%s", p, dropped ? " (writes dropped)" : scratch ? " (scratch copy)" : "");
        if (save) { f->fs = fs; f->slot = scratch ? -1 : slot; f->scratch = scratch; f->dropped = dropped; }
        Service *fsvc = service_new("IFile", file, f);
        fsvc->on_close = file_close;
        ipc_return_object(resp, fsvc);
        break;
    }
    case 9: {  // OpenDirectory(mode)
        DIR *d = opendir(p);
        if (!d) { resp->result = errno_result(); break; }
        HostDir *hd = calloc(1, sizeof *hd);
        hd->d = d;
        hd->mode = (int)IN(uint32_t, 0);
        snprintf(hd->path, sizeof hd->path, "%s", p);
        Service *dsvc = service_new("IDirectory", directory, hd);
        dsvc->on_close = dir_close;
        ipc_return_object(resp, dsvc);
        break;
    }
    case 10: commit(fs); break;  // Commit (always succeeds for the game: see save_failed)
    case 11: case 12: OUT(int64_t, 0) = 0x40000000; resp_size(resp, 8); break;
    case 13: rm_rf(p, true); break;
    case 14: memset(resp->data, 0, 0x20); resp_size(resp, 0x20); break;
    default: LOG("[fs] IFileSystem cmd %u (ok)\n", req->cmd);
    }
}

static Service *open_host_fs(const char *sub) {
    HostFs *fs = calloc(1, sizeof *fs);
    if (getenv("LARECOMP_SAVE_DIR")) g_save_dir = getenv("LARECOMP_SAVE_DIR");  // test runs use a scratch copy
    else if (shared_title_las() && rando_seed_dir()) {  // each randomizer seed has its own save slots
        static char seed_save[600];
        snprintf(seed_save, sizeof seed_save, "%s/save", rando_seed_dir());
        g_save_dir = seed_save;
    }
    snprintf(fs->root, sizeof fs->root, "%s/%s", g_save_dir, sub);
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "%s", fs->root);
    for (char *q = cmd + 1; *q; q++)
        if (*q == '/') { *q = 0; mkdir(cmd, 0755); *q = '/'; }
    mkdir(cmd, 0755);
    fs->journaled = strcmp(sub, "sdcard") != 0;
    pthread_mutex_lock(&g_mount_lock);
    bool other_live = false;
    for (int i = 0; i < LIVE_MOUNTS_MAX; i++)
        if (g_live_mounts[i] && !strcmp(g_live_mounts[i]->root, fs->root)) other_live = true;
    if (fs->journaled && other_live) LOG("[save] %s is still mounted: its working copies are left alone\n", fs->root);
    if (fs->journaled && !other_live) {  // (a working copy left by a crash: never committed - dropped, as on the Switch)
        if (!recover_save_flags(fs->root)) {
            int error = errno;
            pthread_mutex_unlock(&g_mount_lock);
            free(fs);
            errno = error;
            return NULL;
        }
        DIR *d = opendir(fs->root);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            size_t l = strlen(e->d_name);
            if (l > 8 && !strcmp(e->d_name + l - 8, ".pending")) {
                char stale[1300];
                snprintf(stale, sizeof stale, "%s/%s", fs->root, e->d_name);
                LOG("[save] dropping uncommitted %s\n", stale);
                unlink(stale);
            }
        }
        if (d) closedir(d);
    }
    for (int i = 0; fs->journaled && i < LIVE_MOUNTS_MAX; i++)
        if (!g_live_mounts[i]) { g_live_mounts[i] = fs, fs->live = true; break; }
    pthread_mutex_unlock(&g_mount_lock);
    LOG("[fs] mounted host directory %s\n", fs->root);
    Service *svc = service_new("IFileSystem", filesystem, fs);
    svc->on_close = fs_unmount;
    return svc;
}

// ------------------------------------------------------------------ fsp-srv
SERVICE("fsp-srv", fsp_srv) {
    (void)s;
    switch (req->cmd) {
    case 1: break;  // SetCurrentProcess
    case 18: ipc_return_object(resp, open_host_fs("sdcard")); break;
    case 22: break;  // CreateSaveDataFileSystem
    case 51: case 53: {  // OpenSaveDataFileSystem(space, attribute)
        uint8_t type = IN(uint8_t, 0x8 + 0x18);  // attribute.type
        char sub[64];
        snprintf(sub, sizeof sub, "save_%u", type);
        Service *mount = open_host_fs(sub);
        if (mount) ipc_return_object(resp, mount);
        else resp->result = errno_result();
        break;
    }
    case 200: ipc_return_object(resp, open_romfs_storage()); break;  // OpenDataStorageByCurrentProcess
    case 202:  // OpenDataStorageByDataId (system data archives)
        LOG("[fs] OpenDataStorageByDataId %llx (not available)\n", IN(uint64_t, 8));
        resp->result = MAKE_RESULT(2, 1001);
        break;
    case 1005: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;  // GetGlobalAccessLogMode
    case 1003: case 1004: case 1006: break;
    case 1011: memset(resp->data, 0, 0x10); resp_size(resp, 0x10); break;
    default: LOG("[fs] fsp-srv cmd %u (ok)\n", req->cmd);
    }
}
