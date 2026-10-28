/* Generic fallback adapter derived from a package's own ExeFS metadata.
 * Layout policy (guest shift, window, host regions, 32 MiB module spacing) is
 * taken from the native-only Echoes adapter; nothing is copied from a game. */
#define _POSIX_C_SOURCE 200809L
#include "generic.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define NSO_HEADER_SIZE 0x100u
#define NSO_MAX_FILE (512u << 20)      /* native preparation policy */
#define NSO_MAX_MAPPED (256u << 20)    /* native preparation policy */
#define NPDM_META_SIZE 0x80u
#define NPDM_ACI0_READ 0x18u
#define NPDM_MAX_FILE (1u << 20)
#define GENERIC_NCE_LEAD_IN UINT64_C(0x1000000)

static const struct { const char *name; uint32_t id; } load_order[] = {
    /* Horizon load order after rtld: main, subsdk0..9, sdk. The shared module
     * ABI has stable IDs for two subsdk modules. */
    {"main", MODULE_ABI_MAIN}, {"subsdk0", MODULE_ABI_SUBSDK0},
    {"subsdk1", MODULE_ABI_SUBSDK1}, {"sdk", MODULE_ABI_SDK}};
#define LOAD_ORDER_COUNT (sizeof load_order / sizeof load_order[0])

static bool problem(char *why, size_t cap, const char *format, const char *detail) {
    if (why && cap) snprintf(why, cap, format, detail ? detail : "");
    return false;
}
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }
static bool nonzero(const uint8_t *p, size_t n) { while (n--) if (*p++) return true; return false; }

typedef struct Inventory {
    bool module[LOAD_ORDER_COUNT], npdm, rtld, failed;
    uint64_t module_size[LOAD_ORDER_COUNT], npdm_size;
    char *why; size_t cap;
} Inventory;

static bool visit(void *context, const char *name, uint64_t size) {
    Inventory *inv = context;
    if (!name) { inv->failed = true; return problem(inv->why, inv->cap, "Invalid ExeFS entry name%s", NULL); }
    for (size_t i = 0; i < LOAD_ORDER_COUNT; i++)
        if (!strcmp(name, load_order[i].name)) {
            if (inv->module[i]) break;
            inv->module[i] = true; inv->module_size[i] = size; return true;
        }
    if (!strcmp(name, "main.npdm") && !inv->npdm) { inv->npdm = true; inv->npdm_size = size; return true; }
    if (!strcmp(name, "rtld") && !inv->rtld) { inv->rtld = true; return true; }
    inv->failed = true;
    if (!strncmp(name, "subsdk", 6))
        return problem(inv->why, inv->cap, "ExeFS module %.32s is beyond the main, subsdk0, subsdk1 and sdk modules Raitchi can load", name);
    return problem(inv->why, inv->cap, "Unexpected or duplicate ExeFS file: %.48s", name);
}

static bool read_npdm(const TitleGenericSource *source, uint64_t size, TitleGenericAdapter *g,
                      uint64_t *program_id, char *why, size_t cap) {
    uint8_t meta[NPDM_META_SIZE], aci[NPDM_ACI0_READ];
    if (size < NPDM_META_SIZE || size > NPDM_MAX_FILE || !source->read(source->context, "main.npdm", 0, meta, sizeof meta) ||
        memcmp(meta, "META", 4))
        return problem(why, cap, "main.npdm META header is missing or invalid%s", NULL);
    uint8_t flags = meta[0xc];
    uint8_t space = (uint8_t)((flags >> 1) & 7u);
    if (!(flags & 1u) || space == 0 || space == 2)
        return problem(why, cap, "This is a 32-bit program; Raitchi runs 64-bit programs only%s", NULL);
    if (space != 3)
        return problem(why, cap, "This program uses the 36-bit address space; Raitchi's native session supports 39-bit programs only%s", NULL);
    uint32_t aci_offset = le32(meta + 0x70), aci_size = le32(meta + 0x74);
    if (aci_size < NPDM_ACI0_READ || aci_offset > size || size - aci_offset < aci_size ||
        !source->read(source->context, "main.npdm", aci_offset, aci, sizeof aci) || memcmp(aci, "ACI0", 4))
        return problem(why, cap, "main.npdm ACI0 header is missing or invalid%s", NULL);
    uint32_t stack = le32(meta + 0x1c);
    /* Mirrors main.c startup policy: priority 0..63, core 0..3, 4 KiB stack. */
    if (meta[0xe] > 63 || meta[0xf] > 3 || !stack || (stack & 0xfffu) ||
        stack > title_adapter_find(TITLE_ECHOES)->regions[SESSION_STACK].size)
        return problem(why, cap, "main.npdm main-thread priority, core or stack size is outside the supported policy%s", NULL);
    g->main_thread_priority = meta[0xe];
    g->main_thread_core = meta[0xf];
    g->main_thread_stack_size = stack;
    g->system_resource_size = le32(meta + 0x14);
    g->address_space = space;
    size_t n = strnlen((const char *)meta + 0x20, 16);
    memcpy(g->npdm_name, meta + 0x20, n);
    g->npdm_name[n] = 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)g->npdm_name[i] < 0x20 || (unsigned char)g->npdm_name[i] > 0x7e) g->npdm_name[i] = '?';
    *program_id = le64(aci + 0x10);
    return true;
}

