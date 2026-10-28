/* Adapted from pinned LAS Android shared-font service. Copyright (c) 2026 Peiikai.
 * MIT: native/runtime/LICENSE-LASRecomp. */
// nn::pl shared system fonts. The Switch's system fonts are not part of the game, so every SharedFontType the game
// asks for is filled from, in order of preference:
//  1. LARECOMP_FONT=<file> (.ttf / .otf / .ttc face 0 / .bfttf)
//  2. a dump from the user's own console in fonts/, under the console's own names (plain .ttf versions work too):
//     nintendo_udsg-r_std_003 (Standard, Morisawa UD Shin Go), nintendo_udsg-r_org_zh-cn_003 (Chinese Simplified),
//     nintendo_udsg-r_ext_zh-cn_003 (its Ext), nintendo_udjxh-db_zh-tw_003 (Chinese Traditional),
//     nintendo_udsg-r_ko_003 (Korean), nintendo_ext_003 (NintendoExt button glyphs)
//  3. a substitute of that same language where the OS has one - a gothic like UD Shin Go is much closer to the
//     game's look than Arial. Without one (a Linux without a CJK font, say) the type serves Standard, which is
//     what a game asking for a language whose font is missing gets on the console too - but then that language's
//     characters are the substitute's, so Chinese / Korean text can show missing glyphs.
//     Standard's Japanese gothics are skipped when the game plays in Russian: Japanese fonts carry Cyrillic as
//     full-width characters (JIS X 0208), every letter a square apart (issue #32) - Arial / DejaVu Sans then.
//  4. Standard only: DejaVu Sans shipped with the app (third_party/fonts/DejaVu, free license), for systems with none
//     of the above (Bazzite / Fedora Atomic have no DejaVu - issue #37).
//  5. NintendoExt only: a game-free generic button-font substitute, for prompts that use L/R and other button glyphs.
// .bfttf is the console's obfuscated TTF: 8-byte header (magic, size) then every u32 XORed with a fixed key (bfttf_decode).
// .ttc collections get face 0 copied out as a standalone font (the game's font engine wants one sfnt).
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../hle.h"
#include "../paths.h"

typedef struct { uint64_t addr, size; } Font;
// SharedFontType: 0 Standard, 1 ChineseSimple, 2 ChineseSimpleExt, 3 ChineseTraditional, 4 Korean, 5 NintendoExt
enum { F_STD, F_ZH_CN, F_ZH_CN_EXT, F_ZH_TW, F_KO, F_EXT, N_FONT_TYPES };
static Font fonts[N_FONT_TYPES];
static bool g_font_trace;
static unsigned g_font_trace_count;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    long size = fseek(f, 0, SEEK_END) ? -1 : ftell(f);
    // A failed ftell() (e.g. a special file) must not fall through: (size_t)-1 + 4 wraps to a tiny
    // malloc that the subsequent fread() would then overrun.
    if (size < 0 || fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; }
    *len = (size_t)size;
    uint8_t *p = malloc(*len + 4);
    if (!p || fread(p, 1, *len, f) != *len) { free(p); p = NULL; }
    fclose(f);
    return p;
}

// .bfttf -> TTF in place (returns the new length, 0 if it isn't one). Read as little-endian words: the first is
// 0x7F9A0218 ^ key, the second the font's size ^ key, then the font itself, each word XORed with the key and stored
// byte-swapped (issue #48: the console's own files; decoded without the swap they never matched)
static size_t bfttf_decode(uint8_t *d, size_t len) {
    static const uint32_t KEY = 0x49621806, MAGIC = 0x7F9A0218;
    if (len < 12) return 0;
    uint32_t w0, w1;
    memcpy(&w0, d, 4);
    memcpy(&w1, d + 4, 4);
    uint32_t size = w1 ^ KEY;
    size_t words = ((size_t)size + 3) & ~(size_t)3;  // (the last word is padded)
    if ((w0 ^ KEY) != MAGIC || size < 4 || words > len - 8) return 0;
    for (size_t i = 0; i < words; i += 4) {
        uint32_t w;
        memcpy(&w, d + 8 + i, 4);
        put32(d + i, w ^ KEY);  // (big-endian: the font's own byte order)
    }
    uint32_t ver = be32(d);
    return ver == 0x00010000u || ver == 0x4F54544Fu || ver == 0x74727565u ? size : 0;  // TrueType / 'OTTO' / 'true'
}

