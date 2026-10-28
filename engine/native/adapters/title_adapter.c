#include "title_adapter.h"
#include "../runtime/aot_cpu.h"

#include <stdatomic.h>
#include <string.h>

extern const TitleAdapter las_title_adapter;
extern const TitleAdapter eevee_title_adapter;
extern const TitleAdapter echoes_title_adapter;
extern const TitleAdapter wonder_title_adapter;
static _Atomic int selected_title;
/* 0 empty, 1 being written, 2 published (immutable). */
static _Atomic int generic_state;
static TitleGenericAdapter generic_adapter;

const TitleAdapter *title_adapter_find(enum TitleId id) {
    switch (id) {
    case TITLE_LAS: return &las_title_adapter;
    case TITLE_EEVEE: return &eevee_title_adapter;
    case TITLE_ECHOES: return &echoes_title_adapter;
    case TITLE_WONDER: return &wonder_title_adapter;
    case TITLE_GENERIC:
        return atomic_load_explicit(&generic_state, memory_order_acquire) == 2 ? &generic_adapter.adapter : NULL;
    default: return NULL;
    }
}

enum TitleAdapterStatus title_adapter_install_generic(const TitleGenericAdapter *generic) {
    if (!generic) return TITLE_ADAPTER_ARGUMENT;
    const TitleAdapter *a = &generic->adapter;
    if (a->id != TITLE_GENERIC || !a->key || strcmp(a->key, TITLE_GENERIC_KEY) || !a->program_id ||
        !a->module_count || a->module_count > MODULE_ABI_MAX_MODULES || a->capabilities ||
        !memchr(generic->version, 0, sizeof generic->version) ||
        !memchr(generic->save_namespace, 0, sizeof generic->save_namespace) || !generic->save_namespace[0])
        return TITLE_ADAPTER_PROFILE;
    if (atomic_load_explicit(&selected_title, memory_order_acquire)) return TITLE_ADAPTER_ALREADY_SELECTED;
    int empty = 0;
    if (!atomic_compare_exchange_strong_explicit(&generic_state, &empty, 1,
            memory_order_acq_rel, memory_order_relaxed))
        return TITLE_ADAPTER_ALREADY_SELECTED;
    generic_adapter = *generic;
    generic_adapter.adapter.version = generic_adapter.version;
    generic_adapter.adapter.save_namespace = generic_adapter.save_namespace;
    atomic_store_explicit(&generic_state, 2, memory_order_release);
    return TITLE_ADAPTER_OK;
}

static enum TitleAdapterStatus fail(TitleAdapterFailure *failure,
    enum TitleAdapterStatus status, uint32_t module) {
    if (failure) { failure->status = status; failure->module_index = module; }
    return status;
}

static void reset_failure(TitleAdapterFailure *failure) {
    if (!failure) return;
    memset(failure, 0, sizeof *failure);
    failure->module_index = failure->abi.module_index = MODULE_ABI_NO_MODULE;
    failure->abi.function_index = MODULE_ABI_NO_FUNCTION;
    failure->layout.first_index = failure->layout.second_index = SESSION_LAYOUT_NO_INDEX;
    failure->native.module_index = failure->native.other_module_index = NATIVE_MANIFEST_NO_INDEX;
    failure->native.target_index = SIZE_MAX;
}

