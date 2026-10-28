/* Raitchi shader preparation profiles. Profiles contain only exact render-state
 * inputs and one-way shader identities; original shader bytes stay in the game.
 * The identity is the selected title/version identity used by the discovery
 * journal, never a renderer/driver cache identity. */
#pragma once
#include "shader_discovery.h"

#define SHADER_PROFILE_SCHEMA 1u
#define SHADER_PROFILE_HEADER_BYTES 120u
#define SHADER_PROFILE_METADATA_BYTES 481u
#define SHADER_PROFILE_RECORD_BYTES (SHADER_PROFILE_METADATA_BYTES + 32u)
#define SHADER_PROFILE_MAX_BYTES (SHADER_PROFILE_HEADER_BYTES + NVN_DISCOVERY_MAX_RECORDS * SHADER_PROFILE_RECORD_BYTES)

typedef struct {
    ShaderRec rec;
    ShaderKey vkey, fkey;
    uint32_t code_bytes[2], constant_bytes[2];
} ShaderProfileEntry;

typedef struct {
    uint8_t game_id[32];
    ShaderProfileEntry *entries;
    size_t count;
} ShaderProfile;

typedef enum {
    SHADER_PROFILE_ERROR = -1,
    SHADER_PROFILE_MISSING = 0,
    SHADER_PROFILE_LOADED = 1,
    SHADER_PROFILE_INCOMPATIBLE = 2,
    SHADER_PROFILE_CORRUPT = 3,
    SHADER_PROFILE_LIMIT = 4
} ShaderProfileResult;

typedef struct {
    size_t appended, reused, missing, rejected;
    bool cancelled;
} ShaderProfileMergeStats;

/* Resolver returns a malloc-owned exact code+constants span. stage is the
 * Maxwell stage enum (vertex=0, fragment=1, compute=5). Implementations must
 * respect the code/constants partition, not just the combined hash. The merger
 * independently checks the span's hash and recipe before admitting any bytes.
 * Any non-NULL returned allocation is freed by the merger, including failures. */
typedef bool (*ShaderProfileResolve)(void *context, unsigned stage, uint64_t hash,
                                    uint32_t code_bytes, uint32_t constant_bytes,
                                    uint8_t **out);

/* All functions are coordinator/writer-thread operations. The caller holds the
 * discovery-store lock and the source reader alive throughout merge/export. */
ShaderProfileResult shader_profile_load(const char *path, const uint8_t game_id[32],
                                        NvnDiscoverySha256 sha, ShaderProfile *out);
/* Bounded metadata parser for asset readers. Copies decoded entries; the input
 * buffer may be released immediately after this call. */
ShaderProfileResult shader_profile_load_bytes(const void *data, size_t size, const uint8_t game_id[32],
                                              NvnDiscoverySha256 sha, ShaderProfile *out);
void shader_profile_close(ShaderProfile *profile);
/* Count metadata recipes absent from the learned journal before opening/scanning
 * game sources. A completely warm profile needs no source indexing at all. */
bool shader_profile_missing_count(const ShaderProfile *profile, const NvnDiscoveryStore *store,
                                  size_t *missing);
bool shader_profile_merge(const ShaderProfile *profile, NvnDiscoveryStore *store,
                          ShaderProfileResolve resolve, void *context,
                          const volatile int *cancel, ShaderProfileMergeStats *stats);
/* Export the union of exact learned/imported recipes. This is an atomic
 * metadata-only file; no shader/code/constants/host pointers are serialized. */
bool shader_profile_export(const char *path, NvnDiscoveryStore *store);
/* Preserve existing same-version profile recipes even when current source
 * indexing could not resolve all of them. Exact metadata dedup, <=4096 recipes.
 * A corrupt/incompatible existing destination is preserved and returns false. */
bool shader_profile_export_union(const char *path, NvnDiscoveryStore *store);

/* Atomic union of existing metadata, imported metadata (including unresolved
 * recipes), and learned recipes. Rejects incompatible/corrupt destinations and
 * invalid input without modifying them. No original shader bytes are written. */
bool shader_profile_import_union(const char *path, NvnDiscoveryStore *store,
                                 const ShaderProfile *imported);

/* Public for import tooling and asset-free tests. No host struct layout is part
 * of the stable wire format. */
bool shader_profile_entry_valid(const ShaderProfileEntry *entry);
