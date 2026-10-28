/* Initial title policies. File hashes/build metadata remain independent inputs. */
#ifndef SHARED_RECOMP_TITLE_ADAPTER_H
#define SHARED_RECOMP_TITLE_ADAPTER_H

#include "../module_abi.h"
#include "../runtime/session_layout.h"
#include "../runtime/native_manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TITLE_ADAPTER_API __attribute__((visibility("default")))

enum TitleId { TITLE_LAS = 1, TITLE_EEVEE = 2, TITLE_ECHOES = 3, TITLE_WONDER = 4,
    /* Fallback for a base program whose ID matches no title adapter. Its values
     * are derived from the package (native/adapters/generic/generic.c) and
     * installed once per process before selection. Native-only. */
    TITLE_GENERIC = 5 };
#define TITLE_GENERIC_KEY "generic"

/* Renderer compatibility policy, independent of AOT/artifact capabilities. */
enum TitleRendererPolicy {
    TITLE_RENDERER_LEGACY_TARGET_HEURISTICS = 1u << 0
};

typedef struct TitleModulePolicy {
    uint32_t module_id;
    const char *name;
    uint64_t canonical_base;
} TitleModulePolicy;

/* Module load/lookup order and startup stages are separate policies. IDs name
 * modules, not array positions. Echoes uses SDK16.2's five-argument start. */
typedef struct TitleStartupPolicy {
    uint32_t argument_count;
    uint32_t early_count;
    uint32_t early_modules[3];
    uint32_t late_count;
    uint32_t late_modules[3];
} TitleStartupPolicy;

typedef struct TitleAdapter {
    enum TitleId id;
    const char *key;
    const char *label;
    const char *version;
    const char *save_namespace;
    uint64_t program_id;
    uint64_t guest_shift;
    uint64_t canonical_window_lo;
    uint64_t canonical_window_hi;
    uint32_t capabilities;
    uint32_t audout_min_queue_ms; /* versioned service policy, not a separate mixer */
    uint32_t module_count;
    uint32_t nvn_api_major, nvn_api_minor; /* zero major retains the legacy 53.207 ABI */
    /* Public upstream recognition prefix only: LAS20 bytes, Eevee8 bytes.
     * This is not a complete hash or replacement for manifest/file checks. */
    uint32_t main_build_prefix_size;
    uint8_t main_build_prefix[20];
    /* Optional additional public version recognition, in declared lookup order.
     * Complete original hashes remain independently verified manifest inputs. */
    uint32_t module_build_prefix_size[MODULE_ABI_MAX_MODULES];
    uint8_t module_build_prefix[MODULE_ABI_MAX_MODULES][20];
    uint64_t module_spans[MODULE_ABI_MAX_MODULES];
    TitleModulePolicy modules[MODULE_ABI_MAX_MODULES];
    SessionRegionSpec regions[SESSION_REGION_COUNT];
    TitleStartupPolicy startup;
    uint32_t renderer_policy_flags; /* append-only host policy; not manifest/CPU ABI input */
} TitleAdapter;

enum TitleAdapterStatus {
    TITLE_ADAPTER_OK, TITLE_ADAPTER_ARGUMENT, TITLE_ADAPTER_UNKNOWN,
    TITLE_ADAPTER_PROFILE, TITLE_ADAPTER_CAPABILITIES, TITLE_ADAPTER_BUILD,
    TITLE_ADAPTER_LAYOUT, TITLE_ADAPTER_ALREADY_SELECTED, TITLE_ADAPTER_AOT_UNSUPPORTED
};

typedef struct TitleAdapterFailure {
    enum TitleAdapterStatus status;
    uint32_t module_index;
    ModuleAbiFailure abi;
    SessionLayoutFailure layout;
    NativeManifestFailure native;
} TitleAdapterFailure;

/* Generic adapter plus the storage its string fields point to. The embedded
 * TitleAdapter refers into this object: do not copy it by value. NPDM values
 * are recorded for diagnostics; startup still reads main.npdm itself. */
#define TITLE_GENERIC_VERSION_MAX 16u
typedef struct TitleGenericAdapter {
    TitleAdapter adapter;
    char version[TITLE_GENERIC_VERSION_MAX + 1];
    char save_namespace[64];
    char npdm_name[17];
    uint8_t main_thread_priority, main_thread_core, address_space;
    uint32_t main_thread_stack_size, system_resource_size;
    uint32_t rtld_present;
} TitleGenericAdapter;

/* Known IDs return static tables. TITLE_GENERIC returns NULL until
 * title_adapter_install_generic() succeeded in this process. */
TITLE_ADAPTER_API const TitleAdapter *title_adapter_find(enum TitleId id);

/* Publish a derived generic adapter once per process, before any selection.
 * Rejects another install, a selected title, or anything but a well-formed
 * TITLE_GENERIC adapter. The adapter is copied; string fields are rebound. */
TITLE_ADAPTER_API enum TitleAdapterStatus title_adapter_install_generic(const TitleGenericAdapter *generic);

/* Pure preflight, no process selection, reservation, file access or guest call.
 * Independent profile must come from verified producer/original-file metadata,
 * not a loaded descriptor. Module sizes/text/counts/full digests are never
 * invented by the adapter. Unknown builds and nonbaseline capability masks are
 * rejected. Output is cleared on failure; input/output must not overlap. */
TITLE_ADAPTER_API enum TitleAdapterStatus title_adapter_build_layout(enum TitleId id,
    const ModuleAbiProfile *profile, uint32_t artifact_capabilities, uint32_t page_size,
    SessionLayoutSpec *out, TitleAdapterFailure *failure);

/* Original-image preflight: independently pinned native runtime contract,
 * CPU3, title/program/build identities and declared load-order/ranges. Native
 * manifests have no generated functions or AOT capability/artifact digests. */
TITLE_ADAPTER_API enum TitleAdapterStatus title_adapter_build_native_layout(enum TitleId id,
    const NativeManifest *manifest, const uint8_t expected_contract_sha256[32],
    uint32_t page_size, SessionLayoutSpec *out, TitleAdapterFailure *failure);
/* Same preflight against an explicit adapter, e.g. a derived generic adapter
 * that has not been installed (preparation runs in the launcher process). */
TITLE_ADAPTER_API enum TitleAdapterStatus title_adapter_build_native_layout_for(const TitleAdapter *adapter,
    const NativeManifest *manifest, const uint8_t expected_contract_sha256[32],
    uint32_t page_size, SessionLayoutSpec *out, TitleAdapterFailure *failure);

/* Select immutable static metadata once, during setup before any guest threads.
 * Validate the complete profile/layout first. Does not activate SessionLayout,
 * reserve memory or bind dispatch. A second selection, including the same title,
 * requires a fresh process. Invalid IDs do not consume the selection. */
TITLE_ADAPTER_API enum TitleAdapterStatus title_adapter_select(enum TitleId id);
TITLE_ADAPTER_API const TitleAdapter *title_adapter_selected(void);
TITLE_ADAPTER_API const char *title_adapter_status_string(enum TitleAdapterStatus status);

#ifdef __cplusplus
}
#endif
#endif
