// Data root and shipped resources (runtime/paths.h).
#include <SDL3/SDL.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#include <sys/utime.h>
#else
#include <sys/statvfs.h>
#include <utime.h>
#endif

#include "gamelib.h"
#include "paths.h"
#include "platform.h"

static char g_root[1024], g_start[1024], g_exe_dir[1024];

#ifdef _WIN32
#define SEP "\\"
static bool is_link(const char *p) {  // symlink or junction
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}
// The final path, junctions and symlinks followed (GetFinalPathNameByHandle); _fullpath (lexical, as before) only
// where the file system can't say
static bool resolve(const char *p, char *out, size_t n) {
    wchar_t w[1100], f[1100];
    if (MultiByteToWideChar(CP_UTF8, 0, p, -1, w, 1100) > 0) {
        HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS, NULL);  // (BACKUP_SEMANTICS: folders too)
        if (h != INVALID_HANDLE_VALUE) {
            DWORD k = GetFinalPathNameByHandleW(h, f, 1100, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            CloseHandle(h);
            if (k && k < 1100) {
                const wchar_t *s = f;
                if (!wcsncmp(f, L"\\\\?\\UNC\\", 8)) s = f + 6, f[6] = L'\\';  // \\?\UNC\srv\share -> \\srv\share
                else if (!wcsncmp(f, L"\\\\?\\", 4)) s = f + 4;                    // \\?\C:\... -> C:\...
                return WideCharToMultiByte(CP_UTF8, 0, s, -1, out, (int)n, NULL, NULL) > 0;
            }
        }
    }
    return _fullpath(out, p, n) != NULL;
}
#define path_eq(a, b) (_stricmp(a, b) == 0)
#define path_prefix(a, b, n) (_strnicmp(a, b, n) == 0)
#else
#define SEP "/"
static bool is_link(const char *p) {
    struct stat st;
    return lstat(p, &st) == 0 && S_ISLNK(st.st_mode);
}
static bool resolve(const char *p, char *out, size_t n) {
    char buf[PATH_MAX];
    if (!realpath(p, buf)) return false;
    snprintf(out, n, "%s", buf);
    return true;
}
#define path_eq(a, b) (strcmp(a, b) == 0)
#define path_prefix(a, b, n) (strncmp(a, b, n) == 0)
#endif

static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static void mkdirs(const char *path) {
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++)
        if (*c == '/' || *c == '\\') { char s = *c; *c = 0; mkdir(p, 0755); *c = s; }
    mkdir(p, 0755);
}

static bool writable(const char *dir) {
    char t[1100];
    snprintf(t, sizeof t, "%s" SEP ".write_test", dir);
    FILE *f = fopen(t, "wb");
    if (!f) return false;
    fclose(f);
    remove(t);
    return true;
}

static void fail(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Raitchi", msg, NULL);
    exit(1);
}

static bool g_dev;  // the data root is the starting folder because it has extracted/ (a source checkout)
bool rt_dev_mode(void) { return g_dev; }

// ---- where the data folder is: the platform's folder, or another one a pointer file there names
#define POINTER_FILE "data_root.txt"            // in the platform's folder: the data folder's path (one line)
#define NOTE_TO ".lasrecomp_move_to"            // in a move's source until it switched: where the copy went
#define NOTE_FROM ".lasrecomp_move_from"        // in a move's destination: the old folder, still to be emptied
#define MARK_PARTIAL ".lasrecomp_incomplete"    // in a move's destination until its copy was checked
// What a data folder holds (and a move takes along). Logs stay where they are.
static const char *const DATA_ENTRIES[] = {"save", "rando", "larecomp.ini", "fonts", "gamecontrollerdb.txt", "cache",
                                           "extracted"};
#define N_ENTRIES (sizeof DATA_ENTRIES / sizeof *DATA_ENTRIES)
static char g_home[1024];  // the platform's folder (where POINTER_FILE lives)

static void exe_dir(char *out, size_t n) {
    out[0] = 0;
    if (!plat_exe_path(out, n)) return;
    char *s = strrchr(out, '/'), *b = strrchr(out, '\\');
    if (b > s) s = b;
    if (s) *s = 0;
}

