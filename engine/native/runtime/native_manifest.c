#include "native_manifest.h"
#include <string.h>

_Static_assert(sizeof(NativeModuleSpec) == 104, "native module metadata layout");

static enum NativeManifestStatus fail(NativeManifestFailure *out,
    enum NativeManifestStatus status, uint32_t module, uint32_t other, size_t target) {
    if (out) *out = (NativeManifestFailure){status, module, other, target};
    return status;
}
#define FAIL(s, m, o) fail(failure, (s), (m), (o), SIZE_MAX)
#define HEADER(s) FAIL((s), NATIVE_MANIFEST_NO_INDEX, NATIVE_MANIFEST_NO_INDEX)

static int nonzero(const uint8_t *bytes, size_t count) {
    for (size_t i = 0; i < count; ++i) if (bytes[i]) return 1;
    return 0;
}
static int blank_module(const NativeModuleSpec *m) {
    return !m->struct_size && !m->module_id && !m->canonical_base && !m->mapped_size &&
        !m->text_offset && !m->text_size && !nonzero(m->nso_build_id, 32) &&
        !nonzero(m->nso_sha256, 32);
}
enum NativeManifestStatus native_manifest_validate(const NativeManifest *manifest,
    const uint8_t expected[32], NativeManifestFailure *failure) {
    HEADER(NATIVE_MANIFEST_OK);
    if (!manifest || !expected) return HEADER(NATIVE_MANIFEST_ARGUMENT);
    if (manifest->struct_size != sizeof *manifest || manifest->reserved)
        return HEADER(NATIVE_MANIFEST_FORMAT);
    if (manifest->version != NATIVE_MANIFEST_VERSION)
        return HEADER(NATIVE_MANIFEST_VERSION_MISMATCH);
    if (!manifest->title_id || !manifest->program_id || !manifest->module_count ||
        manifest->module_count > MODULE_ABI_MAX_MODULES)
        return HEADER(NATIVE_MANIFEST_FORMAT);
    if (!nonzero(expected, 32) || memcmp(expected, manifest->contract_sha256, 32))
        return HEADER(NATIVE_MANIFEST_CONTRACT);
    const ModuleAbiIdentity *id = &manifest->identity;
    if (id->struct_size != sizeof *id || id->abi_version != MODULE_ABI_VERSION ||
        id->target != MODULE_ABI_TARGET_ANDROID_ARM64_LP64 ||
        id->cpu_identity != MODULE_ABI_CPU_A64_SHARED_66560 ||
        id->cpu_size != 66560 || id->cpu_alignment != 16)
        return HEADER(NATIVE_MANIFEST_IDENTITY);
    uint32_t align = manifest->mapping_alignment;
    if (align < 4096 || align > 65536 || (align & (align - 1u)) ||
        !id->guest_window_lo || id->guest_window_lo >= id->guest_window_hi ||
        ((id->guest_shift | id->guest_window_lo | id->guest_window_hi) & (align - 1u)) ||
        id->guest_window_hi > UINT64_MAX - id->guest_shift)
        return HEADER(NATIVE_MANIFEST_RANGE);
    uint64_t bases[MODULE_ABI_MAX_MODULES], ends[MODULE_ABI_MAX_MODULES];
    uint32_t seen = 0;
    for (uint32_t i = 0; i < manifest->module_count; ++i) {
        const NativeModuleSpec *m = &manifest->modules[i];
        if (m->struct_size != sizeof *m || m->module_id < MODULE_ABI_MAIN ||
            m->module_id > MODULE_ABI_SUBSDK1 || !nonzero(m->nso_build_id, 32) ||
            !nonzero(m->nso_sha256, 32))
            return FAIL(NATIVE_MANIFEST_MODULE, i, NATIVE_MANIFEST_NO_INDEX);
        uint32_t bit = UINT32_C(1) << (m->module_id - MODULE_ABI_MAIN);
        if (seen & bit) return FAIL(NATIVE_MANIFEST_MODULE, i, NATIVE_MANIFEST_NO_INDEX);
        seen |= bit;
        if (!m->mapped_size || m->mapped_size > SIZE_MAX ||
            ((m->canonical_base | m->mapped_size) & (align - 1u)) ||
            m->canonical_base < id->guest_shift ||
            m->mapped_size > UINT64_MAX - m->canonical_base ||
            !m->text_size || ((m->text_offset | m->text_size) & 3u) ||
            m->text_offset > m->mapped_size || m->text_size > m->mapped_size - m->text_offset)
            return FAIL(NATIVE_MANIFEST_RANGE, i, NATIVE_MANIFEST_NO_INDEX);
        bases[i] = m->canonical_base - id->guest_shift;
        if (bases[i] < id->guest_window_lo || bases[i] >= id->guest_window_hi ||
            m->mapped_size > id->guest_window_hi - bases[i])
            return FAIL(NATIVE_MANIFEST_RANGE, i, NATIVE_MANIFEST_NO_INDEX);
        ends[i] = bases[i] + m->mapped_size;
        for (uint32_t j = 0; j < i; ++j)
            if (bases[i] < ends[j] && bases[j] < ends[i])
                return FAIL(NATIVE_MANIFEST_OVERLAP, i, j);
    }
    if (!(seen & UINT32_C(1))) /* MODULE_ABI_MAIN is the first stable ID */
        return HEADER(NATIVE_MANIFEST_MODULE);
    for (uint32_t i = manifest->module_count; i < MODULE_ABI_MAX_MODULES; ++i)
        if (!blank_module(&manifest->modules[i]))
            return FAIL(NATIVE_MANIFEST_FORMAT, i, NATIVE_MANIFEST_NO_INDEX);
    return NATIVE_MANIFEST_OK;
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

enum NativeManifestStatus native_manifest_parse(const void *bytes, size_t length,
    const uint8_t expected[32], NativeManifest *out, NativeManifestFailure *failure) {
    if (out) memset(out, 0, sizeof *out);
    HEADER(NATIVE_MANIFEST_OK);
    if (!bytes || !expected || !out) return HEADER(NATIVE_MANIFEST_ARGUMENT);
    const uint8_t *p = bytes;
    if (length < NATIVE_MANIFEST_HEADER_SIZE || memcmp(p, "NATVM001", 8) ||
        le32(p + 8) != length || le32(p + 52) || nonzero(p + 112, 16))
        return HEADER(NATIVE_MANIFEST_FORMAT);
    uint32_t count = le32(p + 20);
    if (!count || count > MODULE_ABI_MAX_MODULES ||
        length != NATIVE_MANIFEST_HEADER_SIZE + (size_t)count * NATIVE_MANIFEST_MODULE_SIZE)
        return HEADER(NATIVE_MANIFEST_FORMAT);
    NativeManifest candidate = {.struct_size = sizeof candidate,
        .version = le32(p + 12), .title_id = le32(p + 16), .module_count = count,
        .program_id = le64(p + 24), .mapping_alignment = le32(p + 48),
        .identity = {.struct_size = sizeof(ModuleAbiIdentity), .abi_version = MODULE_ABI_VERSION,
            .target = le32(p + 32), .cpu_identity = le32(p + 36), .cpu_size = le32(p + 40),
            .cpu_alignment = le32(p + 44), .guest_shift = le64(p + 56),
            .guest_window_lo = le64(p + 64), .guest_window_hi = le64(p + 72)}};
    memcpy(candidate.contract_sha256, p + 80, 32);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *r = p + NATIVE_MANIFEST_HEADER_SIZE + (size_t)i * NATIVE_MANIFEST_MODULE_SIZE;
        if (le32(r + 4)) return FAIL(NATIVE_MANIFEST_FORMAT, i, NATIVE_MANIFEST_NO_INDEX);
        NativeModuleSpec *m = &candidate.modules[i];
        *m = (NativeModuleSpec){.struct_size = sizeof *m, .module_id = le32(r),
            .canonical_base = le64(r + 72), .mapped_size = le64(r + 80),
            .text_offset = le64(r + 88), .text_size = le64(r + 96)};
        memcpy(m->nso_build_id, r + 8, 32); memcpy(m->nso_sha256, r + 40, 32);
    }
    enum NativeManifestStatus status = native_manifest_validate(&candidate, expected, failure);
    if (status == NATIVE_MANIFEST_OK) *out = candidate;
    return status;
}

