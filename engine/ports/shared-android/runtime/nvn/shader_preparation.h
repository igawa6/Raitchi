/* Coordinator-side profile/source integration; no package extraction or draw work. */
#pragma once
#include "shader_profile.h"
#include "shader_profile_catalog.h"

typedef struct {
    size_t profiles, incompatible, appended, reused, missing, rejected;
    size_t catalog_profiles, catalog_recipes;
    bool catalog_persisted;
    size_t source_stages;
    bool source_attempted, source_complete, cancelled;
} ShaderPreparationStats;

/* Caller owns the discovery-store mutex. The selected version is store.game_id.
 * Missing profiles are normal. Source indexing is lazy: warm profiles require
 * no package reads. Partial inventory never changes hash/partition validation. */
bool shader_preparation_seed(NvnDiscoveryStore *store, const char *romfs_directory,
                             const char *profile_directory, const volatile int *cancel,
                             ShaderPreparationStats *stats);
bool shader_preparation_seed_catalog(NvnDiscoveryStore *store, const char *romfs_directory,
                                     const char *profile_directory, const volatile int *cancel,
                                     const ShaderProfileCatalogReader *catalog,
                                     ShaderPreparationStats *stats);
/* Save the learned union both beside the journal and outside derived caches.
 * Original shader bytes are excluded. Unchanged metadata files are not rewritten. */
bool shader_preparation_export(NvnDiscoveryStore *store, const char *profile_directory);