// The platform's data folder. LARECOMP_DATA_HOME (tests) stands in for it.
static void default_home(char *root, size_t n) {
    const char *t = getenv("LARECOMP_DATA_HOME");
    if (t && *t) { snprintf(root, n, "%s", t); return; }
#ifdef _WIN32
    exe_dir(root, n);
#elif defined(__ANDROID__)
    // Android/data/<package>/files: the app's own without any permission, and reachable from a computer over USB
    // (game files, seeds, saves, the log)
    const char *ext = SDL_GetAndroidExternalStoragePath();
    snprintf(root, n, "%s", ext ? ext : SDL_GetAndroidInternalStoragePath());
#elif defined(__linux__)
    // (the AppImage is read-only): $XDG_DATA_HOME/lasrecomp, ~/.local/share/lasrecomp by default
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (xdg && *xdg == '/') snprintf(root, n, "%s/lasrecomp", xdg);
    else snprintf(root, n, "%s/.local/share/lasrecomp", home && *home ? home : "/tmp");
#else
    const char *home = getenv("HOME");
    snprintf(root, n, "%s/Library/Application Support/" RT_BUNDLE_ID, home && *home ? home : "/tmp");
#endif
}

// A one-line file's line (no line end); false: no file, or empty
static bool read_line(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t k = fread(out, 1, n - 1, f);
    fclose(f);
    out[k] = 0;
    out[strcspn(out, "\r\n")] = 0;
    return out[0] != 0;
}

// Written and flushed to the disk; replaces the file in one step (a cut-off write never leaves half a line)
static bool write_line(const char *path, const char *line) {
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fprintf(f, "%s\n", line) > 0 && fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok &= fclose(f) == 0;
#ifdef _WIN32
    ok = ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    ok = ok && rename(tmp, path) == 0;
#endif
    if (!ok) remove(tmp);
    return ok;
}

static void join(char *out, size_t n, const char *dir, const char *name) { snprintf(out, n, "%s" SEP "%s", dir, name); }

// a and b are the same folder, or one is inside the other (unresolvable: treated as overlapping)
static bool within(const char *child, const char *parent) {
    size_t n = strlen(parent);
    while (n > 1 && (parent[n - 1] == '/' || parent[n - 1] == '\\')) n--;
    return path_prefix(child, parent, n) && (!child[n] || child[n] == '/' || child[n] == '\\');
}
static bool overlap(const char *a, const char *b) {
    char ra[1100], rb[1100];
    if (!resolve(a, ra, sizeof ra) || !resolve(b, rb, sizeof rb)) return true;
    return within(ra, rb) || within(rb, ra);
}
static bool same_dir(const char *a, const char *b) {
    char ra[1100], rb[1100];
    return resolve(a, ra, sizeof ra) && resolve(b, rb, sizeof rb) && path_eq(ra, rb);
}

// Inside the app itself (the .app bundle, the mounted AppImage): never a data folder
static bool in_app(const char *p) {
    char r[1100], app[1100];
    if (!resolve(p, r, sizeof r)) return false;
#ifdef __APPLE__
    char e[1024];
    exe_dir(e, sizeof e);
    char *c = strstr(e, ".app/Contents/");
    if (!c) return false;
    c[4] = 0;
    return resolve(e, app, sizeof app) && within(r, app);
#elif defined(__linux__)
    const char *d = getenv("APPDIR");
    return d && *d && resolve(d, app, sizeof app) && within(r, app);
#else
    (void)app;
    return false;
#endif
}

// The data folder a pointer file names (false: none, or it points into the app)
static bool pointer_root(const char *home, char *out, size_t n) {
    char p[1100];
    join(p, sizeof p, home, POINTER_FILE);
    if (!read_line(p, out, n)) return false;
    if (in_app(out)) {
        fprintf(stderr, "[paths] %s points into the app (%s): ignored\n", p, out);
        return false;
    }
    return true;
}

bool rt_data_root_early(char *out, size_t n) {
    const char *env = getenv("LARECOMP_DATA_ROOT");
    if (env && *env) return (size_t)snprintf(out, n, "%s", env) < n;
    if (is_dir("extracted")) return false;  // (dev mode)
    char home[1024];
    default_home(home, sizeof home);
    if (!pointer_root(home, out, n) || !is_dir(out)) snprintf(out, n, "%s", home);
    return true;
}

// The entries of a data folder (DATA_ENTRIES) removed; links themselves, never what they point to
static bool remove_entries(const char *dir) {
    bool ok = true;
    for (size_t i = 0; i < N_ENTRIES; i++) {
        char p[1100];
        join(p, sizeof p, dir, DATA_ENTRIES[i]);
        if (!is_link(p) && !exists(p)) continue;
        if (is_dir(p) && !is_link(p)) ok &= plat_remove_tree(p);
#ifdef _WIN32
        else if (is_dir(p)) ok &= RemoveDirectoryA(p) != 0;  // (a junction)
#endif
        else ok &= remove(p) == 0;
    }
    return ok;
}