const NativeModuleSpec *native_manifest_find_module(const NativeManifest *manifest, uint32_t module_id) {
    if (!manifest || !module_id || manifest->module_count > MODULE_ABI_MAX_MODULES) return NULL;
    for (uint32_t i = 0; i < manifest->module_count; ++i)
        if (manifest->modules[i].module_id == module_id) return &manifest->modules[i];
    return NULL;
}
int native_manifest_is_code_address(const NativeManifest *manifest, uint64_t pc) {
    if (!manifest || !pc || (pc & 3u) || manifest->module_count > MODULE_ABI_MAX_MODULES) return 0;
    for (uint32_t i = 0; i < manifest->module_count; ++i) {
        const NativeModuleSpec *m = &manifest->modules[i];
        if (m->canonical_base < manifest->identity.guest_shift) continue;
        uint64_t base = m->canonical_base - manifest->identity.guest_shift;
        if (pc >= base && pc - base >= m->text_offset && pc - base - m->text_offset < m->text_size)
            return 1;
    }
    return 0;
}
enum NativeManifestStatus native_manifest_validate_init_targets(const NativeManifest *manifest,
    const uint8_t expected[32], const uint64_t *targets, size_t count, NativeManifestFailure *failure) {
    enum NativeManifestStatus status = native_manifest_validate(manifest, expected, failure);
    if (status != NATIVE_MANIFEST_OK) return status;
    if ((!targets && count) || count > SIZE_MAX / sizeof *targets)
        return HEADER(NATIVE_MANIFEST_ARGUMENT);
    for (size_t i = 0; i < count; ++i)
        if (targets[i] && !native_manifest_is_code_address(manifest, targets[i]))
            return fail(failure, NATIVE_MANIFEST_INIT_TARGET,
                NATIVE_MANIFEST_NO_INDEX, NATIVE_MANIFEST_NO_INDEX, i);
    return NATIVE_MANIFEST_OK;
}
const char *native_manifest_status_string(enum NativeManifestStatus status) {
    switch (status) {
    case NATIVE_MANIFEST_OK: return "ok";
    case NATIVE_MANIFEST_ARGUMENT: return "invalid native manifest argument";
    case NATIVE_MANIFEST_FORMAT: return "invalid native manifest format";
    case NATIVE_MANIFEST_VERSION_MISMATCH: return "native manifest version mismatch";
    case NATIVE_MANIFEST_CONTRACT: return "native runtime contract mismatch";
    case NATIVE_MANIFEST_IDENTITY: return "unsupported native CPU/target identity";
    case NATIVE_MANIFEST_MODULE: return "invalid native module identity";
    case NATIVE_MANIFEST_RANGE: return "invalid native image/text range";
    case NATIVE_MANIFEST_OVERLAP: return "overlapping native module images";
    case NATIVE_MANIFEST_INIT_TARGET: return "initializer outside original native text";
    default: return "unknown native manifest status";
    }
}
