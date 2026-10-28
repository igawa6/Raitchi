/* Derived from LAS Recomp Android at cca791a3e7218e1dac95e7d4a11b8f97dba3ea51.
 * Copyright (c) 2026 Peiikai. MIT: native/runtime/LICENSE-LASRecomp.
 * Shared runtime: LAS-specific features run only for the selected LAS adapter. */
// Golden unchecked pots (pots randomizer): pots whose hidden item hasn't been collected are drawn tinted gold.
//
// rando/<seed>/pots.tsv (written by tools/rando/pots.py) lists every item pot: level, x, y, z, flag index.
// The renderer asks pot_tint() for each draw whose shader supports the tint (runtime/nvn/maxwell/translate.c);
// a draw is a pot when its model matrix (vertex UBO 2, 3x4 row-major) is unscaled and sits exactly on a pot
// of the current level. The pot stays gold until its item's GlobalFlags bit is set.
#include "../game_addresses.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../common_session.h"
#include "../platform.h"
#include "../rando.h"

// GA(PLAYER_STATUS_PTR): PlayerStatus*: current level name at +0
#define EVENT_FLAGS (GA(LIVE_SAVE) + 0x5DC0)  // live save: GlobalFlags bitfield (2048 bits)

// bushes.tsv rows may add: cut flag, hidden item scale, full item scale, room file (and the item's record offset in it,
// unused: see pot_regrow_tick)
typedef struct { char level[40]; float x, y, z; int flag, cut; float small, full; char room[16]; } Pot;
static Pot *g_pots;
static int g_npots = -1;
// gold = luminance remapped onto a gold ramp (keeps the pot's shading), plus a slow shimmer
// (colour target is linear HDR: lum ~0.05-0.3 on a pot, so the curve lifts it)
static float g_gold[7] = {1.85f, 1.06f, 0.09f, 1.0f, 0.50f, 0.34f, 0.03f};  // warm gold (tuned 2026-10-08)
void pot_gold_tint(float tint[8]);


static void load(void) {
    g_npots = 0;
    const char *e = getenv("LARECOMP_POT_TINT");  // r,g,b,mix,pulse_r,pulse_g,pulse_b (tuning)
    if (e) sscanf(e, "%f,%f,%f,%f,%f,%f,%f", &g_gold[0], &g_gold[1], &g_gold[2], &g_gold[3], &g_gold[4], &g_gold[5], &g_gold[6]);
    const char *dir = rando_seed_dir();
    if (!dir) return;
    int cap = 0;
    // pots.tsv (potsanity) and bushes.tsv (bushsanity, flags 2048+: runtime/hle/flags_ext.c): same format
    for (int k = 0; k < 2; k++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, k ? "bushes.tsv" : "pots.tsv");
        FILE *f = fopen(path, "r");
        if (!f) continue;
        int n0 = g_npots;
        char line[256];
        while (fgets(line, sizeof line, f)) {
            Pot p = {.cut = -1};
            if (sscanf(line, "%39s %f %f %f %d %d %f %f %15s", p.level, &p.x, &p.y, &p.z, &p.flag, &p.cut, &p.small,
                       &p.full, p.room) < 5) continue;
            if (g_npots == cap) g_pots = realloc(g_pots, sizeof(Pot) * (cap = cap ? cap * 2 : 256));
            g_pots[g_npots++] = p;
        }
        fclose(f);
        LOG("[pots] %d gold %s from %s\n", g_npots - n0, k ? "bushes" : "pots", path);
    }
}

static const char *current_level(void) {
    uint64_t ps = LD64(GA(PLAYER_STATUS_PTR));
    return ps ? (const char *)(uintptr_t)ps : "";
}

