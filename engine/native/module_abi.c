#include "module_abi.h"

_Static_assert(sizeof(void *) == 8, "provisional contract requires LP64 pointers");
_Static_assert(sizeof(ModuleAbiGuestFn) == 8, "provisional contract requires 64-bit function pointers");
_Static_assert(sizeof(ModuleAbiIdentity) == 48, "identity layout");
_Static_assert(sizeof(ModuleAbiModuleSpec) == 112, "module spec layout");
_Static_assert(sizeof(ModuleAbiModule) == 144, "module descriptor layout");
_Static_assert(sizeof(ModuleAbiDescriptor) == 64, "descriptor layout");
_Static_assert(sizeof(ModuleAbiProfile) == 64, "profile layout");

static enum ModuleAbiStatus fail(ModuleAbiFailure *out, enum ModuleAbiStatus status,
                                uint32_t module, uint64_t function) {
    if (out) {
        out->status = status;
        out->module_index = module;
        out->function_index = function;
    }
    return status;
}

static int bytes_equal(const uint8_t *a, const uint8_t *b, size_t count) {
    for (size_t i = 0; i < count; ++i) if (a[i] != b[i]) return 0;
    return 1;
}

static int nonzero_digest(const uint8_t *p) {
    for (size_t i = 0; i < 32; ++i) if (p[i]) return 1;
    return 0;
}

static enum ModuleAbiStatus check_identity(const ModuleAbiIdentity *id) {
    if (id->struct_size != sizeof *id) return MODULE_ABI_SIZE;
    if (id->abi_version != MODULE_ABI_VERSION) return MODULE_ABI_VERSION_MISMATCH;
    if (id->target != MODULE_ABI_TARGET_ANDROID_ARM64_LP64) return MODULE_ABI_TARGET_MISMATCH;
    if (id->cpu_alignment != 16 ||
        !((id->cpu_identity == MODULE_ABI_CPU_A64_PREFIX_1024 && id->cpu_size == 1024) ||
          ((id->cpu_identity == MODULE_ABI_CPU_A64_CACHE_66560 ||
            id->cpu_identity == MODULE_ABI_CPU_A64_SHARED_66560) && id->cpu_size == 66560)))
        return MODULE_ABI_CPU_MISMATCH;
    if (!id->guest_window_lo || id->guest_window_lo >= id->guest_window_hi ||
        (id->guest_shift & 3) || (id->guest_window_lo & 3) || (id->guest_window_hi & 3))
        return MODULE_ABI_LAYOUT;
    return MODULE_ABI_OK;
}

static enum ModuleAbiStatus compare_identity(const ModuleAbiIdentity *a,
                                            const ModuleAbiIdentity *b) {
    if (a->cpu_identity != b->cpu_identity || a->cpu_size != b->cpu_size ||
        a->cpu_alignment != b->cpu_alignment) return MODULE_ABI_CPU_MISMATCH;
    if (a->guest_shift != b->guest_shift || a->guest_window_lo != b->guest_window_lo ||
        a->guest_window_hi != b->guest_window_hi) return MODULE_ABI_PROFILE_MISMATCH;
    return MODULE_ABI_OK;
}

/* Checked endpoints are returned only after every subtraction/addition is safe. */
static enum ModuleAbiStatus check_spec(const ModuleAbiModuleSpec *spec,
                                      const ModuleAbiIdentity *id, uint32_t index,
                                      uint64_t *base, uint64_t *end) {
    if (spec->struct_size != sizeof *spec) return MODULE_ABI_SIZE;
    if (spec->module_id != index + MODULE_ABI_MAIN) return MODULE_ABI_PROFILE_MISMATCH;
    if (!nonzero_digest(spec->nso_build_id) || !nonzero_digest(spec->nso_sha256))
        return MODULE_ABI_METADATA_MISMATCH;
    if (!spec->mapped_size || !spec->text_size || !spec->function_count ||
        spec->canonical_base < id->guest_shift ||
        spec->text_offset > spec->mapped_size ||
        spec->text_size > spec->mapped_size - spec->text_offset ||
        (spec->canonical_base & 3) || (spec->text_offset & 3) || (spec->text_size & 3))
        return MODULE_ABI_LAYOUT;
    *base = spec->canonical_base - id->guest_shift;
    if (*base < id->guest_window_lo || *base >= id->guest_window_hi ||
        spec->mapped_size > id->guest_window_hi - *base) return MODULE_ABI_LAYOUT;
    *end = *base + spec->mapped_size;
    if (spec->function_count > SIZE_MAX / sizeof(ModuleAbiGuestFn) ||
        spec->function_count > SIZE_MAX / sizeof(uint64_t) ||
        spec->function_count > spec->text_size / 4) return MODULE_ABI_TABLE;
    return MODULE_ABI_OK;
}

