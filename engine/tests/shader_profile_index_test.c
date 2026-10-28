/* Asset-free first-launch profile -> source index -> discovery startup bridge.
 * The package opener is a stub returning a real reader over a synthetic RomFS;
 * package keys, JNI, real NSP/NCA selection and Vulkan are outside this gate. */
#define _GNU_SOURCE
#include "../ports/shared-android/runtime/nxpkg.c"
#include "../ports/shared-android/runtime/nvn/shader_source_index.c"
#include "../ports/shared-android/runtime/nvn/shader_preparation.h"

bool shared_sha256_bytes(const void *, size_t, uint8_t[32]);
static unsigned checks, scenarios, package_opens;
static const char *package_fixture;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); abort(); } } while (0)

NxRomfs *shared_package_open_romfs(char *why, size_t why_size) {
    package_opens++;
    if (!package_fixture) { if (why && why_size) snprintf(why, why_size, "synthetic package unavailable"); return NULL; }
    NxRomfs *r = calloc(1, sizeof *r); CHECK(r); r->fd = open(package_fixture, O_RDONLY); CHECK(r->fd >= 0);
    struct stat st; CHECK(!fstat(r->fd, &st)); r->size = (uint64_t)st.st_size;
    r->sec.size = r->sec.data_size = r->size; r->sec.enc_type = 1;
    CHECK(!pthread_mutex_init(&r->lock, NULL)); return r;
}
static void p32(uint8_t *p, uint32_t value) { for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(value >> (8 * i)); }
static void p64(uint8_t *p, uint64_t value) { p32(p, (uint32_t)value); p32(p + 4, (uint32_t)(value >> 32)); }
static uint64_t bnsh(uint8_t *p, unsigned stage, uint32_t code, uint32_t constants, unsigned seed) {
    memcpy(p, "BNSH\0\0\0\0", 8); p[0xc] = 0xff; p[0xd] = 0xfe; p[0x16] = 0x40; p32(p + 0x1c, 0x1000);
    memcpy(p + 0x40, "grsc", 4); p32(p + 0x5c, 1); p64(p + 0x60, 0x80); p64(p + 0x90, 0x100);
    p64(p + 0x108 + stage * 8, 0x140); p64(p + 0x148, 0x180); p64(p + 0x150, 0x900); p32(p + 0x158, code + constants);
    p32(p + 0x180 + 0x6fc, constants); p32(p + 0x180 + 0x700, code);
    for (uint32_t i = 0; i < code + constants; i++) p[0x900 + i] = (uint8_t)(seed + i * 7 + i / 3);
    return nvn_disc_codehash(p + 0x900, code + constants);
}
static void save(const char *path, const void *bytes, size_t n) {
    FILE *f = fopen(path, "wb"); CHECK(f); CHECK(fwrite(bytes, 1, n, f) == n); CHECK(!fclose(f));
}
static void empty(NvnDiscoveryStore *s, const char *path, const uint8_t game[32]) {
    CHECK(nvn_discovery_init_empty(s, path, game, shared_sha256_bytes, 8 * 1024 * 1024, 4096));
}
typedef struct { const uint8_t *data; size_t size; } MemorySource;
static bool memory_read(void *context, uint64_t offset, void *out, size_t size) {
    MemorySource *source = context;
    CHECK(offset <= source->size && size <= source->size - offset);
    memcpy(out, source->data + offset, size); return true;
}
static void compare(const NvnDiscoveryRecipe *a, const NvnDiscoveryRecipe *b) {
    uint8_t *aw = NULL, *bw = NULL, ah[32], bh[32]; size_t an = 0, bn = 0;
    CHECK(nvn_disc_payload_make(a, shared_sha256_bytes, &aw, &an, ah));
    CHECK(nvn_disc_payload_make(b, shared_sha256_bytes, &bw, &bn, bh));
    CHECK(an == bn && !memcmp(aw, bw, an)); free(aw); free(bw);
}
typedef struct { uint8_t *bytes; size_t size; bool absent; unsigned opens, reads, closes; uint8_t game[32]; } Catalog;
static ShaderProfileResult catalog_open(void *context, const char *asset, uint64_t *size) {
    Catalog *c = context; c->opens++;
    char expected[SHADER_PROFILE_CATALOG_ASSET_PATH_BYTES];
    CHECK(shader_profile_catalog_asset_path(c->game, expected)); CHECK(!strcmp(asset, expected));
    *size = c->size; return c->absent ? SHADER_PROFILE_MISSING : SHADER_PROFILE_LOADED;
}
static bool catalog_read(void *context, size_t offset, void *out, size_t size) {
    Catalog *c = context; c->reads++; CHECK(offset <= c->size && size <= c->size - offset);
    memcpy(out, c->bytes + offset, size); return true;
}
static void catalog_close(void *context) { ((Catalog *)context)->closes++; }

