/* Optional, exact-game/version metadata profiles bundled in the APK. The asset
 * reader is supplied by the host (SDL/Android); this module has no JNI or GPU
 * dependency. Assets contain no game code, shader payloads, keys or saves. */
#pragma once
#include "shader_profile.h"

#define SHADER_PROFILE_CATALOG_ASSET_PATH_BYTES 85u
typedef struct {
    /* LOADED means an asset is open; MISSING means no such asset. Other results
     * mean failure. Report the full asset size, not a truncated prefix. */
    ShaderProfileResult (*open)(void *context, const char *asset, uint64_t *size);
    /* Exact bounded reads: false for short reads. Called only within full size. */
    bool (*read)(void *context, size_t offset, void *bytes, size_t size);
    void (*close)(void *context);
    void *context;
} ShaderProfileCatalogReader;

/* Path derives solely from the verified game/version identity; no user paths. */
bool shader_profile_catalog_asset_path(const uint8_t game_id[32],
                                      char out[SHADER_PROFILE_CATALOG_ASSET_PATH_BYTES]);
ShaderProfileResult shader_profile_catalog_load(const uint8_t game_id[32],
                                                NvnDiscoverySha256 sha,
                                                const ShaderProfileCatalogReader *reader,
                                                ShaderProfile *out);
