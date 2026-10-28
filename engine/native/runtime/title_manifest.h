/* Versioned little-endian sidecar; independent expectations before dlopen. */
#ifndef SHARED_RECOMP_TITLE_MANIFEST_H
#define SHARED_RECOMP_TITLE_MANIFEST_H
#include "../module_abi.h"

#ifdef __cplusplus
extern "C" {
#endif
#define TITLE_MANIFEST_VERSION 1u
#define TITLE_MANIFEST_HEADER_SIZE 176u
#define TITLE_MANIFEST_MODULE_SIZE 112u
#define TITLE_MANIFEST_MAX_SIZE (TITLE_MANIFEST_HEADER_SIZE + 4u * TITLE_MANIFEST_MODULE_SIZE)
#define TITLE_MANIFEST_API __attribute__((visibility("default")))

typedef struct TitleManifest {
    uint32_t title_id;
    uint32_t capabilities;
    uint8_t contract_sha256[32];
    uint8_t code_sha256[32];
    uint8_t library_sha256[32];
    ModuleAbiProfile profile;
    ModuleAbiModuleSpec modules[MODULE_ABI_MAX_MODULES];
} TitleManifest;
enum TitleManifestStatus {
    TITLE_MANIFEST_OK, TITLE_MANIFEST_ARGUMENT, TITLE_MANIFEST_FORMAT,
    TITLE_MANIFEST_CONTRACT, TITLE_MANIFEST_PROFILE
};
/* Strict exact-size format, no native pointers or padding on disk. Unknown fields,
 * versions, target/API/SIMD/CPU contracts are rejected. Output cannot alias input.
 * Caller owns the output for the session; do not shallow-copy its profile pointer.
 * This parses declarations only. The caller must hash the library and all original
 * modules, check ELF/producer provenance, and apply its supported title policy. */
TITLE_MANIFEST_API enum TitleManifestStatus title_manifest_parse(const void *bytes,
    size_t length, const uint8_t expected_contract_sha256[32], TitleManifest *out,
    ModuleAbiFailure *failure);

#ifdef __cplusplus
}
#endif
#endif