// An unfinished copy (MARK_PARTIAL) in dir removed: what a move had put there
static void remove_partial(const char *dir) {
    char p[1100];
    fprintf(stderr, "[paths] removing an unfinished copy of the data in %s\n", dir);
    remove_entries(dir);
    join(p, sizeof p, dir, NOTE_FROM);
    remove(p);
    join(p, sizeof p, dir, MARK_PARTIAL);
    remove(p);
}

// What a move left to do, from the data folder now in use (root):
//  - it was a move's source, and the move never switched over: the copy there goes
//  - it is a move's destination: the old folder's data goes (only once the switch is made: the pointer names root)
static void finish_moves(const char *root, void (*progress)(int, double)) {
    char note[1100], other[1024], p[1100], back[1024];
    join(note, sizeof note, root, NOTE_TO);
    if (read_line(note, other, sizeof other)) {
        join(p, sizeof p, other, MARK_PARTIAL);
        bool partial = exists(p);
        join(p, sizeof p, other, NOTE_FROM);
        bool ours = partial || (read_line(p, back, sizeof back) && same_dir(back, root));
        if (ours && !overlap(root, other)) remove_partial(other);
        remove(note);
    }
    join(note, sizeof note, root, NOTE_FROM);
    if (read_line(note, other, sizeof other)) {
        if (!overlap(root, other) && is_dir(other)) {
            fprintf(stderr, "[paths] removing the old copy of the data in %s\n", other);
            join(p, sizeof p, other, NOTE_TO);
            remove(p);  // (first: that folder can never undo this move)
            for (size_t i = 0; i < N_ENTRIES; i++) {
                char e[1100];
                join(e, sizeof e, other, DATA_ENTRIES[i]);
                if (is_dir(e) && !is_link(e)) plat_remove_tree(e);
                else remove(e);
                if (progress) progress(2, (double)(i + 1) / N_ENTRIES);
            }
            if (!same_dir(other, g_home)) rmdir(other);  // (only if empty)
        }
        remove(note);
    }
}

#ifdef __ANDROID__
// The files the app ships are APK assets (android/app/src/main/assets), which only SDL's file streams can read: copied
// once into <internal storage>/res, which then stands in for the executable's folder (rt_resource).
static void android_resources(void) {
    static const char *const names[] = {"Baloo2-SemiBold.ttf", "DejaVuSans.ttf", "shared_5.ttf", "icon.png"};
    const char *internal = SDL_GetAndroidInternalStoragePath();
    if (!internal) return;
    snprintf(g_exe_dir, sizeof g_exe_dir, "%s/res", internal);
    mkdirs(g_exe_dir);
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        SDL_IOStream *in = SDL_IOFromFile(names[i], "rb");
        if (!in) continue;
        Sint64 size = SDL_GetIOSize(in);
        char out[1200];
        snprintf(out, sizeof out, "%s/%s", g_exe_dir, names[i]);
        struct stat st;
        if (size > 0 && !(stat(out, &st) == 0 && st.st_size == size)) {
            void *data = SDL_LoadFile_IO(in, NULL, false);
            FILE *f = data ? fopen(out, "wb") : NULL;
            if (f) { fwrite(data, 1, (size_t)size, f); fclose(f); }
            SDL_free(data);
        }
        SDL_CloseIO(in);
    }
}
#endif

void rt_paths_init(void) {
    if (!getcwd(g_start, sizeof g_start)) g_start[0] = 0;
#ifdef __ANDROID__
    android_resources();
#else
    exe_dir(g_exe_dir, sizeof g_exe_dir);
#endif
    default_home(g_home, sizeof g_home);
    char root[1024];
    const char *env = getenv("LARECOMP_DATA_ROOT");
    if (env && *env) snprintf(root, sizeof root, "%s", env);
    else if (is_dir("extracted")) snprintf(root, sizeof root, "%s", g_start), g_dev = true;  // dev / portable: as before
    else {
        snprintf(root, sizeof root, "%s", g_home);
        char ptr[1024];
        while (pointer_root(g_home, ptr, sizeof ptr)) {  // another folder chosen on the start screen
            if (is_dir(ptr) && writable(ptr)) { snprintf(root, sizeof root, "%s", ptr); break; }
            char msg[1400];
            snprintf(msg, sizeof msg, "Raitchi's data folder isn't available:\n%s\n\nIf it's on a drive that isn't "
                     "connected, connect it and try again.", ptr);
            const SDL_MessageBoxButtonData b[] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "Try again"},
                                                  {0, 1, "Use the default folder"},
                                                  {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 2, "Quit"}};
            const SDL_MessageBoxData box = {SDL_MESSAGEBOX_WARNING, NULL, "Raitchi", msg, 3, b, NULL};
            int k = 2;
            if (!SDL_ShowMessageBox(&box, &k)) fprintf(stderr, "%s\n", msg);
            if (k == 2) exit(1);
            if (k == 1) {  // (the data there stays; the start screen can switch back to it)
                char p[1100];
                join(p, sizeof p, g_home, POINTER_FILE);
                remove(p);
            }
        }
    }
    mkdirs(root);
    char msg[1400];
    if (!is_dir(root) || !writable(root) || chdir(root) != 0) {
#ifdef _WIN32
        snprintf(msg, sizeof msg, "Raitchi can't write to its folder:\n%s\n\nIt keeps game files, saves and seeds "
                 "next to the program. Move that folder somewhere you can write to (e.g. Documents or "
                 "Desktop, not Program Files) and start it again.", root);
#else
        snprintf(msg, sizeof msg, "Raitchi can't write to its data folder:\n%s", root);
#endif
        fail(msg);
    }
    if (!getcwd(g_root, sizeof g_root)) snprintf(g_root, sizeof g_root, "%s", root);
    fprintf(stderr, "[paths] data: %s\n", g_root);
    if (!g_dev && !(env && *env)) finish_moves(g_root, NULL);
}