// .ttc -> face 0 as its own sfnt (table data copied, offsets rewritten)
static uint8_t *ttc_face0(const uint8_t *f, size_t len, size_t *out_len) {
    if (len < 16 || memcmp(f, "ttcf", 4)) return NULL;
    uint32_t o = be32(f + 12);
    if (o + 12 > len) return NULL;
    uint32_t nt = (uint32_t)f[o + 4] << 8 | f[o + 5];
    size_t total = 12 + 16 * nt;
    for (uint32_t i = 0; i < nt; i++) total += (be32(f + o + 12 + 16 * i + 12) + 3) & ~3u;
    uint8_t *out = calloc(1, total);
    memcpy(out, f + o, 12);
    size_t pos = 12 + 16 * nt;
    for (uint32_t i = 0; i < nt; i++) {
        const uint8_t *e = f + o + 12 + 16 * i;
        uint32_t off = be32(e + 8), n = be32(e + 12);
        if ((size_t)off + n > len) { free(out); return NULL; }
        memcpy(out + 12 + 16 * i, e, 8);  // tag, checksum
        put32(out + 12 + 16 * i + 8, (uint32_t)pos);
        put32(out + 12 + 16 * i + 12, n);
        memcpy(out + pos, f + off, n);
        pos += (n + 3) & ~3u;
    }
    *out_len = total;
    return out;
}

// Does the font map this character to a glyph? (its cmap: format 4 or 12 subtables, Unicode encodings)
static bool font_has(const uint8_t *f, size_t len, uint32_t cp) {
    if (len < 12) return false;
    uint32_t nt = (uint32_t)f[4] << 8 | f[5], cmap = 0, cmap_len = 0;
    for (uint32_t i = 0; i < nt && 12 + 16 * (i + 1) <= len; i++)
        if (!memcmp(f + 12 + 16 * i, "cmap", 4)) { cmap = be32(f + 12 + 16 * i + 8); cmap_len = be32(f + 12 + 16 * i + 12); }
    if (!cmap || (size_t)cmap + cmap_len > len || cmap_len < 4) return false;
    const uint8_t *c = f + cmap, *end = f + cmap + cmap_len;
    uint32_t ns = (uint32_t)c[2] << 8 | c[3];
    for (uint32_t i = 0; i < ns && c + 4 + 8 * (i + 1) <= end; i++) {
        uint32_t plat = (uint32_t)c[4 + 8 * i] << 8 | c[5 + 8 * i], enc = (uint32_t)c[6 + 8 * i] << 8 | c[7 + 8 * i];
        if (!(plat == 0 || (plat == 3 && (enc == 1 || enc == 10)))) continue;  // (Unicode subtables only)
        const uint8_t *t = c + be32(c + 8 + 8 * i);
        if (t + 4 > end) continue;
        uint32_t fmt = (uint32_t)t[0] << 8 | t[1];
        if (fmt == 4 && cp <= 0xFFFF && t + 14 <= end) {
            uint32_t segs = ((uint32_t)t[6] << 8 | t[7]) / 2;
            const uint8_t *ends = t + 14, *starts = ends + 2 * segs + 2, *deltas = starts + 2 * segs, *ranges = deltas + 2 * segs;
            if (ranges + 2 * segs > end) continue;
            for (uint32_t k = 0; k < segs; k++) {
                uint32_t e = (uint32_t)ends[2 * k] << 8 | ends[2 * k + 1], st = (uint32_t)starts[2 * k] << 8 | starts[2 * k + 1];
                if (cp > e) continue;
                if (cp < st) break;
                uint32_t ro = (uint32_t)ranges[2 * k] << 8 | ranges[2 * k + 1], d = (uint32_t)deltas[2 * k] << 8 | deltas[2 * k + 1];
                if (!ro) return ((cp + d) & 0xFFFF) != 0;
                const uint8_t *g = ranges + 2 * k + ro + 2 * (cp - st);
                return g + 2 <= end && ((((uint32_t)g[0] << 8 | g[1]) + (g[0] | g[1] ? d : 0)) & 0xFFFF) != 0;
            }
        } else if (fmt == 12 && t + 16 <= end) {
            uint32_t ng = be32(t + 12);
            for (uint32_t k = 0; k < ng && t + 16 + 12 * (k + 1) <= end; k++) {
                const uint8_t *gr = t + 16 + 12 * k;
                if (cp >= be32(gr) && cp <= be32(gr + 4)) return be32(gr + 8) + (cp - be32(gr)) != 0;
            }
        }
    }
    return false;
}