enum TitleAdapterStatus title_adapter_build_layout(enum TitleId id,
    const ModuleAbiProfile *profile, uint32_t artifact_capabilities, uint32_t page_size,
    SessionLayoutSpec *out, TitleAdapterFailure *failure) {
    if (out) memset(out, 0, sizeof *out);
    reset_failure(failure);
    if (!profile || !out) return fail(failure, TITLE_ADAPTER_ARGUMENT, MODULE_ABI_NO_MODULE);
    const TitleAdapter *adapter = title_adapter_find(id);
    if (!adapter) return fail(failure, TITLE_ADAPTER_UNKNOWN, MODULE_ABI_NO_MODULE);
    if (id == TITLE_ECHOES || id == TITLE_WONDER || id == TITLE_GENERIC) return fail(failure, TITLE_ADAPTER_AOT_UNSUPPORTED, MODULE_ABI_NO_MODULE);
    ModuleAbiFailure abi;
    if (module_abi_validate_profile(profile, &abi) != MODULE_ABI_OK) {
        if (failure) failure->abi = abi;
        return fail(failure, TITLE_ADAPTER_PROFILE, abi.module_index);
    }
    if (artifact_capabilities != adapter->capabilities)
        return fail(failure, TITLE_ADAPTER_CAPABILITIES, MODULE_ABI_NO_MODULE);
    const ModuleAbiIdentity *identity = &profile->identity;
    if (identity->cpu_identity != MODULE_ABI_CPU_A64_SHARED_66560 ||
        identity->cpu_size != sizeof(Cpu) || identity->cpu_alignment != _Alignof(Cpu) ||
        identity->guest_shift != adapter->guest_shift ||
        identity->guest_window_lo != adapter->canonical_window_lo - adapter->guest_shift ||
        identity->guest_window_hi != adapter->canonical_window_hi - adapter->guest_shift ||
        profile->module_count != adapter->module_count)
        return fail(failure, TITLE_ADAPTER_PROFILE, MODULE_ABI_NO_MODULE);
    SessionLayoutSpec spec = {.struct_size = sizeof spec, .version = SESSION_LAYOUT_VERSION,
        .module_count = profile->module_count, .page_size = page_size,
        .guest_shift = adapter->guest_shift, .canonical_window_lo = adapter->canonical_window_lo,
        .canonical_window_hi = adapter->canonical_window_hi};
    memcpy(spec.regions, adapter->regions, sizeof spec.regions);
    for (uint32_t i = 0; i < profile->module_count; ++i) {
        const ModuleAbiModuleSpec *module = &profile->modules[i];
        const TitleModulePolicy *policy = &adapter->modules[i];
        if (module->module_id != policy->module_id || module->canonical_base != policy->canonical_base)
            return fail(failure, TITLE_ADAPTER_PROFILE, i);
        spec.modules[i] = (SessionModuleSpec){module->module_id, 0,
            {module->canonical_base, module->mapped_size}};
    }
    if (!adapter->main_build_prefix_size || adapter->main_build_prefix_size > 20 ||
        memcmp(profile->modules[0].nso_build_id, adapter->main_build_prefix,
               adapter->main_build_prefix_size))
        return fail(failure, TITLE_ADAPTER_BUILD, 0);
    SessionLayout normalized;
    SessionLayoutFailure layout;
    if (session_layout_build(&spec, &normalized, &layout) != SESSION_LAYOUT_OK) {
        if (failure) failure->layout = layout;
        return fail(failure, TITLE_ADAPTER_LAYOUT, MODULE_ABI_NO_MODULE);
    }
    *out = spec;
    return TITLE_ADAPTER_OK;
}

enum TitleAdapterStatus title_adapter_build_native_layout(enum TitleId id,
    const NativeManifest *manifest, const uint8_t expected[32], uint32_t page_size,
    SessionLayoutSpec *out, TitleAdapterFailure *failure) {
    if (out) memset(out, 0, sizeof *out);
    reset_failure(failure);
    if (!manifest || !expected || !out) return fail(failure, TITLE_ADAPTER_ARGUMENT, MODULE_ABI_NO_MODULE);
    const TitleAdapter *adapter = title_adapter_find(id);
    if (!adapter) return fail(failure, TITLE_ADAPTER_UNKNOWN, MODULE_ABI_NO_MODULE);
    return title_adapter_build_native_layout_for(adapter, manifest, expected, page_size, out, failure);
}