const char *rt_data_root(void) { return g_root; }

bool rt_resource(const char *name, const char *dev_rel, char *out, size_t n) {
    char c[4][1100] = {{0}};
    if (name) {
        snprintf(c[0], sizeof c[0], "%s" SEP "%s", g_exe_dir, name);
        snprintf(c[1], sizeof c[1], "%s" SEP ".." SEP "Resources" SEP "%s", g_exe_dir, name);  // .app/Contents/Resources
    }
    if (dev_rel) {
        snprintf(c[2], sizeof c[2], "%s" SEP "%s", g_start, dev_rel);
        snprintf(c[3], sizeof c[3], "%s" SEP ".." SEP "%s", g_exe_dir, dev_rel);  // build/lasrecomp -> repo root
    }
    for (int i = 0; i < 4; i++)
        if (c[i][0] && exists(c[i])) return (size_t)snprintf(out, n, "%s", c[i]) < n;  // (as is: a venv python must stay a link)
    return false;
}

// ---- game files
// What the setup (runtime/ui/launcher.mm) creates in extracted/, plus what older versions' extraction and manual
// extractions left there (never read at run time).
static const char *const GAME_FILES[] = {"exefs", "romfs", "romfs.bin", GAMELIB_NAME, GAMELIB_NAME ".id", ".gamebuild",
                                         ".zigcache", ".extract", "extract.log", "xci_secure", "nso_raw", "update_pfs",
                                         ".DS_Store"};

// The other data folder the game files may be in (Android: the memory card's; rt_paths_set_game_root)
static char g_game_root[1024];
void rt_paths_set_game_root(const char *root) { snprintf(g_game_root, sizeof g_game_root, "%s", root ? root : ""); }

// <data root>/extracted with links resolved (false: no data root, or it can't be resolved)
static bool game_dir_want(char *want, size_t n) {
    char root[1100];
    if (!g_root[0] || !resolve(g_root, root, sizeof root)) return false;
    return (size_t)snprintf(want, n, "%s" SEP "extracted", root) < n;
}

// game_dir must be <data root>/extracted (or <the game files' data folder>/extracted, rt_paths_set_game_root), a
// real folder (a dev worktree's symlinked extracted/ is refused)
static bool game_dir_ok(const char *game_dir) {
    char want[1100], got[1100];
    if (is_link(game_dir)) {
        fprintf(stderr, "[paths] %s is a link (symlink / junction): its game files are not ours to delete\n", game_dir);
        return false;
    }
    if (!game_dir_want(want, sizeof want) || !resolve(game_dir, got, sizeof got)) return false;
    if (path_eq(want, got)) return true;
    char alt[1100], alt_r[1100];
    if (g_game_root[0] && (size_t)snprintf(alt, sizeof alt, "%s" SEP "extracted", g_game_root) < sizeof alt &&
        path_eq(resolve(alt, alt_r, sizeof alt_r) ? alt_r : alt, got))
        return true;
    fprintf(stderr, "[paths] %s resolves to %s, not the data folder's %s\n", game_dir, got, want);
    return false;
}