// Tint: rgb = mix(rgb, t0.rgb * lum^t1.w + t1.rgb * lum, t0.w)  (runtime/nvn/maxwell/translate.c).
// m: the draw's model matrix (3x4 row-major). Fills tint (float4[2]) and returns true for an unchecked pot.
// Pots of the current level, sorted by x (draws binary-search them); rebuilt when the level changes. (The Field has
// ~920 bushes: a fixed 512-entry list left most of them never gold, and a linear scan per draw was slow.)
typedef struct { int pot; float x, y, z; uint64_t seen, mesh; } Track;
static Track g_track[8];  // carried pots (see below)
static int *g_lv_idx, g_nlv = 0;
static char g_lv_name[40] = "\x01";
static uint64_t g_lv_frame = ~0ull;
// Reboot: the next seed's pots.tsv / bushes.tsv.
RT_ON_RESET(pot_glow_reset) {
    free(g_pots), free(g_lv_idx);
    g_pots = NULL, g_lv_idx = NULL;
    g_npots = -1, g_nlv = 0;
    strlcpy(g_lv_name, "\x01", sizeof g_lv_name);
    g_lv_frame = ~0ull;
    memset(g_track, 0, sizeof g_track);
}

static int by_x(const void *a, const void *b) {
    float d = g_pots[*(const int *)a].x - g_pots[*(const int *)b].x;
    return d < 0 ? -1 : d > 0;
}
static void level_pots(void) {
    extern uint64_t renderer_present_count(void);
    uint64_t f = renderer_present_count();
    if (f == g_lv_frame) return;
    g_lv_frame = f;
    const char *lv = current_level();
    if (strncmp(lv, g_lv_name, sizeof g_lv_name)) {
        strlcpy(g_lv_name, lv, sizeof g_lv_name);
        if (!g_lv_idx) g_lv_idx = malloc(sizeof(int) * (size_t)(g_npots ? g_npots : 1));
        g_nlv = 0;
        for (int i = 0; i < g_npots; i++)
            if (!strcmp(g_pots[i].level, lv)) g_lv_idx[g_nlv++] = i;
        qsort(g_lv_idx, (size_t)g_nlv, sizeof(int), by_x);
    }
    if (ENV("LARECOMP_LOG_POTS") && f % 120 == 0) LOG("[pots] frame %llu level '%s': %d item pots\n", (unsigned long long)f, lv, g_nlv);
}

// Cheap per-draw gate for the renderer: false when no item pot can be on screen.
bool pot_glow_active(void) {
    if (!shared_title_las()) return false;
    if (g_npots < 0) load();
    if (!g_npots) return false;
    level_pots();
    return g_nlv > 0;
}

// Carried pots: a gold pot that Link lifts leaves its spot, so it's followed from frame to frame: an unscaled
// draw (rotation allowed) within a short step of where a tracked gold pot was drawn in the last couple of frames
// with the same mesh (vertex buffer) is that pot. Pots stand a tile apart, so a step this small can't jump to a
// neighbour, and the mesh check keeps other draws (full-screen passes, Link) out.

bool flags_ext_get(int idx);  // runtime/hle/flags_ext.c (flags 2048+ for bushes)
void flags_ext_clear(int idx);
static bool unchecked(const Pot *p) { return !flags_ext_get(p->flag); }

static void track(int pot, float x, float y, float z, uint64_t f, uint64_t mesh) {
    int slot = 0;
    for (int i = 0; i < 8; i++) {
        if (g_track[i].pot == pot && g_track[i].seen) { slot = i; break; }
        if (g_track[i].seen < g_track[slot].seen) slot = i;
    }
    g_track[slot] = (Track){pot, x, y, z, f, mesh};
}