// The characters the game counts on in every font it's given: a space and "□" (U+25A1), its box for a missing
// character - it looks the box's glyph up at startup without checking for none, and crashed on a PC whose Yu Gothic
// lacked it (issue #45). A substitute without them is passed over for the next one.
static bool font_usable(const uint8_t *f, size_t len) { return font_has(f, len, 0x20) && font_has(f, len, 0x25A1); }

static int load_one_checked(const char *path, Font *ft, bool check);
static int load_one(const char *path, Font *ft) { return load_one_checked(path, ft, false); }
static int load_one_checked(const char *path, Font *ft, bool check) {
    // Shared user fonts stay in the app's resource root, while each title's
    // saves/settings/shader caches use its own session working directory.
    char resolved[4096];
    if (path[0] != '/' && !(path[0] && path[1] == ':')) {
        int n = snprintf(resolved, sizeof resolved, "%s/%s", rt_data_root(), path);
        if (n < 0 || (size_t)n >= sizeof resolved) return 0;
        path = resolved;
    }
    size_t len;
    uint8_t *d = read_file(path, &len);
    if (!d) return 0;
    size_t n = strlen(path);
    if (n > 6 && !strcasecmp(path + n - 6, ".bfttf")) {
        len = bfttf_decode(d, len);
        if (!len) { LOG("[pl] %s: not a valid .bfttf\n", path); free(d); return 0; }
    } else if (len >= 4 && !memcmp(d, "ttcf", 4)) {
        size_t l2;
        uint8_t *f0 = ttc_face0(d, len, &l2);
        free(d);
        if (!f0) { LOG("[pl] %s: bad font collection\n", path); return 0; }
        d = f0;
        len = l2;
    }
    if (check && !font_usable(d, len)) {
        LOG("[pl] %s: no glyph for a space or \"\u25A1\" (U+25A1) - skipped\n", path);
        free(d);
        return 0;
    }
    void *p = hle_alloc(len + 16);
    memcpy(p, d, len);
    free(d);
    ft->addr = (uint64_t)p;
    ft->size = len;
    LOG("[pl] system font %s (%zu bytes)\n", path, len);
    return 1;
}

static int g_fonts_loaded;
RT_ON_RESET(pl_reset) { g_fonts_loaded = 0; g_font_trace = false; __atomic_store_n(&g_font_trace_count, 0, __ATOMIC_RELAXED); memset(fonts, 0, sizeof fonts); }  // (they were in the HLE heap)