bool rt_remove_game_file(const char *game_dir, const char *name) {
    bool known = false;
    for (size_t i = 0; i < sizeof GAME_FILES / sizeof *GAME_FILES; i++) known |= !strcmp(name, GAME_FILES[i]);
    if (!known || !game_dir_ok(game_dir)) {
        fprintf(stderr, "[paths] refusing to delete %s/%s (not app-created game files in the data root)\n", game_dir, name);
        return false;
    }
    char p[1100];
    snprintf(p, sizeof p, "%s" SEP "%s", game_dir, name);
    if (!exists(p) && !is_link(p)) return true;
    if (is_link(p)) {
        fprintf(stderr, "[paths] refusing to delete %s (a link)\n", p);
        return false;
    }
    // the entry itself must resolve inside <data root>/extracted too (belt and braces: neither it nor game_dir is a link)
    char want[1100], got[1100];
    size_t wl = game_dir_want(want, sizeof want) ? strlen(want) : 0;
    if (!wl || !resolve(p, got, sizeof got) || strlen(got) <= wl || !path_prefix(got, want, wl) ||
        (got[wl] != '/' && got[wl] != '\\')) {
        fprintf(stderr, "[paths] refusing to delete %s (resolves outside %s)\n", p, wl ? want : "the data folder");
        return false;
    }
    return is_dir(p) ? plat_remove_tree(p) : remove(p) == 0;  // (plat_remove_tree doesn't follow links inside)
}

bool rt_remove_game_files(const char *game_dir) {
    if (!game_dir_ok(game_dir)) {
        fprintf(stderr, "[paths] refusing to delete game files in %s (not <data root>/extracted)\n", game_dir);
        return false;
    }
    bool ok = true;
    for (size_t i = 0; i < sizeof GAME_FILES / sizeof *GAME_FILES; i++) ok &= rt_remove_game_file(game_dir, GAME_FILES[i]);
    rmdir(game_dir);  // only if empty: anything else in there stays
    return ok;
}

#ifdef _WIN32
// Sizes skip only real links (symlinks, junctions): OneDrive's cloud files are reparse points too, but they're files
static bool size_link(const char *p) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(p, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    FindClose(h);
    return (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(fd.dwReserved0);
}
#else
#define size_link is_link
#endif

static unsigned long long tree_size(const char *p) {
    struct stat st;
    if (size_link(p) || stat(p, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return (unsigned long long)st.st_size;
    unsigned long long n = 0;
    DIR *d = opendir(p);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char c[2048];
            snprintf(c, sizeof c, "%s" SEP "%s", p, e->d_name);
            n += tree_size(c);
        }
        closedir(d);
    }
    return n;
}

unsigned long long rt_tree_size(const char *path) { return tree_size(path); }

unsigned long long rt_game_files_size(const char *game_dir) {
    unsigned long long n = 0;
    char p[1100];
    for (size_t i = 0; i < sizeof GAME_FILES / sizeof *GAME_FILES; i++) {
        snprintf(p, sizeof p, "%s" SEP "%s", game_dir, GAME_FILES[i]);
        n += tree_size(p);
    }
    return n;
}

// ---- choosing another data folder, moving the data there (the start screen: runtime/ui/launcher.mm)
const char *rt_data_root_fixed(void) {
    const char *env = getenv("LARECOMP_DATA_ROOT");
    if (env && *env) return "Set by LARECOMP_DATA_ROOT.";
    if (g_dev) return "A source checkout uses its own folder.";
    return NULL;
}

static bool has_entries(const char *dir) {
    for (size_t i = 0; i < N_ENTRIES; i++) {
        char p[1100];
        join(p, sizeof p, dir, DATA_ENTRIES[i]);
        if (exists(p) || is_link(p)) return true;
    }
    return false;
}

// nothing in it but what file managers leave behind
static bool empty_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return true;
    struct dirent *e;
    bool empty = true;
    while (empty && (e = readdir(d))) {
        const char *n = e->d_name;
        empty = !strcmp(n, ".") || !strcmp(n, "..") || !strcmp(n, ".DS_Store") || !strcmp(n, ".localized") ||
                !strcmp(n, "desktop.ini") || !strcmp(n, "Thumbs.db");
    }
    closedir(d);
    return empty;
}

static unsigned long long free_space(const char *dir) {
#ifdef _WIN32
    ULARGE_INTEGER a;
    return GetDiskFreeSpaceExA(dir, &a, NULL, NULL) ? a.QuadPart : 0;
#else
    struct statvfs v;
    return statvfs(dir, &v) == 0 ? (unsigned long long)v.f_bavail * v.f_frsize : 0;
#endif
}

static unsigned long long entries_size(const char *dir) {
    unsigned long long n = 0;
    for (size_t i = 0; i < N_ENTRIES; i++) {
        char p[1100];
        join(p, sizeof p, dir, DATA_ENTRIES[i]);
        n += tree_size(p);
    }
    return n;
}

// LAS Recomp's data (an unfinished copy too), or nothing yet
static bool usable_dir(const char *dir) {
    char p[1100];
    join(p, sizeof p, dir, MARK_PARTIAL);
    return !is_dir(dir) || has_entries(dir) || exists(p) || empty_dir(dir);
}