int main(int argc, char **argv) {
    CHECK(argc == 2); CHECK(!chdir(argv[1])); CHECK(!mkdir("cache", 0700)); CHECK(!mkdir("romfs", 0700)); CHECK(!mkdir("profiles", 0700));
    CHECK(!unsetenv("LARECOMP_PACKAGE_URI")); CHECK(!unsetenv("LARECOMP_PACKAGE_PATH")); CHECK(!unsetenv("LARECOMP_SHADER_PROFILE"));
    uint8_t *archive = calloc(1, 0x20000); CHECK(archive);
    const size_t vo = 0xfff8, fo = 0x12000, co = 0x14000;
    uint64_t vh = bnsh(archive + vo, 0, 128, 16, 0x21), fh = bnsh(archive + fo, 4, 128, 8, 0x49), ch = bnsh(archive + co, 5, 32, 8, 0xc5);
    save("romfs/test.bfsha", archive, 0x20000);
    uint8_t game[32]; for (unsigned i = 0; i < 32; i++) game[i] = (uint8_t)(i * 3 + 0x21);
    NvnDiscoveryRecipe g = {.rec = {.kind = 'G', .vs = vh, .fs = fh, .samples = 1, .ncolor = 1, .channel_mask = 15, .topo = 2, .poly_fill = 1},
        .vkey = {.glsl = 1, .tex_uint = 0x12345678}, .fkey = {.glsl = 1, .tex_uint = 0x80000001},
        .code = {archive + vo + 0x900, archive + fo + 0x900}, .code_bytes = {128, 128}, .constant_bytes = {16, 8}};
    g.rec.color[0] = 0x14;
    NvnDiscoveryRecipe c = {.rec = {.kind = 'C', .vs = ch, .local = {16, 2, 1}}, .vkey = {.glsl = 1},
        .code = {archive + co + 0x900, NULL}, .code_bytes = {32, 0}, .constant_bytes = {8, 0}};
    NvnDiscoveryRecipe depth = g; depth.rec.fs = 0; depth.rec.ncolor = 0; depth.rec.color[0] = 0; depth.rec.depth = 0x8a;
    depth.code[1] = NULL; depth.code_bytes[1] = depth.constant_bytes[1] = 0;
    NvnDiscoveryRecipe recipes[3] = {g, c, depth}; NvnDiscoveryStore store = {0};
    empty(&store, "cache/initial.bin", game);
    for (unsigned i = 0; i < 3; i++) CHECK(nvn_discovery_append(&store, &recipes[i]) == NVN_DISCOVERY_APPEND_APPENDED);
    CHECK(shader_preparation_export(&store, "profiles")); CHECK(!unlink("cache/shader_profile.bin")); nvn_discovery_close(&store); scenarios++;
    empty(&store, "cache/cold.bin", game); ShaderPreparationStats stats;
    CHECK(shader_preparation_seed(&store, "romfs", "profiles", NULL, &stats));
    CHECK(stats.profiles == 1 && stats.appended == 3 && !stats.rejected && !stats.missing);
    CHECK(stats.source_attempted && stats.source_complete && stats.source_stages == 3 && !package_opens);
    for (unsigned i = 0; i < 3; i++) { NvnDiscoveryRecipe got = {0}; CHECK(nvn_discovery_read_at(&store, i, &got)); compare(&recipes[i], &got); nvn_discovery_recipe_release(&got); }
    CHECK(nvn_discovery_commit(&store)); CHECK(shader_preparation_export(&store, "profiles")); scenarios++;
    /* Warm recipes skip source inventory even with a package selected but
     * unavailable, demonstrating no hidden reads behind the fast cache path. */
    CHECK(!rename("romfs/test.bfsha", "archive.hidden")); CHECK(!setenv("LARECOMP_PACKAGE_PATH", "synthetic-selected.nsp", 1));
    CHECK(shader_preparation_seed(&store, "romfs", "profiles", NULL, &stats));
    CHECK(stats.profiles == 2 && stats.reused == 6 && !stats.source_attempted && !package_opens && !stats.appended); scenarios++;
    nvn_discovery_close(&store); empty(&store, "cache/package-fail.bin", game);
    CHECK(shader_preparation_seed(&store, "romfs", "profiles", NULL, &stats));
    CHECK(stats.source_attempted && !stats.source_complete && !stats.appended && stats.missing == 6 && package_opens == 1 && !store.count); scenarios++;
    /* A real NxRomfs reader over a synthetic raw image exercises the package
     * branch's actual logical pread + tree enumeration + exact stage resolver. */
    uint8_t *image = calloc(1, 0x20200); CHECK(image);
    p64(image, 0x50); p64(image + 8, 0x50); p64(image + 16, 4); p64(image + 24, 0x54); p64(image + 32, 24);
    p64(image + 40, 0x6c); p64(image + 48, 4); p64(image + 56, 0x70); p64(image + 64, 44); p64(image + 72, 0x200);
    p32(image + 0x54, 0); p32(image + 0x58, UINT32_MAX); p32(image + 0x5c, UINT32_MAX); p32(image + 0x60, 0); p32(image + 0x64, UINT32_MAX);
    p32(image + 0x70, 0); p32(image + 0x74, UINT32_MAX); p64(image + 0x80, 0x20000); p32(image + 0x88, UINT32_MAX); p32(image + 0x8c, 10);
    memcpy(image + 0x90, "test.bfsha", 10); memcpy(image + 0x200, archive, 0x20000); save("synthetic-romfs.img", image, 0x20200);
    package_fixture = "synthetic-romfs.img";
    CHECK(shader_preparation_seed(&store, "romfs", "profiles", NULL, &stats));
    CHECK(stats.source_attempted && stats.source_complete && stats.source_stages == 3 && stats.appended == 3 && stats.reused == 3 && package_opens == 2); scenarios++;
    nvn_discovery_close(&store); empty(&store, "cache/cancel.bin", game); volatile int cancel = 1;
    CHECK(!shader_preparation_seed(&store, "romfs", "profiles", &cancel, &stats));
    CHECK(stats.cancelled && !stats.source_attempted && !store.count && package_opens == 2); scenarios++;
    CHECK(!unsetenv("LARECOMP_PACKAGE_PATH"));
    uint8_t other[32]; memcpy(other, game, 32); other[0] ^= 1; nvn_discovery_close(&store); empty(&store, "cache/other.bin", other);
    CHECK(!shader_preparation_seed(&store, "romfs", "profiles", NULL, &stats));
    CHECK(stats.incompatible == 1 && !stats.source_attempted && !store.count); scenarios++;
    CHECK(!unlink("cache/shader_profile.bin"));
    CHECK(shader_preparation_seed(&store, "nonexistent", "profiles", NULL, &stats));
    CHECK(!stats.profiles && !stats.source_attempted && !store.count); scenarios++;
    /* A corrupted source span fails rehashing after a successful inventory;
     * wrong partition likewise must not be admitted from the same total hash. */
    ShaderSourceIndex *idx = shader_source_index_create(); CHECK(idx); char why[256];
    MemorySource memory = {.data = archive, .size = 0x20000};
    CHECK(shader_source_index_scan(idx, memory_read, &memory, memory.size, 0, memory.size, NULL, why, sizeof why));
    CHECK(shader_source_index_count(idx) == 3);
    uint8_t *exact = shader_source_index_read_exact(idx, vh, 0, 128, 16, NULL);
    CHECK(exact && !memcmp(exact, archive + vo + 0x900, 144)); free(exact);
    CHECK(!shader_source_index_read_exact(idx, vh, 4, 128, 16, NULL));
    CHECK(!shader_source_index_read_exact(idx, vh, 0, 136, 8, NULL));
    archive[vo + 0x900] ^= 1;
    CHECK(!shader_source_index_read_exact(idx, vh, 0, 128, 16, NULL));
    archive[vo + 0x900] ^= 1;
    shader_source_index_destroy(idx); scenarios++;

    /* Runtime g_romfs_dir is absolute. With an empty RomFS and all stages in
     * the verified original ExeFS/main data, the bridge must still seed the
     * matching global profile, then avoid opening sources on the next pass. */
    CHECK(!mkdir("game", 0700)); CHECK(!mkdir("game/romfs", 0700)); CHECK(!mkdir("game/exefs", 0700));
    uint8_t *nso = calloc(1, 0x20100); CHECK(nso); memcpy(nso, "NSO0", 4);
    p32(nso + 0x20, 0x100); p32(nso + 0x28, 0x20000);
    memcpy(nso + 0x100, archive, 0x20000); save("game/exefs/main", nso, 0x20100); free(nso);
    char cwd[4096], absolute_romfs[4200]; CHECK(getcwd(cwd, sizeof cwd));
    CHECK(snprintf(absolute_romfs, sizeof absolute_romfs, "%s/game/romfs", cwd) < (int)sizeof absolute_romfs);
    nvn_discovery_close(&store); empty(&store, "cache/nso-only.bin", game);
    CHECK(shader_preparation_seed(&store, absolute_romfs, "profiles", NULL, &stats));
    CHECK(stats.profiles == 1 && stats.appended == 3 && !stats.missing && !stats.rejected);
    CHECK(stats.source_attempted && stats.source_complete && stats.source_stages == 3 && package_opens == 2);
    for (unsigned i = 0; i < 3; i++) { NvnDiscoveryRecipe got = {0}; CHECK(nvn_discovery_read_at(&store, i, &got)); compare(&recipes[i], &got); nvn_discovery_recipe_release(&got); }
    CHECK(shader_preparation_export(&store, "profiles")); scenarios++;
    CHECK(!rename("game/exefs/main", "game/exefs/main.hidden"));
    CHECK(shader_preparation_seed(&store, absolute_romfs, "profiles", NULL, &stats));
    CHECK(stats.profiles == 2 && stats.reused == 6 && !stats.source_attempted && !stats.appended && package_opens == 2); scenarios++;

    /* A local profile from another version is a warning/cache miss. It is
     * preserved, while a correct global profile still imports and later gains
     * newly learned metadata even though local export remains incompatible. */
    CHECK(!rename("game/exefs/main.hidden", "game/exefs/main"));
    NvnDiscoveryStore old_version = {0}; empty(&old_version, "cache/old-version.bin", other);
    CHECK(nvn_discovery_append(&old_version, &g) == NVN_DISCOVERY_APPEND_APPENDED);
    CHECK(shader_profile_export("cache/shader_profile.bin", &old_version)); nvn_discovery_close(&old_version);
    struct stat local_before, local_after; CHECK(!stat("cache/shader_profile.bin", &local_before));
    nvn_discovery_close(&store); empty(&store, "cache/current-version.bin", game);
    CHECK(!shader_preparation_seed(&store, absolute_romfs, "profiles", NULL, &stats));
    CHECK(stats.incompatible == 1 && stats.profiles == 1 && stats.appended == 3 && store.count == 3);
    CHECK(stats.source_attempted && stats.source_complete && !stats.missing); scenarios++;
    NvnDiscoveryRecipe new_variant = g; new_variant.rec.channel_mask = 7;
    CHECK(nvn_discovery_append(&store, &new_variant) == NVN_DISCOVERY_APPEND_APPENDED);
    CHECK(!shader_preparation_export(&store, "profiles"));
    CHECK(!stat("cache/shader_profile.bin", &local_after) && local_before.st_ino == local_after.st_ino);
    ShaderProfile local_profile = {0};
    CHECK(shader_profile_load("cache/shader_profile.bin", other, shared_sha256_bytes, &local_profile) == SHADER_PROFILE_LOADED && local_profile.count == 1);
    shader_profile_close(&local_profile);
    char global_path[128] = "profiles/";
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; i++) { global_path[9 + i * 2] = hex[game[i] >> 4]; global_path[10 + i * 2] = hex[game[i] & 15]; }
    memcpy(global_path + 73, ".bin", 5);
    CHECK(shader_profile_load(global_path, game, shared_sha256_bytes, &local_profile) == SHADER_PROFILE_LOADED && local_profile.count == 4);
    shader_profile_close(&local_profile); scenarios++;
    nvn_discovery_close(&store);

    /* A completely cold installation has no journal/profile metadata yet. Its
     * matching bundled metadata catalog must resolve directly from game inputs,
     * and its warm pass must avoid source inventory even without source files. */
    CHECK(!mkdir("catalog-case",0700)); CHECK(!chdir("catalog-case"));
    CHECK(!mkdir("cache",0700)); CHECK(!mkdir("profiles",0700)); CHECK(!mkdir("romfs",0700));
    save("romfs/test.bfsha", archive, 0x20000);
    empty(&store,"cache/export-source.bin",game);
    for (unsigned i = 0; i < 3; i++) CHECK(nvn_discovery_append(&store,&recipes[i]) == NVN_DISCOVERY_APPEND_APPENDED);
    CHECK(shader_profile_export("asset.bin",&store)); nvn_discovery_close(&store);
    Catalog catalog = {0}; memcpy(catalog.game,game,32);
    struct stat asset_stat; CHECK(!stat("asset.bin",&asset_stat)); catalog.size = asset_stat.st_size;
    catalog.bytes = malloc(catalog.size); CHECK(catalog.bytes);
    FILE *asset_file = fopen("asset.bin","rb"); CHECK(asset_file);
    CHECK(fread(catalog.bytes,1,catalog.size,asset_file) == catalog.size); CHECK(!fclose(asset_file));
    ShaderProfileCatalogReader reader = {catalog_open,catalog_read,catalog_close,&catalog};
    empty(&store,"cache/catalog-cold.bin",game);
    CHECK(shader_preparation_seed_catalog(&store,"romfs","profiles",NULL,&reader,&stats));
    CHECK(stats.profiles == 1 && stats.catalog_profiles == 1 && stats.catalog_recipes == 3);
    CHECK(stats.appended == 3 && store.count == 3 && !stats.missing && stats.catalog_persisted);
    CHECK(stats.source_attempted && stats.source_complete && stats.source_stages == 3);
    CHECK(catalog.opens == 1 && catalog.reads == 1 && catalog.closes == 1); scenarios++;
    CHECK(!rename("romfs/test.bfsha","archive.hidden"));
    CHECK(shader_preparation_seed_catalog(&store,"romfs","profiles",NULL,&reader,&stats));
    CHECK(stats.profiles == 3 && stats.reused == 9 && !stats.source_attempted && !stats.appended);
    CHECK(stats.catalog_persisted && stats.catalog_profiles == 1); scenarios++;
    catalog.absent = true;
    CHECK(shader_preparation_seed_catalog(&store,"romfs","profiles",NULL,&reader,&stats));
    CHECK(stats.profiles == 2 && !stats.catalog_profiles && !stats.incompatible && !stats.source_attempted);
    catalog.absent = false; catalog.bytes[120] ^= 1;
    CHECK(!shader_preparation_seed_catalog(&store,"romfs","profiles",NULL,&reader,&stats));
    CHECK(stats.incompatible == 1 && !stats.catalog_profiles && !stats.source_attempted);
    catalog.bytes[120] ^= 1; catalog.bytes[24] ^= 1;
    CHECK(!shader_preparation_seed_catalog(&store,"romfs","profiles",NULL,&reader,&stats));
    CHECK(stats.incompatible == 1 && !stats.catalog_profiles && !stats.source_attempted);
    catalog.bytes[24] ^= 1; scenarios++;

    /* No source currently exists: unresolved metadata survives both catalog
     * import and a later learned-profile export. It resolves after sources
     * return, even when the bundled catalog itself is unavailable then. */
    CHECK(!unlink("cache/shader_profile.bin")); CHECK(!unlink(global_path));
    nvn_discovery_close(&store); empty(&store,"cache/catalog-unresolved.bin",game);
    CHECK(shader_preparation_seed_catalog(&store,"romfs","profiles",NULL,&reader,&stats));
    CHECK(stats.catalog_profiles == 1 && stats.missing == 3 && !store.count && stats.catalog_persisted);
    CHECK(shader_profile_load(global_path,game,shared_sha256_bytes,&local_profile) == SHADER_PROFILE_LOADED);
    CHECK(local_profile.count == 3); shader_profile_close(&local_profile);
    NvnDiscoveryRecipe learned_compute = c; learned_compute.rec.local[0] = 32;
    CHECK(nvn_discovery_append(&store,&learned_compute) == NVN_DISCOVERY_APPEND_APPENDED);
    CHECK(shader_preparation_export(&store,"profiles"));
    CHECK(shader_profile_load(global_path,game,shared_sha256_bytes,&local_profile) == SHADER_PROFILE_LOADED);
    CHECK(local_profile.count == 4); shader_profile_close(&local_profile); scenarios++;
    CHECK(!rename("archive.hidden","romfs/test.bfsha")); catalog.absent = true;
    CHECK(shader_preparation_seed_catalog(&store,"romfs","profiles",NULL,&reader,&stats));
    CHECK(stats.profiles == 2 && !stats.catalog_profiles && stats.appended == 3 && store.count == 4);
    CHECK(stats.source_attempted && stats.source_complete && !stats.missing && !stats.rejected);
    CHECK(shader_preparation_export(&store,"profiles"));
    CHECK(shader_profile_load(global_path,game,shared_sha256_bytes,&local_profile) == SHADER_PROFILE_LOADED);
    CHECK(local_profile.count == 4); shader_profile_close(&local_profile); scenarios++;
    free(catalog.bytes); nvn_discovery_close(&store); free(image); free(archive);
    printf("{\"scenarios\":%u,\"assertions\":%u,\"synthetic_only\":true,\"actual_startup_bridge\":true}\n", scenarios, checks);
    return 0;
}
