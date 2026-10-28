#include "shader_preparation.h"
#include "shader_source_index.h"
#include "../native_package_android.h"
#include <sys/stat.h>

typedef struct {
    const char *romfs_directory;
    const volatile int *cancel;
    ShaderSourceIndex *index;
    NxRomfs *romfs;
    ShaderPreparationStats *stats;
} PreparationSources;

static bool profile_path(char out[4096], const char *directory, const uint8_t game[32]) {
    if (!directory || !*directory) return false;
    char name[65];
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        name[2 * i] = hex[game[i] >> 4]; name[2 * i + 1] = hex[game[i] & 15];
    }
    name[64] = 0;
    int n = snprintf(out, 4096, "%s/%s.bin", directory, name);
    return n > 0 && n < 4096;
}

static bool preparation_resolve(void *context, unsigned stage, uint64_t hash,
                                uint32_t code_bytes, uint32_t constant_bytes, uint8_t **out) {
    PreparationSources *sources = context;
    *out = NULL;
    if (sources->cancel && *sources->cancel) return false;
    if (!sources->stats->source_attempted) {
        sources->stats->source_attempted = true;
        sources->index = shader_source_index_create();
        if (!sources->index) return false;
        const char *package = getenv("LARECOMP_PACKAGE_URI");
        if (!package || !*package) package = getenv("LARECOMP_PACKAGE_PATH");
        char why[256];
        if (package && *package) {
            sources->romfs = shared_package_open_romfs(why, sizeof why);
            if (sources->romfs)
                sources->stats->source_complete = shader_source_index_romfs(sources->index,
                    sources->romfs, sources->cancel, why, sizeof why);
        } else if (sources->romfs_directory && *sources->romfs_directory) {
            sources->stats->source_complete = shader_source_index_directory(sources->index,
                sources->romfs_directory, sources->cancel, why, sizeof why);
        }
        // NSP preparation already preserves and verifies the small original
        // ExeFS modules. Index main's data without extracting the RomFS tree.
        if (sources->romfs_directory) {
            const char *end = strrchr(sources->romfs_directory, '/');
            char main_path[4096];
            size_t prefix = end ? (size_t)(end - sources->romfs_directory) : 0;
            if (prefix && prefix < sizeof main_path - sizeof "/exefs/main") {
                memcpy(main_path, sources->romfs_directory, prefix);
                memcpy(main_path + prefix, "/exefs/main", sizeof "/exefs/main");
                struct stat st;
                if (!stat(main_path, &st) && S_ISREG(st.st_mode))
                    sources->stats->source_complete &= shader_source_index_nso(sources->index,
                        main_path, sources->cancel, why, sizeof why);
            }
        }
        sources->stats->source_stages = shader_source_index_count(sources->index);
    }
    if (!sources->index) return false;
    /* ShaderKey uses Maxwell's compact FS=1; BNSH enumerates all six stages. */
    uint8_t bnsh_stage = stage == 1 ? 4 : (uint8_t)stage;
    *out = shader_source_index_read_exact(sources->index, hash, bnsh_stage,
                                          code_bytes, constant_bytes, NULL);
    return *out != NULL;
}

static bool preparation_merge_profile(const ShaderProfile *profile, NvnDiscoveryStore *store,
                                      PreparationSources *sources) {
    sources->stats->profiles++;
    size_t missing = 0;
    bool ok = shader_profile_missing_count(profile, store, &missing);
    if (ok && !missing) sources->stats->reused += profile->count;
    if (ok && missing) {
        ShaderProfileMergeStats merged = {0};
        ok = shader_profile_merge(profile, store, preparation_resolve, sources,
                                  sources->cancel, &merged);
        sources->stats->appended += merged.appended;
        sources->stats->reused += merged.reused;
        sources->stats->missing += merged.missing;
        sources->stats->rejected += merged.rejected;
        sources->stats->cancelled |= merged.cancelled;
    }
    return ok;
}

static bool preparation_merge_path(const char *path, NvnDiscoveryStore *store,
                                   PreparationSources *sources) {
    ShaderProfile profile = {0};
    ShaderProfileResult result = shader_profile_load(path, store->game_id, store->sha256, &profile);
    if (result == SHADER_PROFILE_MISSING) return true;
    if (result != SHADER_PROFILE_LOADED) {
        sources->stats->incompatible++;
        return false;
    }
    bool ok = preparation_merge_profile(&profile, store, sources);
    shader_profile_close(&profile);
    return ok;
}

bool shader_preparation_seed(NvnDiscoveryStore *store, const char *romfs_directory,
                             const char *profile_directory, const volatile int *cancel,
                             ShaderPreparationStats *stats) {
    return shader_preparation_seed_catalog(store, romfs_directory, profile_directory, cancel, NULL, stats);
}

bool shader_preparation_seed_catalog(NvnDiscoveryStore *store, const char *romfs_directory,
                                     const char *profile_directory, const volatile int *cancel,
                                     const ShaderProfileCatalogReader *catalog,
                                     ShaderPreparationStats *stats) {
    if (!store || !store->initialized || !stats) return false;
    memset(stats, 0, sizeof *stats);
    PreparationSources sources = {.romfs_directory = romfs_directory, .cancel = cancel, .stats = stats};
    bool ok = preparation_merge_path("cache/shader_profile.bin", store, &sources);
    char path[4096];
    if (profile_path(path, profile_directory, store->game_id))
        ok = preparation_merge_path(path, store, &sources) && ok;
    const char *explicit_profile = getenv("LARECOMP_SHADER_PROFILE");
    if (explicit_profile && *explicit_profile)
        ok = preparation_merge_path(explicit_profile, store, &sources) && ok;
    if (catalog && !(cancel && *cancel)) {
        ShaderProfile profile = {0};
        ShaderProfileResult result = shader_profile_catalog_load(store->game_id, store->sha256, catalog, &profile);
        if (result == SHADER_PROFILE_LOADED) {
            stats->catalog_profiles = 1;
            stats->catalog_recipes = profile.count;
            ok = preparation_merge_profile(&profile, store, &sources) && ok;
            // Persist unresolved metadata too: a partial source inventory must
            // not discard recipes during the next learned-journal export.
            bool saved = shader_profile_import_union("cache/shader_profile.bin", store, &profile);
            if (profile_path(path, profile_directory, store->game_id)) {
                bool directory_ok = !mkdir(profile_directory, 0700) || errno == EEXIST;
                saved = (directory_ok && shader_profile_import_union(path, store, &profile)) && saved;
            }
            stats->catalog_persisted = saved;
            shader_profile_close(&profile);
        } else if (result != SHADER_PROFILE_MISSING) {
            stats->incompatible++;
            ok = false;
        }
    }
    stats->cancelled |= cancel && *cancel;
    shader_source_index_destroy(sources.index);
    nx_romfs_close(sources.romfs);
    return ok && !stats->cancelled;
}

bool shader_preparation_export(NvnDiscoveryStore *store, const char *profile_directory) {
    if (!store || !store->initialized || !store->count) return false;
    bool ok = shader_profile_export_union("cache/shader_profile.bin", store);
    char path[4096];
    if (profile_path(path, profile_directory, store->game_id)) {
        if (mkdir(profile_directory, 0700) && errno != EEXIST) return false;
        ok = shader_profile_export_union(path, store) && ok;
    }
    return ok;
}
