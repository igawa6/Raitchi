#include "title_manifest.h"
#include <string.h>

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static int nonzero(const uint8_t *p) {
    uint8_t any = 0;
    for (unsigned i = 0; i < 32; ++i) any |= p[i];
    return any != 0;
}

enum TitleManifestStatus title_manifest_parse(const void *bytes, size_t length,
    const uint8_t expected_contract[32], TitleManifest *out, ModuleAbiFailure *failure) {
    if (out) memset(out, 0, sizeof *out);
    if (failure) *failure = (ModuleAbiFailure){MODULE_ABI_OK, MODULE_ABI_NO_MODULE, MODULE_ABI_NO_FUNCTION};
    if (!bytes || !expected_contract || !out) return TITLE_MANIFEST_ARGUMENT;
    const uint8_t *p = bytes;
    if (length < TITLE_MANIFEST_HEADER_SIZE || memcmp(p, "SAOTM001", 8) ||
        le32(p + 8) != length || le32(p + 12) != TITLE_MANIFEST_VERSION ||
        le32(p + 16) != 1 || /* runtime API */
        (le32(p + 20) != 1 && le32(p + 20) != 2) || /* supported title IDs */
        le32(p + 24) != MODULE_ABI_TARGET_ANDROID_ARM64_LP64 ||
        le32(p + 28) != 30 || /* minimum Android API */
        le32(p + 32) != MODULE_ABI_CPU_A64_SHARED_66560 ||
        le32(p + 36) != 66560 || le32(p + 40) != 16 || le32(p + 44) != 1 || /* ARM64 SIMD asm */
        (le32(p + 48) & ~7u)) return TITLE_MANIFEST_FORMAT;
    uint32_t count = le32(p + 52);
    if ((count != 2 && count != 4) ||
        length != TITLE_MANIFEST_HEADER_SIZE + (size_t)count * TITLE_MANIFEST_MODULE_SIZE)
        return TITLE_MANIFEST_FORMAT;
    if (!nonzero(expected_contract) || memcmp(p + 80, expected_contract, 32) ||
        !nonzero(p + 112) || !nonzero(p + 144)) return TITLE_MANIFEST_CONTRACT;
    TitleManifest result = {.title_id = le32(p + 20), .capabilities = le32(p + 48),
        .profile = {.struct_size = sizeof(ModuleAbiProfile), .module_count = count,
            .identity = {.struct_size = sizeof(ModuleAbiIdentity), .abi_version = MODULE_ABI_VERSION,
                .target = MODULE_ABI_TARGET_ANDROID_ARM64_LP64,
                .cpu_identity = MODULE_ABI_CPU_A64_SHARED_66560,
                .cpu_size = 66560, .cpu_alignment = 16,
                .guest_shift = le64(p + 56), .guest_window_lo = le64(p + 64),
                .guest_window_hi = le64(p + 72)}}};
    memcpy(result.contract_sha256, p + 80, 32);
    memcpy(result.code_sha256, p + 112, 32);
    memcpy(result.library_sha256, p + 144, 32);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *record = p + TITLE_MANIFEST_HEADER_SIZE + (size_t)i * TITLE_MANIFEST_MODULE_SIZE;
        if (le32(record + 4)) return TITLE_MANIFEST_FORMAT;
        ModuleAbiModuleSpec *module = &result.modules[i];
        *module = (ModuleAbiModuleSpec){.struct_size = sizeof *module, .module_id = le32(record),
            .canonical_base = le64(record + 72), .mapped_size = le64(record + 80),
            .text_offset = le64(record + 88), .text_size = le64(record + 96),
            .function_count = le64(record + 104)};
        memcpy(module->nso_build_id, record + 8, 32);
        memcpy(module->nso_sha256, record + 40, 32);
    }
    result.profile.modules = result.modules;
    if (module_abi_validate_profile(&result.profile, failure) != MODULE_ABI_OK)
        return TITLE_MANIFEST_PROFILE;
    *out = result;
    out->profile.modules = out->modules;
    return TITLE_MANIFEST_OK;
}
