/* Only synthetic metadata and shader spans; no APK, GPU or game inputs. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include "shader_profile_catalog.h"
bool shared_sha256_bytes(const void *, size_t, uint8_t[32]);
static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); abort(); } } while (0)
typedef struct {
    uint8_t *bytes; uint64_t size; size_t actual;
    unsigned reads, closes; bool missing, fail;
    char path[SHADER_PROFILE_CATALOG_ASSET_PATH_BYTES];
} Asset;
static ShaderProfileResult open_asset(void *ctx, const char *path, uint64_t *size) {
    Asset *a = ctx; snprintf(a->path, sizeof a->path, "%s", path); *size = a->size;
    return a->missing ? SHADER_PROFILE_MISSING : SHADER_PROFILE_LOADED;
}
static bool read_asset(void *ctx, size_t at, void *out, size_t n) {
    Asset *a = ctx; a->reads++;
    CHECK(n <= 65536 && at <= a->size && n <= a->size - at);
    if (a->fail || at > a->actual || n > a->actual - at) return false;
    memcpy(out, a->bytes + at, n); return true;
}
static void close_asset(void *ctx) { ((Asset *)ctx)->closes++; }
static void read_file(const char *path, uint8_t **out, size_t *size) {
    struct stat st; CHECK(!stat(path, &st)); *size = st.st_size; *out = malloc(*size); CHECK(*out);
    FILE *f = fopen(path,"rb"); CHECK(f); CHECK(fread(*out,1,*size,f)==*size); CHECK(!fclose(f));
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    char path[1024], journal[1024], destination[1024];
    snprintf(path,sizeof path,"%s/catalog.bin",argv[1]);
    snprintf(journal,sizeof journal,"%s/journal.bin",argv[1]);
    snprintf(destination,sizeof destination,"%s/imported.bin",argv[1]);
    uint8_t game[32] = {1,2,3}, code[32] = {7,8,9};
    NvnDiscoveryStore store = {0};
    CHECK(nvn_discovery_init_empty(&store,journal,game,shared_sha256_bytes,8*1024*1024,4096));
    NvnDiscoveryRecipe r = {0}; r.rec.kind = 'C'; r.rec.vs = nvn_disc_codehash(code,sizeof code);
    r.rec.local[0] = r.rec.local[1] = r.rec.local[2] = 1;
    r.vkey.glsl = 1; r.code[0] = code; r.code_bytes[0] = sizeof code;
    CHECK(nvn_discovery_append(&store,&r)==NVN_DISCOVERY_APPEND_APPENDED);
    CHECK(shader_profile_export(path,&store));
    Asset a = {0}; read_file(path,&a.bytes,&a.actual); a.size = a.actual;
    ShaderProfileCatalogReader reader = {open_asset,read_asset,close_asset,&a};
    ShaderProfile p = {0};
    CHECK(shader_profile_catalog_load(game,shared_sha256_bytes,&reader,&p)==SHADER_PROFILE_LOADED);
    CHECK(p.count == 1 && a.reads == 1 && a.closes == 1);
    char expected[85]; CHECK(shader_profile_catalog_asset_path(game,expected));
    CHECK(!strcmp(expected,a.path) && strlen(expected)==84 && !strstr(expected,".."));
    uint8_t wrong[32] = {4}; shader_profile_close(&p);
    CHECK(shader_profile_catalog_load(wrong,shared_sha256_bytes,&reader,&p)==SHADER_PROFILE_INCOMPATIBLE);
    CHECK(!p.entries);
    a.missing = true; unsigned reads = a.reads, closes = a.closes;
    CHECK(shader_profile_catalog_load(game,shared_sha256_bytes,&reader,&p)==SHADER_PROFILE_MISSING);
    CHECK(a.reads == reads && a.closes == closes); a.missing = false;
    a.size = (uint64_t)SHADER_PROFILE_MAX_BYTES + 1;
    CHECK(shader_profile_catalog_load(game,shared_sha256_bytes,&reader,&p)==SHADER_PROFILE_LIMIT);
    CHECK(a.reads == reads && a.closes == closes + 1);
    a.size = UINT64_MAX;
    CHECK(shader_profile_catalog_load(game,shared_sha256_bytes,&reader,&p)==SHADER_PROFILE_LIMIT);
    CHECK(a.reads == reads);
    a.size = 119;
    CHECK(shader_profile_catalog_load(game,shared_sha256_bytes,&reader,&p)==SHADER_PROFILE_CORRUPT);
    CHECK(a.reads == reads);
    a.size = a.actual; a.fail = true;
    CHECK(shader_profile_catalog_load(game,shared_sha256_bytes,&reader,&p)==SHADER_PROFILE_ERROR);
    a.fail = false; a.bytes[120] ^= 1;
    CHECK(shader_profile_catalog_load(game,shared_sha256_bytes,&reader,&p)==SHADER_PROFILE_CORRUPT);
    a.bytes[120] ^= 1; a.size++;
    CHECK(shader_profile_catalog_load(game,shared_sha256_bytes,&reader,&p)==SHADER_PROFILE_ERROR);
    a.size = a.actual;
    CHECK(shader_profile_catalog_load(game,shared_sha256_bytes,&reader,&p)==SHADER_PROFILE_LOADED);
    /* Imported unresolved variants survive export with a different learned recipe. */
    p.entries[0].rec.local[0] = 2;
    CHECK(shader_profile_import_union(destination,&store,&p));
    ShaderProfile united = {0};
    CHECK(shader_profile_load(destination,game,shared_sha256_bytes,&united)==SHADER_PROFILE_LOADED);
    CHECK(united.count == 2); shader_profile_close(&united);
    struct stat before, after; CHECK(!stat(destination,&before));
    CHECK(shader_profile_import_union(destination,&store,&p)); CHECK(!stat(destination,&after));
    CHECK(before.st_ino == after.st_ino);
    uint8_t *saved = NULL; size_t saved_n; read_file(destination,&saved,&saved_n);
    p.entries[0].code_bytes[0] = 0; CHECK(!shader_profile_import_union(destination,&store,&p));
    uint8_t *unchanged = NULL; size_t unchanged_n; read_file(destination,&unchanged,&unchanged_n);
    CHECK(saved_n == unchanged_n && !memcmp(saved,unchanged,saved_n)); free(unchanged);
    p.entries[0].code_bytes[0] = 32; p.game_id[0] ^= 1;
    CHECK(!shader_profile_import_union(destination,&store,&p));
    p.game_id[0] ^= 1;
    /* Existing corrupt destinations are never repaired destructively. */
    FILE *f = fopen(destination,"wb"); CHECK(f); CHECK(fwrite("keep",1,4,f)==4); CHECK(!fclose(f));
    CHECK(!shader_profile_import_union(destination,&store,&p));
    read_file(destination,&unchanged,&unchanged_n); CHECK(unchanged_n == 4 && !memcmp(unchanged,"keep",4));
    free(unchanged); free(saved); shader_profile_close(&p);
    /* More than one read, including a bounded final chunk, exercises APK bounds. */
    for (unsigned i = 2; i <= 150; i++) { r.rec.local[0] = i;
        CHECK(nvn_discovery_append(&store,&r)==NVN_DISCOVERY_APPEND_APPENDED); }
    CHECK(shader_profile_export(path,&store)); free(a.bytes); read_file(path,&a.bytes,&a.actual); a.size = a.actual;
    reads = a.reads; CHECK(shader_profile_catalog_load(game,shared_sha256_bytes,&reader,&p)==SHADER_PROFILE_LOADED);
    CHECK(a.reads == reads + 2 && p.count == 150); shader_profile_close(&p);
    free(a.bytes); nvn_discovery_close(&store);
    printf("{\"checks\":%u,\"metadata_only\":true,\"bounded_asset_reader\":true,\"atomic_unresolved_union\":true}\n",checks);
    return 0;
}