static int same_spec(const ModuleAbiModuleSpec *a, const ModuleAbiModuleSpec *b) {
    return a->module_id == b->module_id &&
           bytes_equal(a->nso_build_id, b->nso_build_id, 32) &&
           bytes_equal(a->nso_sha256, b->nso_sha256, 32) &&
           a->canonical_base == b->canonical_base && a->mapped_size == b->mapped_size &&
           a->text_offset == b->text_offset && a->text_size == b->text_size &&
           a->function_count == b->function_count;
}

enum ModuleAbiStatus module_abi_validate_profile(const ModuleAbiProfile *profile,
                                               ModuleAbiFailure *failure) {
    uint64_t base[MODULE_ABI_MAX_MODULES], end[MODULE_ABI_MAX_MODULES];
    fail(failure, MODULE_ABI_OK, MODULE_ABI_NO_MODULE, MODULE_ABI_NO_FUNCTION);
    if (!profile)
        return fail(failure, MODULE_ABI_NULL, MODULE_ABI_NO_MODULE, MODULE_ABI_NO_FUNCTION);
    if (profile->struct_size != sizeof *profile)
        return fail(failure, MODULE_ABI_SIZE, MODULE_ABI_NO_MODULE, MODULE_ABI_NO_FUNCTION);
    enum ModuleAbiStatus status = check_identity(&profile->identity);
    if (status != MODULE_ABI_OK)
        return fail(failure, status, MODULE_ABI_NO_MODULE, MODULE_ABI_NO_FUNCTION);
    if (profile->module_count != 2 && profile->module_count != 4)
        return fail(failure, MODULE_ABI_PROFILE_MISMATCH, MODULE_ABI_NO_MODULE, MODULE_ABI_NO_FUNCTION);
    if (!profile->modules)
        return fail(failure, MODULE_ABI_NULL, MODULE_ABI_NO_MODULE, MODULE_ABI_NO_FUNCTION);
    for (uint32_t i = 0; i < profile->module_count; ++i) {
        status = check_spec(&profile->modules[i], &profile->identity, i, &base[i], &end[i]);
        if (status != MODULE_ABI_OK) return fail(failure, status, i, MODULE_ABI_NO_FUNCTION);
        for (uint32_t j = 0; j < i; ++j)
            if (base[i] < end[j] && base[j] < end[i])
                return fail(failure, MODULE_ABI_LAYOUT, i, MODULE_ABI_NO_FUNCTION);
    }
    return MODULE_ABI_OK;
}