bool pot_tint(const float *m, float tint[8], uint64_t mesh) {
    if (!pot_glow_active()) return false;
    extern uint64_t renderer_present_count(void);
    uint64_t now = renderer_present_count();
    // unit scale on every axis (the hidden item inside is scaled down)
    float sx = m[0] * m[0] + m[4] * m[4] + m[8] * m[8], sy = m[1] * m[1] + m[5] * m[5] + m[9] * m[9],
          sz = m[2] * m[2] + m[6] * m[6] + m[10] * m[10];
    if (fabsf(sx - 1) > 0.02f || fabsf(sy - 1) > 0.02f || fabsf(sz - 1) > 0.02f) return false;
    bool upright = fabsf(m[0] - 1) <= 0.01f && fabsf(m[5] - 1) <= 0.01f && fabsf(m[10] - 1) <= 0.01f;
    float x = m[3], y = m[7], z = m[11];
    const Pot *hit = NULL;
    int lo = 0, hi = g_nlv;  // first pot with x >= x - 0.02
    while (lo < hi) { int mid = (lo + hi) / 2; if (g_pots[g_lv_idx[mid]].x < x - 0.02f) lo = mid + 1; else hi = mid; }
    for (int k = lo; k < g_nlv && upright && g_pots[g_lv_idx[k]].x <= x + 0.02f; k++) {  // standing on its spot
        const Pot *p = &g_pots[g_lv_idx[k]];
        if (fabsf(p->x - x) > 0.02f || fabsf(p->z - z) > 0.02f || fabsf(p->y - y) > 0.05f) continue;
        if (!unchecked(p)) return false;  // already checked
        hit = p;
        track(g_lv_idx[k], x, y, z, now, mesh);
        break;
    }
    for (int i = 0; i < 8 && !hit; i++) {  // being carried / thrown
        Track *t = &g_track[i];
        if (!t->seen || now - t->seen > 2 || !mesh || t->mesh != mesh || !unchecked(&g_pots[t->pot])) continue;
        if (strcmp(g_pots[t->pot].level, current_level())) { t->seen = 0; continue; }
        float dx = t->x - x, dy = t->y - y, dz = t->z - z;
        if (dx * dx + dy * dy + dz * dz > 0.25f * 0.25f) continue;
        hit = &g_pots[t->pot];
        t->x = x; t->y = y; t->z = z; t->seen = now;
    }
    if (hit) {
        pot_gold_tint(tint);
        return true;
    }
    return false;
}

// Bushsanity: a bush with an item has a cut flag as its switch 0 (tools/rando/bushes.py: the game sets it when the bush
// is cut, and keeps such a bush cut). Bushes still grow back as in the game: once Link is more than a screen away (or in
// another level) the cut flag is cleared, so the bush is there on his next visit - gold until its item is taken, and
// cutting it shows the same item again. (The item itself: rt_bush_item_skip / pot_bush_game_tick below. It comes full
// size through the actor factory, so the room file's record of it is never patched: that took a pass over every page
// of guest data on the game thread, 5-10 ms, at each cut - a one-frame hitch per bush.)
uint64_t doorjump_player(uint64_t frame);  // runtime/hle/doorjump.c
static int g_regrow_cell = -1;
RT_ON_RESET(pot_regrow_reset) { g_regrow_cell = -1; }

void pot_regrow_tick(uint64_t frame) {
    if (!pot_glow_active()) return;
    uint64_t pc = doorjump_player(frame);
    if (!pc) return;
    const float *lp = (const float *)(uintptr_t)(pc + 0x80);
    int cx = (int)floorf(lp[0] / 15.0f), cz = (int)floorf(lp[2] / 12.0f), cell = cx * 64 + cz;
    static char level[40];
    bool new_level = strncmp(current_level(), level, sizeof level) != 0;
    if (!new_level && cell == g_regrow_cell) return;
    g_regrow_cell = cell;  // bushes regrow (as in the game): Link left their screen
    strlcpy(level, current_level(), sizeof level);
    for (int i = 0; i < g_npots; i++) {
        const Pot *p = &g_pots[i];
        if (p->cut < 0 || !flags_ext_get(p->cut)) continue;
        if (!new_level && !strcmp(p->level, level) && abs((int)floorf(p->x / 15.0f) - cx) <= 1 &&
            abs((int)floorf(p->z / 12.0f) - cz) <= 1) continue;  // (its room may still be loaded: next time)
        flags_ext_clear(p->cut);
    }
}