void rt_data_target(const char *picked, RtDataTarget *t) {
    memset(t, 0, sizeof *t);
    char r[1100], root[1100], p[1100];
    snprintf(t->path, sizeof t->path, "%s", picked);
    if (!resolve(picked, r, sizeof r) || !is_dir(r) || !resolve(g_root, root, sizeof root)) {
        snprintf(t->refuse, sizeof t->refuse, "That folder can't be opened.");
        return;
    }
    // the folder itself when it's empty, existing app data, or the platform's folder; otherwise a "Raitchi"
    // folder in it
    if (same_dir(r, g_home) || usable_dir(r)) snprintf(t->path, sizeof t->path, "%s", r);
    else join(t->path, sizeof t->path, r, "Raitchi");
    join(p, sizeof p, t->path, MARK_PARTIAL);
    t->has_data = has_entries(t->path) && !exists(p);
    t->bytes = entries_size(g_root);
    t->free = free_space(r);
    struct stat a, b;
    bool same_fs = stat(root, &a) == 0 && stat(r, &b) == 0 && a.st_dev == b.st_dev;  // (hard links: no second copy)
    t->need = (same_fs ? 0 : t->bytes) + (64ull << 20);
    if (path_eq(t->path, root)) snprintf(t->refuse, sizeof t->refuse, "That's the data folder already.");
    else if (within(t->path, root)) snprintf(t->refuse, sizeof t->refuse, "That folder is inside the data folder.");
    else if (within(root, t->path)) snprintf(t->refuse, sizeof t->refuse, "That folder holds the data folder.");
    else if (in_app(r)) snprintf(t->refuse, sizeof t->refuse, "That folder is inside the app.");
    else if (!writable(r)) snprintf(t->refuse, sizeof t->refuse, "Raitchi can't write to that folder.");
    else if (!same_dir(t->path, g_home) && !usable_dir(t->path))
        snprintf(t->refuse, sizeof t->refuse, "Choose an empty folder.");
}

// ---- the move: copy, check the copy, switch over, then empty the old folder
typedef struct {
    void (*progress)(int step, double frac);
    unsigned long long done, total;
    int step;
    char *err;
    size_t errn;
    char *buf, *buf2;
} Move;

static void moved(Move *m, unsigned long long n) {
    m->done += n;
    if (m->progress) m->progress(m->step, m->total ? (double)m->done / (double)m->total : 1.0);
}

static bool move_fail(Move *m, const char *what, const char *p) {
    if (!m->err[0]) snprintf(m->err, m->errn, "%s %s (%s)", what, p, errno ? strerror(errno) : "differs");
    return false;
}

#ifdef _WIN32
static int lstat_(const char *p, struct stat *st) {
    if (is_link(p)) { errno = EPERM; return -1; }  // (links in the data folder: not moved on Windows)
    return stat(p, st);
}
#ifndef S_ISLNK
#define S_ISLNK(m) 0
#endif
#else
#define lstat_ lstat
#endif

#define MOVE_BUF (1u << 20)
static bool copy_file(Move *m, const char *s, const char *d, const struct stat *st) {
    remove(d);
    static const char *no_links;  // (tests: LARECOMP_MOVE_NO_LINKS=1 copies as if to another drive)
    if (!no_links) no_links = getenv("LARECOMP_MOVE_NO_LINKS") ? "1" : "";
#ifdef _WIN32
    if (!*no_links && CreateHardLinkA(d, s, NULL)) { moved(m, st->st_size); return true; }  // (same volume: a second
#else                                                                                       //  name, no copy)
    if (!*no_links && link(s, d) == 0) { moved(m, st->st_size); return true; }
#endif
    FILE *in = fopen(s, "rb"), *out = in ? fopen(d, "wb") : NULL;
    bool ok = in && out;
    errno = 0;
    while (ok) {
        size_t k = fread(m->buf, 1, MOVE_BUF, in);
        if (!k) { ok = !ferror(in); break; }
        ok = fwrite(m->buf, 1, k, out) == k;
        moved(m, k);
    }
    if (out) ok &= fflush(out) == 0 && fsync(fileno(out)) == 0, ok &= fclose(out) == 0;
    if (in) fclose(in);
    if (!ok) return move_fail(m, "Couldn't copy", s);
    struct utimbuf u = {st->st_atime, st->st_mtime};
    utime(d, &u);
    return true;
}