void title_generic_version_token(const char *version, char out[TITLE_GENERIC_VERSION_MAX + 1]) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)(version ? version : ""); *p && n < TITLE_GENERIC_VERSION_MAX; p++) {
        unsigned char c = *p;
        out[n++] = (char)((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          c == '.' || c == '_' || c == '-' ? c : '_');
    }
    if (!n) out[n++] = '0';
    out[n] = 0;
}

bool title_generic_parse_nvn_api(const char *text, uint32_t *major, uint32_t *minor) {
    if (major) *major = 0;
    if (minor) *minor = 0;
    if (!text || !major || !minor) return false;
    uint32_t values[2] = {0, 0};
    const char *p = text;
    for (unsigned part = 0; part < 2; part++) {
        unsigned digits = 0;
        while (*p >= '0' && *p <= '9') {
            if (++digits > 3) return false;
            values[part] = values[part] * 10u + (uint32_t)(*p++ - '0');
        }
        if (!digits || (part == 0 && *p++ != '.')) return false;
    }
    if (*p || !values[0]) return false;
    *major = values[0];
    *minor = values[1];
    return true;
}

bool title_generic_derive(const TitleGenericSource *source, const TitleGenericOptions *options,
                          TitleGenericAdapter *out, char *why, size_t cap) {
    if (out) memset(out, 0, sizeof *out);
    if (why && cap) why[0] = 0;
    if (!source || !source->list || !source->read || !options || !out)
        return problem(why, cap, "Invalid generic adapter arguments%s", NULL);
    TitleGenericAdapter g;
    memset(&g, 0, sizeof g);
    Inventory inv = {.why = why, .cap = cap};
    if (!source->list(source->context, visit, &inv)) {
        if (!inv.failed && !(why && cap && why[0])) problem(why, cap, "Cannot read the game's ExeFS file table%s", NULL);
        return false;
    }
    if (!inv.module[0]) return problem(why, cap, "The game's ExeFS has no main module%s", NULL);
    if (!inv.module[LOAD_ORDER_COUNT - 1]) return problem(why, cap, "The game's ExeFS has no sdk module%s", NULL);
    if (!inv.npdm) return problem(why, cap, "The game's ExeFS has no main.npdm%s", NULL);
    uint64_t program_id = 0;
    if (!read_npdm(source, inv.npdm_size, &g, &program_id, why, cap)) return false;
    if (!program_id) return problem(why, cap, "main.npdm has no program ID%s", NULL);
    if (program_id & 0xfffu)
        return problem(why, cap, "Only base application programs can be tried (not updates, add-ons or multi-program packages)%s", NULL);
    if (options->expected_program_id && options->expected_program_id != program_id)
        return problem(why, cap, "main.npdm program ID differs from the package program ID%s", NULL);
    for (int id = TITLE_LAS; id <= TITLE_WONDER; id++) {
        const TitleAdapter *known = title_adapter_find((enum TitleId)id);
        if (known && known->program_id == program_id)
            return problem(why, cap, "A specific Raitchi adapter exists for this program ID: %s", known->key);
    }
    /* Layout policy of the native-only Echoes adapter, which is not game data. */
    const TitleAdapter *policy = title_adapter_find(TITLE_ECHOES);
    TitleAdapter *a = &g.adapter;
    a->id = TITLE_GENERIC;
    a->key = TITLE_GENERIC_KEY;
    a->label = "Generic startup (no Raitchi adapter)";
    a->program_id = program_id;
    a->guest_shift = policy->guest_shift;
    /* Own one trampoline bank below main without moving any guest image or
     * fixed region. The reservation path must acquire this entire window. */
    if (policy->canonical_window_lo <= policy->guest_shift ||
        policy->canonical_window_lo - policy->guest_shift <= GENERIC_NCE_LEAD_IN)
        return problem(why, cap, "Generic trampoline reservation is outside the guest window%s", NULL);
    a->canonical_window_lo = policy->canonical_window_lo - GENERIC_NCE_LEAD_IN;
    a->canonical_window_hi = policy->canonical_window_hi;
    memcpy(a->regions, policy->regions, sizeof a->regions);
    a->capabilities = 0;          /* native-only: no generated code */
    a->audout_min_queue_ms = 0;   /* three of four adapters */
    a->renderer_policy_flags = 0; /* no title-specific renderer policy */
    a->nvn_api_major = options->nvn_api_major;
    a->nvn_api_minor = options->nvn_api_major ? options->nvn_api_minor : 0;
    uint64_t base = policy->canonical_window_lo;
    const uint64_t limit = policy->regions[SESSION_HLE].canonical_base;
    uint32_t count = 0;
    for (size_t i = 0; i < LOAD_ORDER_COUNT; i++) {
        if (!inv.module[i]) continue;
        const char *name = load_order[i].name;
        uint8_t h[NSO_HEADER_SIZE];
        if (inv.module_size[i] < NSO_HEADER_SIZE || inv.module_size[i] > NSO_MAX_FILE ||
            !source->read(source->context, name, 0, h, sizeof h) || memcmp(h, "NSO0", 4))
            return problem(why, cap, "Invalid NSO header in ExeFS module %s", name);
        uint64_t end = 0, previous = 0;
        for (unsigned s = 0; s < 3; s++) {
            uint64_t offset = le32(h + 0x14 + s * 16), size = le32(h + 0x18 + s * 16);
            if (offset < previous) return problem(why, cap, "Unsupported NSO segment order in %s", name);
            previous = offset;
            uint64_t e = offset + size + (s == 2 ? le32(h + 0x3c) : 0);
            if (e > end) end = e;
        }
        uint32_t text_offset = le32(h + 0x14), text_size = le32(h + 0x18);
        uint64_t mapped = (end + TITLE_GENERIC_MAPPING_ALIGNMENT - 1) & ~(uint64_t)(TITLE_GENERIC_MAPPING_ALIGNMENT - 1);
        if (!text_size || ((text_offset | text_size) & 3u) || !mapped || mapped > NSO_MAX_MAPPED)
            return problem(why, cap, "NSO segments of %s exceed the native preparation policy", name);
        if (!nonzero(h + 0x40, 32)) return problem(why, cap, "ExeFS module %s has no build ID", name);
        uint64_t span = (mapped + TITLE_GENERIC_MODULE_SPACING - 1) & ~(TITLE_GENERIC_MODULE_SPACING - 1);
        if (span > limit - base) return problem(why, cap, "Game modules do not fit the native module window%s", NULL);
        a->modules[count] = (TitleModulePolicy){load_order[i].id, name, base};
        a->module_spans[count] = span;
        a->module_build_prefix_size[count] = 20;
        memcpy(a->module_build_prefix[count], h + 0x40, 20);
        if (load_order[i].id == MODULE_ABI_MAIN) {
            a->main_build_prefix_size = 20;
            memcpy(a->main_build_prefix, h + 0x40, 20);
        }
        base += span;
        count++;
    }
    a->module_count = count;
    /* Assumed startup policy (SDK16 shape, as Echoes): SDK initializers run
     * early, the remaining modules late in reverse load order. The argument
     * count (0 here) is read from the SDK's exported nn::init::Start at startup. */
    a->startup.argument_count = 0;
    a->startup.early_count = 1;
    a->startup.early_modules[0] = MODULE_ABI_SDK;
    for (uint32_t i = count - 1; i-- > 0;)
        a->startup.late_modules[a->startup.late_count++] = a->modules[i].module_id;
    SessionLayoutSpec spec = {.struct_size = sizeof spec, .version = SESSION_LAYOUT_VERSION,
        .module_count = count, .page_size = TITLE_GENERIC_MAPPING_ALIGNMENT, .guest_shift = a->guest_shift,
        .canonical_window_lo = a->canonical_window_lo, .canonical_window_hi = a->canonical_window_hi};
    memcpy(spec.regions, a->regions, sizeof spec.regions);
    for (uint32_t i = 0; i < count; i++)
        spec.modules[i] = (SessionModuleSpec){a->modules[i].module_id, 0, {a->modules[i].canonical_base, a->module_spans[i]}};
    SessionLayout normalized;
    if (session_layout_build(&spec, &normalized, NULL) != SESSION_LAYOUT_OK)
        return problem(why, cap, "Generic module layout was rejected%s", NULL);
    g.rtld_present = inv.rtld;
    title_generic_version_token(options->display_version, g.version);
    snprintf(g.save_namespace, sizeof g.save_namespace, "generic-%016" PRIx64 "-%s", program_id, g.version);
    *out = g;
    out->adapter.version = out->version;
    out->adapter.save_namespace = out->save_namespace;
    return true;
}