// A bush's item only exists once the bush is cut. It used to stand hidden inside the bush, shrunk - but shrinking
// doesn't shrink what Link touches to pick it up: pressed against the bush (from below, cutting the one next to it)
// he took the item through the leaves. The gold bush then went green for an item that seemed to come out of another
// bush, and that other bush stayed gold, its own item never taken (user reports, beta 5). Now the level's spawn of
// the item is skipped while the bush stands (rt_bush_item_skip, from the actor factory hook in runtime/hle/souls.c)
// and its spawn info kept; once the bush's cut flag is set the item is spawned into the bush's screen with it, full
// size (pot_bush_game_tick). A bush cut when its room loads lets the item come with the room, full size.
#define T_SINKING_SWORD 0x194
#define BUSH_KEY_TAG 0x7Bu  // (tools/rando/bushes.py key_tag: the items' actor keys)
#define SPAWN_INFO_SIZE 0x1c0  // spawn info: +0 key, +8 type, +0xc zone, +0x10 parameters, +0x180 position / rotation / scale
static uint8_t **g_info;  // per spot: the skipped spawn of its item (bush not cut yet)
static uint64_t *g_item;  // per spot: weak ref (control block) of the item we spawned
static uint64_t g_spawn_scratch;
RT_ON_RESET(pot_bush_reset) {  // (the infos and refs were the old boot's)
    if (g_info)
        for (int i = 0; i < g_npots; i++) free(g_info[i]);
    free(g_info), free(g_item);
    g_info = NULL, g_item = NULL;
    g_spawn_scratch = 0;
}

bool rt_bush_item_skip(uint32_t type, uint64_t info) {
    if (!shared_title_las()) return false;
    if (type != T_SINKING_SWORD || (LD64(info) >> 56) != BUSH_KEY_TAG) return false;
    if (g_npots < 0) load();
    if (g_npots <= 0) return false;
    const float *pos = (const float *)(uintptr_t)(info + 0x180);
    const char *lv = current_level();
    for (int i = 0; i < g_npots; i++) {
        Pot *p = &g_pots[i];
        if (p->cut < 0 || p->x != pos[0] || p->y != pos[1] || p->z != pos[2] || strcmp(p->level, lv)) continue;
        if (!g_info) g_info = calloc((size_t)g_npots, sizeof *g_info), g_item = calloc((size_t)g_npots, sizeof *g_item);
        float *scale = (float *)(uintptr_t)(info + 0x180 + 0x20);
        if (flags_ext_get(p->cut) || !unchecked(p)) {  // cut already (or taken): as the room has it, full size
            if (unchecked(p)) scale[0] = scale[1] = scale[2] = p->full;
            free(g_info[i]);
            g_info[i] = NULL;
            return false;
        }
        if (!g_info[i]) g_info[i] = malloc(SPAWN_INFO_SIZE);
        memcpy(g_info[i], (const void *)(uintptr_t)info, SPAWN_INFO_SIZE);
        scale = (float *)(g_info[i] + 0x180 + 0x20);
        scale[0] = scale[1] = scale[2] = p->full;
        return true;
    }
    return false;
}

static bool weak_alive(uint64_t ctrl) { return ctrl && (int64_t)LD64(ctrl + 8) >= 0; }

static void weak_drop(Cpu *c, uint64_t *slot) {
    if (!*slot) return;
    c->x[0] = *slot;
    *slot = 0;
    guest_call(c, GA(WEAK_RELEASE));
}