static void load_fonts(void) {
    if (g_fonts_loaded) return;
    g_fonts_loaded = 1;
    const char *env = getenv("LARECOMP_FONT");
    const char *trace = getenv("LARECOMP_FONT_TRACE");
    g_font_trace = trace && !strcmp(trace, "1");
    // per type: the console's own file(s), then a substitute of that same language
    static const char *const std[] = {"fonts/nintendo_udsg-r_std_003.bfttf", "fonts/nintendo_udsg-r_std_003.ttf",
                                     "fonts/nintendo_udsgr_std_003.ttf", "fonts/nintendo_udsgr_std_003.bfttf",
                                     "/System/Library/Fonts/ヒラギノ角ゴシック W4.ttc", "C:/Windows/Fonts/YuGothR.ttc",
                                     "C:/Windows/Fonts/YuGothM.ttc",
                                     "/System/Library/Fonts/Supplemental/Arial.ttf", "C:/Windows/Fonts/arial.ttf",
                                     // Linux: DejaVu Sans (Debian / Ubuntu, Arch, Fedora). Not Noto Sans CJK: CFF
                                     // outlines, which the game's font engine crashes on (it wants TrueType)
                                     "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                     "/usr/share/fonts/TTF/DejaVuSans.ttf",
                                     "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf", NULL};
    static const char *const zh_cn[] = {"fonts/nintendo_udsg-r_org_zh-cn_003.bfttf",
                                        "fonts/nintendo_udsg-r_org_zh-cn_003.ttf",
                                        "/System/Library/Fonts/Hiragino Sans GB.ttc",
                                        "/System/Library/Fonts/Supplemental/Hiragino Sans GB.ttc",
                                        "C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simhei.ttf",
                                        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
                                        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", NULL};
    static const char *const zh_cn_ext[] = {"fonts/nintendo_udsg-r_ext_zh-cn_003.bfttf",
                                            "fonts/nintendo_udsg-r_ext_zh-cn_003.ttf", NULL};
    static const char *const zh_tw[] = {"fonts/nintendo_udjxh-db_zh-tw_003.bfttf",
                                        "fonts/nintendo_udjxh-db_zh-tw_003.ttf", "C:/Windows/Fonts/msjh.ttc",
                                        // (PingFang carries both, but a collection copies only face 0 out: Simplified)
                                        "/System/Library/Fonts/PingFang.ttc",
                                        "/System/Library/Fonts/Supplemental/Songti.ttc",
                                        "/usr/share/fonts/truetype/arphic/uming.ttc", NULL};
    static const char *const ko[] = {"fonts/nintendo_udsg-r_ko_003.bfttf", "fonts/nintendo_udsg-r_ko_003.ttf",
                                     "/System/Library/Fonts/AppleSDGothicNeo.ttc", "C:/Windows/Fonts/malgun.ttf",
                                     "/usr/share/fonts/truetype/nanum/NanumGothic.ttf",
                                     "/usr/share/fonts/nanum/NanumGothic.ttf", NULL};
    static const char *const ext[] = {"fonts/nintendo_ext_003.bfttf", "fonts/nintendo_ext_003.ttf", NULL};
    static const char *const *const chains[N_FONT_TYPES] = {std, zh_cn, zh_cn_ext, zh_tw, ko, ext};
    extern const char *settings_language(void);
    bool cyrillic = !strcmp(settings_language(), "ru");
    for (int t = 0; t < N_FONT_TYPES; t++) {
        if (env) load_one(env, &fonts[t]);  // (a forced font: it is what the player asked for, in any language)
        for (size_t i = 0; !fonts[t].addr && chains[t][i]; i++) {
            const char *f = chains[t][i];
            if (t == F_STD && cyrillic && (strstr(f, "/System/Library/Fonts/ヒラギノ") || strstr(f, "YuGoth"))) continue;
            load_one_checked(f, &fonts[t], strncmp(f, "fonts/", 6) != 0);  // (the console's own files: as they are)
        }
    }
    char bundled[1100];
    // Preserve a user's console dump above; otherwise supply generic button icons from the packaged OFL font.
    // This is type 5 only, so standard and CJK text continue through their existing language-specific chains.
    if (!fonts[F_EXT].addr && rt_resource("shared_5.ttf", NULL, bundled, sizeof bundled))
        load_one(bundled, &fonts[F_EXT]);
    if (!fonts[F_STD].addr && rt_resource("DejaVuSans.ttf", "third_party/fonts/DejaVu/DejaVuSans.ttf", bundled, sizeof bundled))
        load_one_checked(bundled, &fonts[F_STD], true);
    if (!fonts[F_STD].addr)  // (only a broken install: the app ships DejaVu Sans)
        FATAL("no font for the game's text: import the Switch fonts in Settings (Game font), or reinstall Raitchi\n");
}

// SharedFontType: 0 Standard, 1 ChineseSimple, 2 ChineseSimpleExt, 3 ChineseTraditional, 4 Korean, 5 NintendoExt
static const Font *font_for(uint64_t type) {
    load_fonts();
    // a type this OS gave us no font for (a Linux without a CJK font, say): Standard, as a missing dump does too
    if (type >= N_FONT_TYPES || !fonts[type].addr) return &fonts[F_STD];
    return &fonts[type];
}

static void font_trace_query(const char *accessor, uint64_t requested, const Font *f) {
    if (!g_font_trace) return;
    unsigned query = __atomic_load_n(&g_font_trace_count, __ATOMIC_RELAXED);
    do {
        if (query >= 32) return;
    } while (!__atomic_compare_exchange_n(&g_font_trace_count, &query, query + 1, false,
                                          __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    unsigned resolved = F_STD;
    for (unsigned i = 0; i < N_FONT_TYPES; i++)
        if (f == &fonts[i]) { resolved = i; break; }
    LOG("[pl-font-trace] accessor=%s requested=%llu resolved=%u addr=0x%llx size=%llu\n",
        accessor, (unsigned long long)requested, resolved,
        (unsigned long long)f->addr, (unsigned long long)f->size);
}

HLE_FUNC("_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE", pl_get_load_state) { c->x[0] = 1; }
HLE_FUNC("_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE", pl_get_address) {
    uint64_t requested = c->x[0];
    const Font *f = font_for(requested);
    font_trace_query("address", requested, f);
    c->x[0] = f->addr;
}
HLE_FUNC("_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE", pl_get_size) {
    uint64_t requested = c->x[0];
    const Font *f = font_for(requested);
    font_trace_query("size", requested, f);
    c->x[0] = f->size;
}
