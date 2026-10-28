/* Native-only metadata preparation shared by Android and host validation. */
#ifndef SHARED_RECOMP_NATIVE_PREPARE_H
#define SHARED_RECOMP_NATIVE_PREPARE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef bool (*NativePrepareHash)(const void *bytes,size_t length,uint8_t out[32]);
typedef struct NativePrepareResult {
    const char *title; /* immutable actual TitleAdapter key */
    uint32_t modules;
    uint64_t program_id; /* adapter program ID; for "generic", read from main.npdm */
    size_t manifest_bytes;
    bool created; /* false only when an identical existing manifest was accepted */
    uint8_t manifest_sha256[32];
} NativePrepareResult;
/* Pure title lookup (no title selection, reservation, loader or renderer).
 * "generic" has no fixed program ID and returns 0. */
uint64_t native_prepare_program_id(const char *title);
/* Input is an app-owned immutable ExeFS directory. Uses real adapter/build,
 * CPU3/native-manifest/layout and original NSO/LZ4 validation. Hash callback is
 * the runtime's real SHA256 implementation; this function does not invent hashes.
 * Writes only manifest_path atomically after ALL originals validate, accepting
 * an already identical file but never replacing conflicting output or symlinks.
 * Mapping policy is16384, as tools/prepare_native_game.py. No guest execution,
 * generated C, game DSO, RomFS extraction, package/key serialization or GPU setup.
 * Title "generic" derives its adapter from the same ExeFS directory
 * (native/adapters/generic) instead of a static table; the caller must check
 * result->program_id against the package. */
bool native_prepare_exefs(const char *title,const char *exefs_dir,const char *manifest_path,
    const uint8_t contract[32],NativePrepareHash sha256,NativePrepareResult *result,
    char *why,size_t why_size);
#ifdef __cplusplus
}
#endif
#endif