static bool spawn_bush_item(Cpu *c, int i) {
    bool digs_screen_room(Cpu *c, uint64_t pair, int screen, uint64_t *sctrl);  // runtime/hle/digs.c
    const Pot *p = &g_pots[i];
    int row = 0;
    char col = 0;
    if (sscanf(p->room, "Field_%2d%c", &row, &col) != 2 || row < 1 || row > 16 || col < 'A' || col > 'P') return true;
    if (!g_spawn_scratch) g_spawn_scratch = (uint64_t)(uintptr_t)hle_alloc(0x400);
    uint64_t s = g_spawn_scratch, info = s + 0x40, sctrl;
    memset((void *)(uintptr_t)s, 0, 0x400);
    memcpy((void *)(uintptr_t)info, g_info[i], SPAWN_INFO_SIZE);
    bool loaded = digs_screen_room(c, s + 0x10, (row - 1) * 16 + (col - 'A'), &sctrl);
    if (loaded) {
        c->x[0] = s;
        c->x[1] = s + 0x10;
        c->x[2] = info;
        guest_call(c, GA(CREATE_ACTOR));
        uint64_t ictrl = LD64(s + 8);
        if (LD64(s) && ictrl) {
            __atomic_fetch_add((int64_t *)(uintptr_t)(ictrl + 0x10), 1, __ATOMIC_ACQ_REL);  // (our weak ref)
            g_item[i] = ictrl;
            c->x[0] = ictrl;
            guest_call(c, GA(SP_RELEASE));
        }
        LOG("[pots] bush %s cut: its item (flag %d) %s\n", p->room, p->flag, LD64(s) ? "spawned" : "failed to spawn");
        { extern void pace_hitch(const char *why);  // nvn/pace.c
          pace_hitch("an item spawn"); }
    }
    if (sctrl) { c->x[0] = sctrl; guest_call(c, GA(SP_RELEASE)); }
    return loaded;
}

// game thread, once per frame (runtime/hle/input.c)
void pot_bush_game_tick(Cpu *c, uint64_t frame) {
    if (!shared_title_las()) return;
    if (!g_info || frame % 4) return;
    extern int game_in_play(void);
    if (!game_in_play()) return;
    uint64_t saved[31], sp = c->sp, pc = c->pc;
    memcpy(saved, c->x, sizeof saved);
    const char *lv = current_level();
    for (int i = 0; i < g_npots; i++) {
        if (g_item[i] && !weak_alive(g_item[i])) weak_drop(c, &g_item[i]);  // (picked up, or its room unloaded)
        if (!g_info[i] || strcmp(g_pots[i].level, lv) || !flags_ext_get(g_pots[i].cut) || !unchecked(&g_pots[i])) continue;
        if (g_item[i]) continue;
        if (spawn_bush_item(c, i)) free(g_info[i]), g_info[i] = NULL;  // (once: a reload of the room keeps its own)
    }
    memcpy(c->x, saved, sizeof saved);
    c->sp = sp;
    c->pc = pc;
}

// The item that was out already when its bush was cut keeps the scale it spawned with: until the room is loaded again
// its draws are scaled up here instead (m: the draw's model matrix, vertex UBO 2, 3x4 row-major, in guest memory -
// scaled in place, so only a matrix still at the hidden scale is touched: a second pass over the same UBO, e.g. the
// shadow map's, leaves it alone). Rigid item models only; the rest show full size from the next room load on.
void pot_item_draw(float *m) {
    if (!pot_glow_active()) return;
    float s2 = m[0] * m[0] + m[4] * m[4] + m[8] * m[8];
    if (s2 > 0.81f || s2 < 1e-4f) return;  // (hidden items are drawn below 0.9)
    float s = sqrtf(s2), x = m[3], y = m[7], z = m[11];
    int lo = 0, hi = g_nlv;
    while (lo < hi) { int mid = (lo + hi) / 2; if (g_pots[g_lv_idx[mid]].x < x - 0.02f) lo = mid + 1; else hi = mid; }
    for (int k = lo; k < g_nlv && g_pots[g_lv_idx[k]].x <= x + 0.02f; k++) {
        const Pot *p = &g_pots[g_lv_idx[k]];
        if (p->cut < 0 || fabsf(p->z - z) > 0.02f || fabsf(p->y - y) > 1.0f || fabsf(s - p->small) > 0.01f) continue;
        if (!flags_ext_get(p->cut) || !unchecked(p)) return;
        for (int i = 0; i < 11; i++)
            if ((i & 3) != 3) m[i] *= p->full / p->small;
        return;
    }
}

