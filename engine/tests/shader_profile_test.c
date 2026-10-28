/* Asset-free metadata profile production tests. All shader spans are synthetic. */
#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "shader_profile.h"

bool shared_sha256_bytes(const void *, size_t, uint8_t[32]);
static unsigned checks, scenarios;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); abort(); } } while (0)
static uint8_t vbytes[144], fbytes[136], cbytes[40], game[32];

static NvnDiscoveryRecipe graphics(bool depth_only) {
    NvnDiscoveryRecipe r = {0};
    r.rec.kind = 'G'; r.rec.vs = nvn_disc_codehash(vbytes, sizeof vbytes);
    r.rec.fs = depth_only ? 0 : nvn_disc_codehash(fbytes, sizeof fbytes);
    r.rec.flags = SR_FLIP_Y | SR_DEPTH_MINUS_ONE | SR_FLIP_X;
    r.rec.nattrib = 2; r.rec.nstream = 1; r.rec.ncolor = depth_only ? 0 : 1;
    r.rec.attrib[0] = 0x1c; r.rec.attrib[1] = 0x3c;
    r.rec.stream[0][0] = 32; r.rec.stream[0][1] = 1;
    if (!depth_only) { r.rec.color[0] = 0x14; r.rec.cview[0] = 0x18; }
    r.rec.depth = 0x8a; r.rec.samples = 4; r.rec.channel_mask = 0xf;
    r.rec.blend_on = 1; r.rec.blend[0][0] = 4; r.rec.a2c = 1; r.rec.topo = 2; r.rec.poly_fill = 1;
    r.vkey.glsl = r.fkey.glsl = 1;
    r.vkey.flip_x = r.fkey.flip_x = r.vkey.flip_y = r.fkey.flip_y = true;
    r.vkey.depth_minus_one = true; r.vkey.attr_kind[0] = 2; r.vkey.attr_kind[1] = 1;
    if (!depth_only) r.fkey.rt_kind[0] = 2;
    r.vkey.tex_uint = 0x12345678; r.fkey.tex_uint = 0x80000001;
    r.code[0] = vbytes; r.code_bytes[0] = 128; r.constant_bytes[0] = sizeof vbytes - 128;
    if (!depth_only) { r.code[1] = fbytes; r.code_bytes[1] = 128; r.constant_bytes[1] = sizeof fbytes - 128; }
    return r;
}
static NvnDiscoveryRecipe compute(void) {
    NvnDiscoveryRecipe r = {0}; r.rec.kind = 'C'; r.rec.vs = nvn_disc_codehash(cbytes, sizeof cbytes);
    r.rec.local[0] = 32; r.rec.local[1] = 2; r.rec.local[2] = 1; r.vkey.glsl = 1;
    r.code[0] = cbytes; r.code_bytes[0] = 32; r.constant_bytes[0] = sizeof cbytes - 32;
    return r;
}
static void save(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb"); CHECK(f); CHECK(fwrite(data, 1, n, f) == n); CHECK(!fclose(f));
}
static uint8_t *load(const char *path, size_t *n) {
    struct stat st; CHECK(!stat(path, &st) && st.st_size >= 0); *n = (size_t)st.st_size;
    uint8_t *b = malloc(*n ? *n : 1); CHECK(b); FILE *f = fopen(path, "rb"); CHECK(f);
    CHECK(fread(b, 1, *n, f) == *n); CHECK(!fclose(f)); return b;
}
static void repair(uint8_t *b, size_t n) {
    for (size_t at = SHADER_PROFILE_HEADER_BYTES; at < n; at += SHADER_PROFILE_RECORD_BYTES)
        CHECK(shared_sha256_bytes(b + at, SHADER_PROFILE_METADATA_BYTES, b + at + SHADER_PROFILE_METADATA_BYTES));
    CHECK(shared_sha256_bytes(b + SHADER_PROFILE_HEADER_BYTES, n - SHADER_PROFILE_HEADER_BYTES, b + 56));
    CHECK(shared_sha256_bytes(b, 88, b + 88));
}
typedef struct { unsigned calls; bool fail, alter; volatile int *cancel; } Resolver;
static bool resolve(void *context, unsigned stage, uint64_t hash, uint32_t code, uint32_t constants, uint8_t **out) {
    Resolver *r = context; r->calls++; CHECK(!*out);
    const uint8_t *source = stage == STAGE_VERTEX ? vbytes : stage == STAGE_FRAGMENT ? fbytes : stage == STAGE_COMPUTE ? cbytes : NULL;
    size_t n = stage == STAGE_VERTEX ? sizeof vbytes : stage == STAGE_FRAGMENT ? sizeof fbytes : sizeof cbytes;
    CHECK(source && (size_t)code + constants == n && hash == nvn_disc_codehash(source, n));
    if (r->fail) return false;
    *out = malloc(n); CHECK(*out); memcpy(*out, source, n);
    if (r->alter) (*out)[n - 1] ^= 1;
    if (r->cancel) *r->cancel = 1;
    return true;
}
static void open_empty(NvnDiscoveryStore *store, const char *path) {
    CHECK(nvn_discovery_init_empty(store, path, game, shared_sha256_bytes, 8 * 1024 * 1024, 4096));
}
static void compare_recipe(const NvnDiscoveryRecipe *a, const NvnDiscoveryRecipe *b) {
    uint8_t *aw = NULL, *bw = NULL, ah[32], bh[32]; size_t an = 0, bn = 0;
    CHECK(nvn_disc_payload_make(a, shared_sha256_bytes, &aw, &an, ah));
    CHECK(nvn_disc_payload_make(b, shared_sha256_bytes, &bw, &bn, bh));
    CHECK(an == bn && !memcmp(aw, bw, an) && !memcmp(ah, bh, 32)); free(aw); free(bw);
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    for (size_t i = 0; i < sizeof vbytes; i++) vbytes[i] = (uint8_t)(0xa7 + i * 7 + i / 3);
    for (size_t i = 0; i < sizeof fbytes; i++) fbytes[i] = (uint8_t)(0x31 + i * 13 + i / 2);
    for (size_t i = 0; i < sizeof cbytes; i++) cbytes[i] = (uint8_t)(0xc2 + i * 11);
    for (size_t i = 0; i < sizeof game; i++) game[i] = (uint8_t)(0x21 + i * 3);
    char profile_path[1024], learned_path[1024], target_path[1024], bad_path[1024], dir_path[1024], keep_path[1100];
    snprintf(profile_path, sizeof profile_path, "%s/profile.bin", argv[1]);
    snprintf(learned_path, sizeof learned_path, "%s/learned.bin", argv[1]);
    snprintf(target_path, sizeof target_path, "%s/target.bin", argv[1]);
    snprintf(bad_path, sizeof bad_path, "%s/bad.bin", argv[1]);
    snprintf(dir_path, sizeof dir_path, "%s/directory", argv[1]);
    snprintf(keep_path, sizeof keep_path, "%s/keep", dir_path);
    NvnDiscoveryRecipe recipes[3] = {graphics(false), compute(), graphics(true)};
    NvnDiscoveryStore learned = {0}, target = {0}; open_empty(&learned, learned_path);
    for (unsigned i = 0; i < 3; i++) CHECK(nvn_discovery_append(&learned, &recipes[i]) == NVN_DISCOVERY_APPEND_APPENDED);
    CHECK(nvn_discovery_commit(&learned)); CHECK(shader_profile_export(profile_path, &learned)); scenarios++;
    size_t original_n = 0; uint8_t *original = load(profile_path, &original_n);
    CHECK(original_n == SHADER_PROFILE_HEADER_BYTES + 3 * SHADER_PROFILE_RECORD_BYTES);
    CHECK(!memcmp(original, "NVNPROF", 7) && nvn_disc_get32(original + 8) == 1 && nvn_disc_get32(original + 16) == 3);
    CHECK(!memmem(original, original_n, vbytes, sizeof vbytes));
    CHECK(!memmem(original, original_n, fbytes, sizeof fbytes));
    CHECK(!memmem(original, original_n, cbytes, sizeof cbytes)); scenarios++;
    struct stat before, after; CHECK(!stat(profile_path, &before));
    CHECK(shader_profile_export(profile_path, &learned)); CHECK(!stat(profile_path, &after));
    CHECK(before.st_ino == after.st_ino && before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec); scenarios++;
    ShaderProfile p = {0}; CHECK(shader_profile_load(profile_path, game, shared_sha256_bytes, &p) == SHADER_PROFILE_LOADED);
    CHECK(p.count == 3 && p.entries[0].vkey.tex_uint == recipes[0].vkey.tex_uint && p.entries[0].fkey.tex_uint == recipes[0].fkey.tex_uint);
    CHECK(p.entries[1].rec.local[0] == 32 && p.entries[2].rec.fs == 0); scenarios++;
    size_t missing = 99; CHECK(shader_profile_missing_count(&p, &learned, &missing) && missing == 0);
    Resolver r = {.fail = true}; ShaderProfileMergeStats stats;
    CHECK(shader_profile_merge(&p, &learned, resolve, &r, NULL, &stats));
    CHECK(stats.reused == 3 && !stats.appended && !stats.missing && !stats.rejected && !r.calls); scenarios++;
    open_empty(&target, target_path); CHECK(shader_profile_missing_count(&p, &target, &missing) && missing == 3);
    r = (Resolver){0}; CHECK(shader_profile_merge(&p, &target, resolve, &r, NULL, &stats));
    CHECK(stats.appended == 3 && !stats.reused && !stats.missing && !stats.rejected && r.calls == 4);
    for (size_t i = 0; i < 3; i++) { NvnDiscoveryRecipe got = {0}; CHECK(nvn_discovery_read_at(&target, i, &got)); compare_recipe(&recipes[i], &got); nvn_discovery_recipe_release(&got); }
    CHECK(nvn_discovery_commit(&target)); nvn_discovery_close(&target);
    CHECK(nvn_discovery_open(&target, target_path, game, shared_sha256_bytes, 8 * 1024 * 1024, 4096) == NVN_DISCOVERY_OPEN_LOADED);
    CHECK(shader_profile_missing_count(&p, &target, &missing) && !missing); scenarios++;
    ShaderProfileEntry duplicate_entries[6]; for (unsigned i = 0; i < 6; i++) duplicate_entries[i] = p.entries[i % 3];
    ShaderProfile dup = {.entries = duplicate_entries, .count = 6}; memcpy(dup.game_id, game, 32);
    r = (Resolver){.fail = true}; CHECK(shader_profile_merge(&dup, &target, resolve, &r, NULL, &stats)); CHECK(stats.reused == 6 && !r.calls); scenarios++;
    nvn_discovery_close(&target); open_empty(&target, target_path);
    r = (Resolver){0}; CHECK(shader_profile_merge(&dup, &target, resolve, &r, NULL, &stats)); CHECK(stats.appended == 3 && stats.reused == 3 && r.calls == 4); scenarios++;
    nvn_discovery_close(&target); open_empty(&target, target_path);
    r = (Resolver){.fail = true}; CHECK(shader_profile_merge(&p, &target, resolve, &r, NULL, &stats)); CHECK(stats.missing == 3 && !target.count); scenarios++;
    r = (Resolver){.alter = true}; CHECK(shader_profile_merge(&p, &target, resolve, &r, NULL, &stats)); CHECK(stats.rejected == 3 && !target.count); scenarios++;
    volatile int cancel = 1; r = (Resolver){0}; CHECK(shader_profile_merge(&p, &target, resolve, &r, &cancel, &stats)); CHECK(stats.cancelled && !r.calls && !target.count); scenarios++;
    cancel = 0; r = (Resolver){.cancel = &cancel}; CHECK(shader_profile_merge(&p, &target, resolve, &r, &cancel, &stats)); CHECK(stats.cancelled && r.calls == 1 && !target.count); scenarios++;
    uint8_t other[32]; memcpy(other, game, 32); other[9] ^= 1;
    ShaderProfile wrong = p; memcpy(wrong.game_id, other, 32); r = (Resolver){0};
    CHECK(!shader_profile_merge(&wrong, &target, resolve, &r, NULL, &stats) && !target.count && !r.calls);
    CHECK(!shader_profile_missing_count(&wrong, &target, &missing));
    ShaderProfile q = {0}; CHECK(shader_profile_load(profile_path, other, shared_sha256_bytes, &q) == SHADER_PROFILE_INCOMPATIBLE && !q.entries); scenarios++;
    CHECK(shader_profile_load(bad_path, game, shared_sha256_bytes, &q) == SHADER_PROFILE_MISSING); scenarios++;
    uint8_t *bad = malloc(original_n); CHECK(bad); memcpy(bad, original, original_n); bad[8] = 2; save(bad_path, bad, original_n);
    CHECK(shader_profile_load(bad_path, game, shared_sha256_bytes, &q) == SHADER_PROFILE_INCOMPATIBLE); scenarios++;
    memcpy(bad, original, original_n); bad[SHADER_PROFILE_HEADER_BYTES + 20] ^= 1; save(bad_path, bad, original_n);
    CHECK(shader_profile_load(bad_path, game, shared_sha256_bytes, &q) == SHADER_PROFILE_CORRUPT); scenarios++;
    for (unsigned which = 0; which < 7; which++) {
        memcpy(bad, original, original_n); uint8_t *record = bad + SHADER_PROFILE_HEADER_BYTES;
        switch (which) {
        case 0: record[18] = 17; break; /* nattrib */
        case 1: record[369] = 3; break; /* invalid attr kind */
        case 2: record[369 + 48 + 42] = 0; break; /* uncoupled fragment flip_x */
        case 3: record[465] = 1; record[466] = record[467] = record[468] = 0; break; /* short graphics */
        case 4: memset(record + 465, 0xff, 8); break; /* code + constant overflow */
        case 5: record[369 + 40] = 2; break; /* invalid bool */
        case 6: record[369 + 48 + 40] = 1; break; /* fragment depth mode */
        }
        repair(bad, original_n); save(bad_path, bad, original_n);
        CHECK(shader_profile_load(bad_path, game, shared_sha256_bytes, &q) == SHADER_PROFILE_CORRUPT && !q.entries);
    } scenarios++;
    memcpy(bad, original, original_n); nvn_disc_put32(bad + 16, NVN_DISCOVERY_MAX_RECORDS + 1); save(bad_path, bad, original_n);
    CHECK(shader_profile_load(bad_path, game, shared_sha256_bytes, &q) == SHADER_PROFILE_LIMIT); scenarios++;
    save(bad_path, original, original_n - 1); CHECK(shader_profile_load(bad_path, game, shared_sha256_bytes, &q) == SHADER_PROFILE_CORRUPT); scenarios++;
    save(bad_path, original, original_n); FILE *extra = fopen(bad_path, "ab"); CHECK(extra); CHECK(fputc(0, extra) == 0); CHECK(!fclose(extra));
    CHECK(shader_profile_load(bad_path, game, shared_sha256_bytes, &q) == SHADER_PROFILE_CORRUPT); scenarios++;
    CHECK(!mkdir(dir_path, 0700)); save(keep_path, "keep", 4); CHECK(!shader_profile_export(dir_path, &learned));
    size_t keep_n; uint8_t *keep = load(keep_path, &keep_n); CHECK(keep_n == 4 && !memcmp(keep, "keep", 4)); free(keep); scenarios++;
    ShaderProfileEntry invalid = p.entries[0]; invalid.vkey.vs_out_mask = 1; CHECK(!shader_profile_entry_valid(&invalid));
    invalid = p.entries[1]; invalid.fkey.glsl = 1; CHECK(!shader_profile_entry_valid(&invalid));
    invalid = p.entries[2]; invalid.code_bytes[1] = 128; CHECK(!shader_profile_entry_valid(&invalid)); scenarios++;
    ShaderProfile bad_in_memory = p; bad_in_memory.count = NVN_DISCOVERY_MAX_RECORDS + 1;
    CHECK(!shader_profile_merge(&bad_in_memory, &target, resolve, &r, NULL, &stats)); scenarios++;
    /* Exporting a partially resolved/learned subset must retain other recipes
     * already present in a supplied profile, then add genuinely new variants. */
    char union_path[1024]; snprintf(union_path, sizeof union_path, "%s/union.bin", argv[1]);
    save(union_path, original, original_n); nvn_discovery_close(&target); open_empty(&target, target_path);
    CHECK(nvn_discovery_append(&target, &recipes[0]) == NVN_DISCOVERY_APPEND_APPENDED);
    CHECK(shader_profile_export_union(union_path, &target));
    CHECK(shader_profile_load(union_path, game, shared_sha256_bytes, &q) == SHADER_PROFILE_LOADED && q.count == 3);
    shader_profile_close(&q);
    NvnDiscoveryRecipe extra_recipe = recipes[0]; extra_recipe.rec.channel_mask ^= 1;
    CHECK(nvn_discovery_append(&target, &extra_recipe) == NVN_DISCOVERY_APPEND_APPENDED);
    CHECK(shader_profile_export_union(union_path, &target));
    CHECK(shader_profile_load(union_path, game, shared_sha256_bytes, &q) == SHADER_PROFILE_LOADED && q.count == 4);
    shader_profile_close(&q); CHECK(!stat(union_path, &before)); CHECK(shader_profile_export_union(union_path, &target));
    CHECK(!stat(union_path, &after) && before.st_ino == after.st_ino); scenarios++;
    size_t union_n; uint8_t *union_bytes = load(union_path, &union_n);
    target.game_id[0] ^= 1; CHECK(!shader_profile_export_union(union_path, &target)); target.game_id[0] ^= 1;
    size_t kept_n; uint8_t *kept_bytes = load(union_path, &kept_n);
    CHECK(kept_n == union_n && !memcmp(kept_bytes, union_bytes, kept_n)); free(kept_bytes);
    union_bytes[union_n - 1] ^= 1; save(union_path, union_bytes, union_n);
    CHECK(!shader_profile_export_union(union_path, &target)); kept_bytes = load(union_path, &kept_n);
    CHECK(kept_n == union_n && !memcmp(kept_bytes, union_bytes, kept_n)); free(kept_bytes); free(union_bytes); scenarios++;
    /* A full store must leave foreground fallback available and retain bytes. */
    nvn_discovery_close(&target); CHECK(nvn_discovery_init_empty(&target, target_path, game, shared_sha256_bytes, NVN_DISCOVERY_HEADER_BYTES, 1));
    r = (Resolver){0}; CHECK(shader_profile_merge(&p, &target, resolve, &r, NULL, &stats)); CHECK(stats.rejected == 3 && !target.count); scenarios++;
    nvn_discovery_close(&target); shader_profile_close(&p); nvn_discovery_close(&learned); free(bad); free(original);
    printf("{\"scenarios\":%u,\"assertions\":%u,\"metadata_only\":true,\"source\":\"synthetic\"}\n", scenarios, checks);
    return 0;
}
