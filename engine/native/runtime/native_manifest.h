/* Original-image metadata for continuous native execution; no generated code. */
#ifndef SHARED_RECOMP_NATIVE_MANIFEST_H
#define SHARED_RECOMP_NATIVE_MANIFEST_H
#include "../module_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NATIVE_MANIFEST_VERSION 1u
#define NATIVE_MANIFEST_HEADER_SIZE 128u
#define NATIVE_MANIFEST_MODULE_SIZE 104u
#define NATIVE_MANIFEST_MAX_SIZE (NATIVE_MANIFEST_HEADER_SIZE + MODULE_ABI_MAX_MODULES * NATIVE_MANIFEST_MODULE_SIZE)
#define NATIVE_MANIFEST_NO_INDEX UINT32_MAX
#define NATIVE_MANIFEST_API __attribute__((visibility("default")))

typedef struct NativeModuleSpec {
    uint32_t struct_size;
    uint32_t module_id; /* stable ModuleAbiModuleId; independent of array order */
    uint8_t nso_build_id[32];
    uint8_t nso_sha256[32]; /* complete original file, verified by caller */
    uint64_t canonical_base;
    uint64_t mapped_size; /* includes BSS and alignment padding */
    uint64_t text_offset;
    uint64_t text_size;
} NativeModuleSpec;

/* Entirely owned/copyable metadata. Module array order is symbol lookup order.
 * Title IDs are nonzero adapter identifiers, not array positions/program IDs.
 * The caller must independently accept the title/build/startup/service policy. */
typedef struct NativeManifest {
    uint32_t struct_size;
    uint32_t version;
    uint32_t title_id;
    uint32_t module_count;
    uint64_t program_id;
    uint32_t mapping_alignment; /* power of two, 4096..65536; verify host policy */
    uint32_t reserved;
    ModuleAbiIdentity identity;
    uint8_t contract_sha256[32]; /* expected native runtime contract, not game C */
    NativeModuleSpec modules[MODULE_ABI_MAX_MODULES];
} NativeManifest;

enum NativeManifestStatus {
    NATIVE_MANIFEST_OK, NATIVE_MANIFEST_ARGUMENT, NATIVE_MANIFEST_FORMAT,
    NATIVE_MANIFEST_VERSION_MISMATCH, NATIVE_MANIFEST_CONTRACT,
    NATIVE_MANIFEST_IDENTITY, NATIVE_MANIFEST_MODULE,
    NATIVE_MANIFEST_RANGE, NATIVE_MANIFEST_OVERLAP, NATIVE_MANIFEST_INIT_TARGET
};
typedef struct NativeManifestFailure {
    enum NativeManifestStatus status;
    uint32_t module_index;
    uint32_t other_module_index;
    size_t target_index;
} NativeManifestFailure;

/* Pure, allocation-free validation. Readable immutable inputs are required.
 * Supports CPU3/Android ARM64 only, 1..4 unique known module IDs with one main.
 * Checks canonical/actual arithmetic, mapping/text alignment, full nonblank
 * identities, all overlaps and zero unused slots. No files, hash computation,
 * reservation, module loading, signature/authenticity or execution is implied. */
NATIVE_MANIFEST_API enum NativeManifestStatus native_manifest_validate(
    const NativeManifest *manifest, const uint8_t expected_contract_sha256[32],
    NativeManifestFailure *failure);

/* Exact-size, unaligned-safe little-endian NATVM001 format:
 * header: magic0, length8, version12, title16, count20, program24, target32,
 * CPU36, size40, align44, mapping48, reserved52, shift56, window64/72,
 * contract80[32], reserved112[16]. Each 104B record: id0, reserved4,
 * build8[32], hash40[32], canonical72, mapped80, text88/96.
 * No generated-function counts, code/library digests or native pointers.
 * Output is cleared on failure; input/expected/output must not overlap. */
NATIVE_MANIFEST_API enum NativeManifestStatus native_manifest_parse(
    const void *bytes, size_t length, const uint8_t expected_contract_sha256[32],
    NativeManifest *out, NativeManifestFailure *failure);

/* Query helpers require a previously validated, immutable manifest. Return NULL
 * /0 for unknown IDs, unaligned PCs or addresses outside original text. */
NATIVE_MANIFEST_API const NativeModuleSpec *native_manifest_find_module(
    const NativeManifest *manifest, uint32_t module_id);
NATIVE_MANIFEST_API int native_manifest_is_code_address(
    const NativeManifest *manifest, uint64_t actual_pc);

/* Validate relocated init/fini entries against ALL original loaded text, not
 * AOT function boundaries. Null entries are allowed; host/HLE/island addresses
 * are rejected. Must run after relocation preflight and before guest execution.
 * Revalidates manifest; does not dereference PCs or the guest init-array itself. */
NATIVE_MANIFEST_API enum NativeManifestStatus native_manifest_validate_init_targets(
    const NativeManifest *manifest, const uint8_t expected_contract_sha256[32],
    const uint64_t *actual_targets, size_t count, NativeManifestFailure *failure);
NATIVE_MANIFEST_API const char *native_manifest_status_string(enum NativeManifestStatus status);

#ifdef __cplusplus
}
#endif
#endif