enum ModuleAbiStatus module_abi_validate(const ModuleAbiDescriptor *descriptor,
                                       const ModuleAbiProfile *expected,
                                       ModuleAbiFailure *failure) {
    uint64_t expected_base[MODULE_ABI_MAX_MODULES], expected_end[MODULE_ABI_MAX_MODULES];
    uint64_t actual_base[MODULE_ABI_MAX_MODULES], actual_end[MODULE_ABI_MAX_MODULES];
    enum ModuleAbiStatus status;
    fail(failure, MODULE_ABI_OK, MODULE_ABI_NO_MODULE, MODULE_ABI_NO_FUNCTION);
    if (!descriptor || !expected)
        return fail(failure, MODULE_ABI_NULL, MODULE_ABI_NO_MODULE, MODULE_ABI_NO_FUNCTION);
    if (descriptor->struct_size != sizeof *descriptor || expected->struct_size != sizeof *expected)
        return fail(failure, MODULE_ABI_SIZE, MODULE_ABI_NO_MODULE, MODULE_ABI_NO_FUNCTION);
    status = check_identity(&expected->identity);
    if (status == MODULE_ABI_OK) status = check_identity(&descriptor->identity);
    if (status == MODULE_ABI_OK) status = compare_identity(&descriptor->identity, &expected->identity);
    if (status != MODULE_ABI_OK)
        return fail(failure, status, MODULE_ABI_NO_MODULE, MODULE_ABI_NO_FUNCTION);
    if ((expected->module_count != 2 && expected->module_count != 4) ||
        descriptor->module_count != expected->module_count)
        return fail(failure, MODULE_ABI_PROFILE_MISMATCH, MODULE_ABI_NO_MODULE, MODULE_ABI_NO_FUNCTION);
    if (!descriptor->modules || !expected->modules)
        return fail(failure, MODULE_ABI_NULL, MODULE_ABI_NO_MODULE, MODULE_ABI_NO_FUNCTION);
    for (uint32_t i = 0; i < expected->module_count; ++i) {
        const ModuleAbiModule *module = &descriptor->modules[i];
        const ModuleAbiModuleSpec *want = &expected->modules[i];
        if (module->struct_size != sizeof *module)
            return fail(failure, MODULE_ABI_SIZE, i, MODULE_ABI_NO_FUNCTION);
        if (module->reserved)
            return fail(failure, MODULE_ABI_PROFILE_MISMATCH, i, MODULE_ABI_NO_FUNCTION);
        status = check_spec(want, &expected->identity, i, &expected_base[i], &expected_end[i]);
        if (status == MODULE_ABI_OK)
            status = check_spec(&module->image, &descriptor->identity, i, &actual_base[i], &actual_end[i]);
        if (status != MODULE_ABI_OK) return fail(failure, status, i, MODULE_ABI_NO_FUNCTION);
        for (uint32_t j = 0; j < i; ++j) {
            if ((expected_base[i] < expected_end[j] && expected_base[j] < expected_end[i]) ||
                (actual_base[i] < actual_end[j] && actual_base[j] < actual_end[i]))
                return fail(failure, MODULE_ABI_LAYOUT, i, MODULE_ABI_NO_FUNCTION);
        }
        if (!same_spec(&module->image, want))
            return fail(failure, MODULE_ABI_METADATA_MISMATCH, i, MODULE_ABI_NO_FUNCTION);
        if (module->address_count != module->image.function_count)
            return fail(failure, MODULE_ABI_TABLE, i, MODULE_ABI_NO_FUNCTION);
        if (!module->addresses || !module->functions)
            return fail(failure, MODULE_ABI_NULL, i, MODULE_ABI_NO_FUNCTION);
        uint64_t text_lo = actual_base[i] + module->image.text_offset;
        uint64_t text_hi = text_lo + module->image.text_size;
        for (uint64_t f = 0; f < module->address_count; ++f) {
            uint64_t addr = module->addresses[f];
            if ((addr & 3) || addr < text_lo || addr >= text_hi ||
                (f && module->addresses[f - 1] >= addr))
                return fail(failure, MODULE_ABI_TABLE, i, f);
            if (!module->functions[f]) return fail(failure, MODULE_ABI_FUNCTION, i, f);
        }
    }
    return MODULE_ABI_OK;
}

const char *module_abi_status_string(enum ModuleAbiStatus status) {
    switch (status) {
    case MODULE_ABI_OK: return "ok";
    case MODULE_ABI_NULL: return "null pointer";
    case MODULE_ABI_SIZE: return "structure size mismatch";
    case MODULE_ABI_VERSION_MISMATCH: return "ABI version mismatch";
    case MODULE_ABI_TARGET_MISMATCH: return "target mismatch";
    case MODULE_ABI_CPU_MISMATCH: return "CPU layout mismatch";
    case MODULE_ABI_PROFILE_MISMATCH: return "profile mismatch";
    case MODULE_ABI_LAYOUT: return "invalid module or address layout";
    case MODULE_ABI_METADATA_MISMATCH: return "module metadata mismatch";
    case MODULE_ABI_TABLE: return "invalid function table";
    case MODULE_ABI_FUNCTION: return "null function entry";
    default: return "unknown validation status";
    }
}