static bool copy_tree(Move *m, const char *s, const char *d) {
    struct stat st;
    if (lstat_(s, &st) != 0) return move_fail(m, "Couldn't read", s);
    if (S_ISLNK(st.st_mode)) {
#ifndef _WIN32
        char t[PATH_MAX];
        ssize_t k = readlink(s, t, sizeof t - 1);
        if (k < 0) return move_fail(m, "Couldn't read", s);
        t[k] = 0;
        remove(d);
        if (symlink(t, d) != 0) return move_fail(m, "Couldn't copy", s);
#endif
        return true;
    }
    if (S_ISREG(st.st_mode)) return copy_file(m, s, d, &st);
    if (!S_ISDIR(st.st_mode)) return true;  // (sockets and such: nothing of ours)
    if (mkdir(d, 0755) != 0 && !is_dir(d)) return move_fail(m, "Couldn't create", d);
    DIR *dir = opendir(s);
    if (!dir) return move_fail(m, "Couldn't read", s);
    bool ok = true;
    struct dirent *e;
    while (ok && (e = readdir(dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strncmp(e->d_name, "._", 2)) continue;  // (._: macOS
        char cs[4096], cd[4096];                                                                         //  metadata)
        join(cs, sizeof cs, s, e->d_name);
        join(cd, sizeof cd, d, e->d_name);
        ok = copy_tree(m, cs, cd);
    }
    closedir(dir);
    return ok;
}

static bool same_bytes(Move *m, const char *a, const char *b) {
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    bool ok = fa && fb;
    while (ok) {
        size_t k = fread(m->buf, 1, MOVE_BUF, fa), j = fread(m->buf2, 1, MOVE_BUF, fb);
        ok = k == j && !memcmp(m->buf, m->buf2, k) && !ferror(fa) && !ferror(fb);
        moved(m, k);
        if (!k) break;
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return ok;
}

static bool check_tree(Move *m, const char *s, const char *d) {
    struct stat a, b;
    errno = 0;
    if (lstat_(s, &a) != 0 || lstat_(d, &b) != 0) return move_fail(m, "Missing in the copy:", d);
    if ((a.st_mode & S_IFMT) != (b.st_mode & S_IFMT)) return move_fail(m, "Differs in the copy:", d);
    if (S_ISREG(a.st_mode)) {
        if (a.st_size != b.st_size) return move_fail(m, "Differs in the copy:", d);
#ifndef _WIN32
        if (a.st_dev == b.st_dev && a.st_ino == b.st_ino) { moved(m, a.st_size); return true; }  // (the same file)
#endif
        errno = 0;
        return same_bytes(m, s, d) || move_fail(m, "Differs in the copy:", d);
    }
#ifndef _WIN32
    if (S_ISLNK(a.st_mode)) {
        char ta[PATH_MAX] = {0}, tb[PATH_MAX] = {0};
        if (readlink(s, ta, sizeof ta - 1) < 0 || readlink(d, tb, sizeof tb - 1) < 0 || strcmp(ta, tb))
            return move_fail(m, "Differs in the copy:", d);
        return true;
    }
#endif
    if (!S_ISDIR(a.st_mode)) return true;
    DIR *dir = opendir(s);
    if (!dir) return move_fail(m, "Couldn't read", s);
    bool ok = true;
    struct dirent *e;
    while (ok && (e = readdir(dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strncmp(e->d_name, "._", 2)) continue;  // (._: macOS
        char cs[4096], cd[4096];                                                                         //  metadata)
        join(cs, sizeof cs, s, e->d_name);
        join(cd, sizeof cd, d, e->d_name);
        ok = check_tree(m, cs, cd);
    }
    closedir(dir);
    return ok;
}

// The switch: the pointer file names `to` (none when it's the platform's folder)
static bool point_to(const char *to) {
    char p[1100], r[1100];
    join(p, sizeof p, g_home, POINTER_FILE);
    if (same_dir(to, g_home)) return remove(p) == 0 || !exists(p);
    if (!resolve(to, r, sizeof r)) return false;
    mkdirs(g_home);
    return write_line(p, r);
}

// A folder to use: created if needed, writable, with no unfinished copy or old move notes in it
static bool prepare_target(const char *to, char *err, size_t n) {
    char p[1100];
    mkdirs(to);
    if (!is_dir(to) || !writable(to)) {
        snprintf(err, n, "Raitchi can't write to %s.", to);
        return false;
    }
    join(p, sizeof p, to, MARK_PARTIAL);
    if (exists(p)) remove_partial(to);
    join(p, sizeof p, to, NOTE_FROM);
    remove(p);  // (a copy that was never switched to: it mustn't empty another folder later)
    return true;
}

bool rt_data_use(const char *to, char *err, size_t n) {
    err[0] = 0;
    if (!prepare_target(to, err, n)) return false;
    if (!point_to(to)) {
        snprintf(err, n, "Couldn't save the data folder's location in %s.", g_home);
        return false;
    }
    fprintf(stderr, "[paths] data folder: %s from the next start\n", to);
    return true;
}

bool rt_data_move(const char *to, void (*progress)(int step, double frac), char *err, size_t n) {
    err[0] = 0;
    char from[1024], p[1100], q[1100], abs_to[1100];
    snprintf(from, sizeof from, "%s", g_root);
    if (!prepare_target(to, err, n)) return false;
    if (!resolve(to, abs_to, sizeof abs_to) || overlap(abs_to, from)) {
        snprintf(err, n, "That folder is inside the data folder.");
        return false;
    }
    if (has_entries(abs_to)) {
        snprintf(err, n, "That folder already has Raitchi data.");
        return false;
    }
    Move m = {progress, 0, entries_size(from), 0, err, n, malloc(MOVE_BUF), malloc(MOVE_BUF)};
    join(p, sizeof p, abs_to, MARK_PARTIAL);
    join(q, sizeof q, from, NOTE_TO);
    bool ok = m.buf && m.buf2 && write_line(p, from) && write_line(q, abs_to);
    if (!ok) move_fail(&m, "Couldn't write to", abs_to);
    fprintf(stderr, "[paths] moving the data (%.2f GB) from %s to %s\n", m.total / 1e9, from, abs_to);
    for (int step = 0; step < 2 && ok; step++) {  // copy, then check every file of the copy
        m.step = step, m.done = 0;
        moved(&m, 0);
        for (size_t i = 0; i < N_ENTRIES && ok; i++) {
            char s[1100], d[1100];
            join(s, sizeof s, from, DATA_ENTRIES[i]);
            join(d, sizeof d, abs_to, DATA_ENTRIES[i]);
            if (!exists(s) && !is_link(s)) continue;
            ok = step ? check_tree(&m, s, d) : copy_tree(&m, s, d);
        }
    }
    free(m.buf), free(m.buf2);
    // checked: the copy names its source (emptied once it's in use), stops being unfinished, and becomes the data
    // folder. Until the switch the old folder is the one in use, and its note undoes the copy at the next start.
    if (ok) {
        join(q, sizeof q, abs_to, NOTE_FROM);
        ok = write_line(q, from) && remove(p) == 0;
        if (!ok) move_fail(&m, "Couldn't write to", abs_to);
    }
    if (ok && !point_to(abs_to)) ok = false, snprintf(err, n, "Couldn't save the data folder's location in %s.", g_home);
    if (!ok) {
        fprintf(stderr, "[paths] move failed: %s\n", err);
        remove_partial(abs_to);
        join(q, sizeof q, from, NOTE_TO);
        remove(q);
        return false;
    }
    fprintf(stderr, "[paths] data moved to %s\n", abs_to);
    finish_moves(abs_to, progress);  // (the old copy; interrupted, the next start finishes it)
    return true;
}

void rt_paths_restart(void) {
    fprintf(stderr, "[paths] restarting\n");
    fflush(stderr);
    if (chdir(g_start) != 0) fprintf(stderr, "[paths] can't go back to %s\n", g_start);  // (not the old data folder:
    plat_exec(NULL);                                                                     //  dev mode there)
}

// Window / Dock icon: icon.png shipped next to the exe or in the .app (none for a plain source build).
void rt_window_icon(SDL_Window *w) {
    char p[1100];
    if (!rt_resource("icon.png", NULL, p, sizeof p)) return;
    SDL_Surface *s = SDL_LoadPNG(p);
    if (s) SDL_SetWindowIcon(w, s), SDL_DestroySurface(s);
}

// LARECOMP_DISPLAY=<number from 0, or part of its name, e.g. Built-in>: the window opens centred on that display
// (test runs on a laptop's own screen while the external one stays free).
void rt_window_display(SDL_Window *w) {
    const char *e = getenv("LARECOMP_DISPLAY");
    if (!e || !*e) return;
    int n = 0;
    SDL_DisplayID *ids = SDL_GetDisplays(&n);
    SDL_DisplayID pick = 0;
    char *end;
    long i = strtol(e, &end, 10);
    if (!*end && i >= 0 && i < n) pick = ids[i];
    for (int k = 0; !pick && k < n; k++) {
        const char *name = SDL_GetDisplayName(ids[k]);
        if (name && strstr(name, e)) pick = ids[k];
    }
    SDL_free(ids);
    if (pick) SDL_SetWindowPosition(w, SDL_WINDOWPOS_CENTERED_DISPLAY(pick), SDL_WINDOWPOS_CENTERED_DISPLAY(pick));
}
