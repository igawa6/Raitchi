#include "shader_profile_catalog.h"

bool shader_profile_catalog_asset_path(const uint8_t game[32],
                                      char out[SHADER_PROFILE_CATALOG_ASSET_PATH_BYTES]) {
    static const char hex[] = "0123456789abcdef";
    static const char prefix[] = "shader-profiles/";
    if (!game || !out) return false;
    memcpy(out, prefix, sizeof prefix - 1);
    size_t at = sizeof prefix - 1;
    for (unsigned i = 0; i < 32; i++) {
        out[at++] = hex[game[i] >> 4]; out[at++] = hex[game[i] & 15];
    }
    memcpy(out + at, ".bin", 5);
    return true;
}

ShaderProfileResult shader_profile_catalog_load(const uint8_t game[32],
                                                NvnDiscoverySha256 sha,
                                                const ShaderProfileCatalogReader *reader,
                                                ShaderProfile *out) {
    if (!out) return SHADER_PROFILE_ERROR;
    memset(out, 0, sizeof *out);
    if (!game || !sha || !reader || !reader->open || !reader->read || !reader->close)
        return SHADER_PROFILE_ERROR;
    char asset[SHADER_PROFILE_CATALOG_ASSET_PATH_BYTES];
    shader_profile_catalog_asset_path(game, asset);
    uint64_t size = 0;
    ShaderProfileResult result = reader->open(reader->context, asset, &size);
    if (result != SHADER_PROFILE_LOADED) return result;
    uint8_t *bytes = NULL;
    if (size > SHADER_PROFILE_MAX_BYTES) result = SHADER_PROFILE_LIMIT;
    else if (size < SHADER_PROFILE_HEADER_BYTES) result = SHADER_PROFILE_CORRUPT;
    else {
        bytes = malloc((size_t)size);
        if (!bytes) result = SHADER_PROFILE_ERROR;
        else {
            /* Keep host reads bounded independently of the largest profile. */
            size_t at = 0;
            while (at < size) {
                size_t n = size - at > 65536 ? 65536 : (size_t)size - at;
                if (!reader->read(reader->context, at, bytes + at, n)) break;
                at += n;
            }
            result = at == size ? shader_profile_load_bytes(bytes, (size_t)size, game, sha, out)
                                : SHADER_PROFILE_ERROR;
        }
    }
    free(bytes);
    reader->close(reader->context);
    return result;
}