// Instanced draws (bushes: one draw per batch, each instance's 4x4 row-major model matrix in vertex storage buffer
// 0, random rotation about y): true when this instance stands on an unchecked item spot of the current level.
bool pot_instance_gold(const float *m) {
    if (!shared_title_las()) return false;
    float sx = m[0] * m[0] + m[4] * m[4] + m[8] * m[8], sy = m[1] * m[1] + m[5] * m[5] + m[9] * m[9];
    if (fabsf(sx - 1) > 0.05f || fabsf(sy - 1) > 0.05f) return false;
    float x = m[3], y = m[7], z = m[11];
    int lo = 0, hi = g_nlv;
    while (lo < hi) { int mid = (lo + hi) / 2; if (g_pots[g_lv_idx[mid]].x < x - 0.02f) lo = mid + 1; else hi = mid; }
    for (int k = lo; k < g_nlv && g_pots[g_lv_idx[k]].x <= x + 0.02f; k++) {
        const Pot *p = &g_pots[g_lv_idx[k]];
        if (fabsf(p->z - z) > 0.02f || fabsf(p->y - y) > 0.05f) continue;
        return unchecked(p);
    }
    return false;
}

// the tint pot_tint gives a gold pot this frame
void pot_gold_tint(float tint[8]) {
    if (!shared_title_las()) { memset(tint, 0, sizeof(float) * 8); return; }
    extern uint64_t renderer_present_count(void);
    float t = (float)renderer_present_count() / 60.0f;
    float pulse = 0.5f + 0.5f * sinf(t * 2.5f);
    tint[0] = g_gold[0]; tint[1] = g_gold[1]; tint[2] = g_gold[2]; tint[3] = g_gold[3];
    tint[4] = g_gold[4] * pulse; tint[5] = g_gold[5] * pulse; tint[6] = g_gold[6] * pulse; tint[7] = 0.8f;
}

// True when the current level has item pots: their hidden item actors' 2D twinkle is drawn at a bogus spot
// (it projects a bone the swapped-in item models don't have), so the renderer drops it in these levels.
bool pot_level_active(void) {
    if (!shared_title_las()) return false;
    if (g_npots < 0) load();
    if (!g_npots) return false;
    static uint64_t frame = ~0ull;
    static bool active;
    extern uint64_t renderer_present_count(void);
    if (renderer_present_count() == frame) return active;
    frame = renderer_present_count();
    level_pots();
    active = g_nlv > 0;
    return active;
}

// LARECOMP_POT_PROBE=1 (debug): log draws whose model matrix sits within a tile of an item pot / bush of this level
// (whatever their scale / rotation / shader) - finds how a kind of object is drawn.
void pot_probe(const float *m, bool tintable, uint64_t mesh, uint32_t inst) {
    if (!pot_glow_active()) return;
    extern uint64_t renderer_present_count(void);
    static uint64_t frame;
    static int n;
    uint64_t f = renderer_present_count();
    if (f != frame) { frame = f; n = 0; }
    if (f % 300 || n >= 24) return;
    float x = m[3], y = m[7], z = m[11];
    for (int k = 0; k < g_nlv; k++) {
        const Pot *p = &g_pots[g_lv_idx[k]];
        if (fabsf(p->x - x) > 1.0f || fabsf(p->z - z) > 1.0f || fabsf(p->y - y) > 2.0f) continue;
        n++;
        LOG("[potprobe] f%llu pot (%.2f %.2f %.2f) draw (%.2f %.2f %.2f) diag %.3f %.3f %.3f tint %d mesh %llx arg3 %u\n",
            (unsigned long long)f, p->x, p->y, p->z, x, y, z, m[0], m[5], m[10], tintable, (unsigned long long)mesh, inst);
        break;
    }
}