typedef struct DirectorySource { const char *dir; char *why; size_t cap; } DirectorySource;
static bool directory_path(const DirectorySource *d, const char *name, char *path, size_t size) {
    int n = snprintf(path, size, "%s/%s", d->dir, name);
    return n > 0 && (size_t)n < size;
}
static bool directory_list(void *context, TitleGenericVisit callback, void *visit_context) {
    DirectorySource *d = context;
    DIR *dir = opendir(d->dir);
    if (!dir) return false;
    bool ok = true;
    for (struct dirent *e; ok && (errno = 0, e = readdir(dir));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char path[4096];
        struct stat st;
        if (!directory_path(d, e->d_name, path, sizeof path) || lstat(path, &st) || !S_ISREG(st.st_mode)) {
            problem(d->why, d->cap, "ExeFS entry is not a regular file: %.48s", e->d_name);
            ok = false;
            break;
        }
        ok = callback(visit_context, e->d_name, (uint64_t)st.st_size);
    }
    if (ok && errno) ok = false;
    closedir(dir);
    return ok;
}
static bool directory_read(void *context, const char *name, uint64_t offset, void *dst, size_t size) {
    const DirectorySource *d = context;
    char path[4096];
    if (!directory_path(d, name, path, sizeof path) || offset > (uint64_t)INT64_MAX) return false;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    uint8_t *p = dst;
    bool ok = true;
    while (size) {
        ssize_t n = pread(fd, p, size, (off_t)offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        p += n; size -= (size_t)n; offset += (uint64_t)n;
    }
    close(fd);
    return ok;
}

bool title_generic_derive_directory(const char *exefs_dir, const TitleGenericOptions *options,
                                    TitleGenericAdapter *out, char *why, size_t cap) {
    if (out) memset(out, 0, sizeof *out);
    if (!exefs_dir || !*exefs_dir) return problem(why, cap, "Invalid generic ExeFS directory%s", NULL);
    DirectorySource d = {exefs_dir, why, cap};
    TitleGenericSource source = {&d, directory_list, directory_read};
    return title_generic_derive(&source, options, out, why, cap);
}

void title_generic_describe(const TitleGenericAdapter *g, char *out, size_t size) {
    if (!out || !size) return;
    out[0] = 0;
    if (!g) return;
    const TitleAdapter *a = &g->adapter;
    char modules[160] = "";
    size_t used = 0;
    for (uint32_t i = 0; i < a->module_count && used < sizeof modules; i++) {
        int n = snprintf(modules + used, sizeof modules - used, "%s%s@%" PRIx64 "+%" PRIx64, i ? "," : "",
                         a->modules[i].name, a->modules[i].canonical_base, a->module_spans[i]);
        if (n < 0) break;
        used += (size_t)n;
    }
    char nvn[48];
    if (a->nvn_api_major) snprintf(nvn, sizeof nvn, "%u.%u (override)", a->nvn_api_major, a->nvn_api_minor);
    else snprintf(nvn, sizeof nvn, "53.207 (assumed default)");
    snprintf(out, size,
        "[generic] no Raitchi adapter for program %016" PRIx64 " version %s; trying generic native startup, "
        "the game may stop or close unexpectedly. Derived: modules=%s rtld=%s npdm=\"%s\" priority=%u core=%u "
        "stack=0x%x system_resource=0x%x address_space=39-bit save=%s. Assumed: NVN API %s, "
        "nn::init::Start argument count from the SDK export, init order sdk early then remaining modules late "
        "in reverse load order, audio queue 0 ms, no renderer policy flags",
        a->program_id, g->version, modules, g->rtld_present ? "present,not-loaded" : "absent", g->npdm_name,
        g->main_thread_priority, g->main_thread_core, g->main_thread_stack_size, g->system_resource_size,
        g->save_namespace, nvn);
}