enum TitleAdapterStatus title_adapter_build_native_layout_for(const TitleAdapter *adapter,
    const NativeManifest *manifest, const uint8_t expected[32], uint32_t page_size,
    SessionLayoutSpec *out, TitleAdapterFailure *failure) {
    if (out) memset(out, 0, sizeof *out);
    reset_failure(failure);
    if (!adapter || !manifest || !expected || !out) return fail(failure, TITLE_ADAPTER_ARGUMENT, MODULE_ABI_NO_MODULE);
    const enum TitleId id = adapter->id;
    NativeManifestFailure native;
    if (native_manifest_validate(manifest, expected, &native) != NATIVE_MANIFEST_OK) {
        if (failure) failure->native = native;
        return fail(failure, TITLE_ADAPTER_PROFILE, native.module_index);
    }
    const ModuleAbiIdentity *identity = &manifest->identity;
    if (manifest->title_id != (uint32_t)id || manifest->program_id != adapter->program_id ||
        identity->guest_shift != adapter->guest_shift ||
        identity->guest_window_lo != adapter->canonical_window_lo - adapter->guest_shift ||
        identity->guest_window_hi != adapter->canonical_window_hi - adapter->guest_shift ||
        manifest->module_count != adapter->module_count)
        return fail(failure, TITLE_ADAPTER_PROFILE, MODULE_ABI_NO_MODULE);
    if (!page_size || manifest->mapping_alignment < page_size || manifest->mapping_alignment % page_size)
        return fail(failure, TITLE_ADAPTER_LAYOUT, MODULE_ABI_NO_MODULE);
    if ((adapter->capabilities & ~AOT_CAP_ALL) ||
        ((id == TITLE_ECHOES || id == TITLE_WONDER || id == TITLE_GENERIC) && adapter->capabilities))
        return fail(failure, TITLE_ADAPTER_CAPABILITIES, MODULE_ABI_NO_MODULE);
    SessionLayoutSpec spec = {.struct_size = sizeof spec, .version = SESSION_LAYOUT_VERSION,
        .module_count = manifest->module_count, .page_size = page_size,
        .guest_shift = adapter->guest_shift, .canonical_window_lo = adapter->canonical_window_lo,
        .canonical_window_hi = adapter->canonical_window_hi};
    memcpy(spec.regions, adapter->regions, sizeof spec.regions);
    for (uint32_t i = 0; i < manifest->module_count; ++i) {
        const NativeModuleSpec *module = &manifest->modules[i];
        const TitleModulePolicy *policy = &adapter->modules[i];
        if (module->module_id != policy->module_id || module->canonical_base != policy->canonical_base ||
            (adapter->module_spans[i] && module->mapped_size > adapter->module_spans[i]))
            return fail(failure, TITLE_ADAPTER_PROFILE, i);
        uint32_t prefix = adapter->module_build_prefix_size[i];
        if (prefix > 20 || (prefix && memcmp(module->nso_build_id, adapter->module_build_prefix[i], prefix)))
            return fail(failure, TITLE_ADAPTER_BUILD, i);
        spec.modules[i] = (SessionModuleSpec){module->module_id, 0,
            {module->canonical_base, module->mapped_size}};
    }
    const NativeModuleSpec *main = native_manifest_find_module(manifest, MODULE_ABI_MAIN);
    if (!main || !adapter->main_build_prefix_size || adapter->main_build_prefix_size > 20 ||
        memcmp(main->nso_build_id, adapter->main_build_prefix, adapter->main_build_prefix_size))
        return fail(failure, TITLE_ADAPTER_BUILD, 0);
    SessionLayout normalized;
    SessionLayoutFailure layout;
    if (session_layout_build(&spec, &normalized, &layout) != SESSION_LAYOUT_OK) {
        if (failure) failure->layout = layout;
        return fail(failure, TITLE_ADAPTER_LAYOUT, MODULE_ABI_NO_MODULE);
    }
    *out = spec;
    return TITLE_ADAPTER_OK;
}

enum TitleAdapterStatus title_adapter_select(enum TitleId id) {
    if (!title_adapter_find(id)) return TITLE_ADAPTER_UNKNOWN;
    int empty = 0;
    return atomic_compare_exchange_strong_explicit(&selected_title, &empty, id,
        memory_order_release, memory_order_relaxed) ? TITLE_ADAPTER_OK : TITLE_ADAPTER_ALREADY_SELECTED;
}

const TitleAdapter *title_adapter_selected(void) {
    return title_adapter_find(atomic_load_explicit(&selected_title, memory_order_acquire));
}

const char *title_adapter_status_string(enum TitleAdapterStatus status) {
    switch (status) {
    case TITLE_ADAPTER_OK: return "ok";
    case TITLE_ADAPTER_ARGUMENT: return "invalid title-adapter argument";
    case TITLE_ADAPTER_UNKNOWN: return "unknown title adapter";
    case TITLE_ADAPTER_PROFILE: return "title/profile mismatch";
    case TITLE_ADAPTER_CAPABILITIES: return "title capability mismatch";
    case TITLE_ADAPTER_BUILD: return "unsupported title build prefix";
    case TITLE_ADAPTER_LAYOUT: return "title session layout rejected";
    case TITLE_ADAPTER_ALREADY_SELECTED: return "title already selected; start a fresh process";
    case TITLE_ADAPTER_AOT_UNSUPPORTED: return "This title requires a native-only session; AOT is unsupported";
    default: return "unknown title-adapter status";
    }
}
