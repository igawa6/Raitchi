/* Shared descriptor/sidecar entry; legacy LAS UI declarations are reference-only. */
#define _GNU_SOURCE
#include "gamelib.h"
#include "rt.h"
#include "runtime/shared_aot.h"
#include "shared_contract.h"
#include "adapters/generic/generic.h"
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static TitleManifest manifest;
static NativeManifest native_manifest;
static bool native_ready, session_attempted;
static const ModuleAbiDescriptor *descriptor;
static GameLibTables legacy_las_view;

bool shared_title_las(void) {
    const TitleAdapter *adapter = title_adapter_selected();
    return adapter && adapter->id == TITLE_LAS;
}
bool shared_renderer_legacy_target_policy(void) {
    const TitleAdapter *adapter = title_adapter_selected();
    return adapter && (adapter->renderer_policy_flags & TITLE_RENDERER_LEGACY_TARGET_HEURISTICS) != 0;
}
const TitleManifest *shared_manifest_current(void) { return descriptor ? &manifest : NULL; }
const ModuleAbiDescriptor *shared_gamelib_descriptor(void) { return descriptor; }
const NativeManifest *shared_native_manifest_current(void) { return native_ready ? &native_manifest : NULL; }
bool shared_session_ready(void) { return descriptor || native_ready; }
uint32_t shared_module_id(uint32_t index) {
    if (native_ready && index < native_manifest.module_count) return native_manifest.modules[index].module_id;
    if (descriptor && index < manifest.profile.module_count) return manifest.modules[index].module_id;
    return MODULE_ABI_NO_MODULE;
}
static int fail(char *why, size_t capacity, const char *reason) {
    if (why && capacity) snprintf(why, capacity, "%s", reason);
    return 0;
}
static int fail_format(char *why, size_t capacity, const char *format, ...) {
    if (why && capacity) {
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(why, capacity, format, arguments);
        va_end(arguments);
    }
    return 0;
}
static void decode_contract(const char *hex, uint8_t out[32]) {
    for (unsigned i = 0; i < 32; ++i) {
        unsigned value;
        if (sscanf(hex + 2 * i, "%2x", &value) != 1) abort();
        out[i] = (uint8_t)value;
    }
}
static int reserve_window(const SessionLayout *layout, void *context) {
    (void)context;
    if (!plat_reserve_guest(layout->window.lo, layout->window.hi)) return 0;
    return 1;
}
bool shared_session_prepare(const char *game_root, const char *library,
                            const char *manifest_path, char *why, size_t capacity) {
    if (shared_session_ready() || session_attempted) return fail(why, capacity, "session already loaded; use a fresh process");
    uint8_t wire[TITLE_MANIFEST_MAX_SIZE + 1], expected_contract[32], digest[32];
    FILE *file = fopen(manifest_path, "rb");
    if (!file) return fail(why, capacity, "shared game manifest is missing");
    size_t length = fread(wire, 1, sizeof wire, file);
    int io_error = ferror(file);
    fclose(file);
    decode_contract(SHARED_AOT_CONTRACT_SHA256_HEX, expected_contract);
    ModuleAbiFailure abi;
    if (io_error || title_manifest_parse(wire, length, expected_contract, &manifest, &abi) != TITLE_MANIFEST_OK)
        return fail(why, capacity, "shared game manifest contract/profile rejected");
    const TitleAdapter *adapter = title_adapter_find((enum TitleId)manifest.title_id);
    const char *requested = getenv("LARECOMP_TITLE");
    if (!adapter || (requested && strcmp(requested, adapter->key)))
        return fail(why, capacity, "selected title and manifest differ");
    long page = sysconf(_SC_PAGESIZE);
    SessionLayoutSpec layout;
    TitleAdapterFailure adapter_failure;
    if (page < 4096 || page > UINT32_MAX ||
        title_adapter_build_layout(adapter->id, &manifest.profile, manifest.capabilities,
                                   (uint32_t)page, &layout, &adapter_failure) != TITLE_ADAPTER_OK)
        return fail(why, capacity, "title policy/layout rejected");
    if (!shared_sha256_file(library, digest) || memcmp(digest, manifest.library_sha256, 32))
        return fail(why, capacity, "title library SHA-256 differs from its producer manifest");
    for (uint32_t i = 0; i < adapter->module_count; ++i) {
        char path[4096];
        int n = snprintf(path, sizeof path, "%s/exefs/%s", game_root, adapter->modules[i].name);
        if (n < 0 || (size_t)n >= sizeof path || !shared_sha256_file(path, digest) ||
            memcmp(digest, manifest.modules[i].nso_sha256, 32))
            return fail(why, capacity, "original NSO SHA-256 differs from its producer manifest");
    }
    session_attempted = true;
    if (title_adapter_select(adapter->id) != TITLE_ADAPTER_OK)
        return fail(why, capacity, "title already selected; use a fresh process");
    SharedAotRequest request = {.struct_size = sizeof request, .version = SHARED_AOT_REQUEST_VERSION,
        .artifact_capabilities = manifest.capabilities, .required_capabilities = adapter->capabilities,
        .library_path = library, .expected = &manifest.profile, .layout = &layout};
    SharedAotFailure failure;
    enum SharedAotStatus status = shared_aot_open_with_reservation(&request, reserve_window, NULL,
                                                                 &descriptor, &failure);
    if (status != SHARED_AOT_OK) return fail(why, capacity, shared_aot_status_string(status));
    /* dlsym must resolve the title object's own descriptor, not a dependency's. */
    Dl_info info;
    struct stat expected_file, actual_file;
    if (!dladdr(descriptor, &info) || !info.dli_fname || stat(library, &expected_file) ||
        stat(info.dli_fname, &actual_file) || expected_file.st_dev != actual_file.st_dev ||
        expected_file.st_ino != actual_file.st_ino)
        FATAL("descriptor is not owned by the verified title library\n");
    /* Bind the same actual loaded file bytes to the preflight hash as well. */
    if (!shared_sha256_file(info.dli_fname, digest) || memcmp(digest, manifest.library_sha256, 32))
        FATAL("title library changed during loading\n");
    if (adapter->id == TITLE_LAS) {
        const ModuleAbiModule *main = &descriptor->modules[0], *sdk = &descriptor->modules[1];
        legacy_las_view = (GameLibTables){.abi = GAMELIB_ABI, .id = "shared-aot-v1",
            .main_addrs = main->addresses, .main_fns = main->functions, .main_count = &main->address_count,
            .sdk_addrs = sdk->addresses, .sdk_fns = sdk->functions, .sdk_count = &sdk->address_count};
    }
    LOG("[shared] selected %s, common CPU3, %u modules; library/NSO hashes verified\n",
        adapter->key, adapter->module_count);
    return true;
}

/* Generic fallback: derive the adapter from this prepared ExeFS, bind it to the
 * launcher's program/version and publish it once before selection. */
static bool generic_session_adapter(const char *root, char *why, size_t capacity) {
    const char *program = getenv("LARECOMP_GENERIC_PROGRAM_ID"), *version = getenv("LARECOMP_GENERIC_VERSION");
    const char *nvn = getenv("LARECOMP_GENERIC_NVN_API");
    char *end = NULL, exefs[4096], detail[192];
    errno = 0;
    unsigned long long expected = program && *program ? strtoull(program, &end, 16) : 0;
    if (!expected || errno || !end || *end || strlen(program) != 16 || expected != native_manifest.program_id)
        return fail(why, capacity, "generic session program ID differs from the prepared game");
    TitleGenericOptions options = {.expected_program_id = expected, .display_version = version};
    if (nvn && *nvn && !title_generic_parse_nvn_api(nvn, &options.nvn_api_major, &options.nvn_api_minor))
        return fail(why, capacity, "LARECOMP_GENERIC_NVN_API must be MAJOR.MINOR");
    int n = snprintf(exefs, sizeof exefs, "%s/exefs", root);
    static TitleGenericAdapter generic;
    if (n < 0 || (size_t)n >= sizeof exefs || !title_generic_derive_directory(exefs, &options, &generic, detail, sizeof detail)) {
        if (why && capacity) snprintf(why, capacity, "generic adapter rejected this game: %s", n < 0 || (size_t)n >= sizeof exefs ? "path too long" : detail);
        return false;
    }
    char token[TITLE_GENERIC_VERSION_MAX + 1];
    title_generic_version_token(version, token);
    if (!version || strcmp(token, version))
        return fail(why, capacity, "generic session version differs from the prepared game");
    if (title_adapter_install_generic(&generic) != TITLE_ADAPTER_OK)
        return fail(why, capacity, "generic adapter already installed; use a fresh process");
    return true;
}

static bool native_session_prepare(const char *root, const char *path, char *why, size_t capacity) {
    if (session_attempted || shared_session_ready()) return fail(why, capacity, "native setup requires a fresh process");
    const char *backend = getenv("LARECOMP_CPU_BACKEND");
    if (!backend || strcmp(backend, "continuous-nce"))
        return fail(why, capacity, "native-only metadata requires continuous NCE");
    uint8_t wire[NATIVE_MANIFEST_MAX_SIZE + 1], expected[32], digest[32];
    FILE *file = fopen(path, "rb");
    if (!file) return fail(why, capacity, "native manifest is missing");
    size_t length = fread(wire, 1, sizeof wire, file);
    int io_error = ferror(file); fclose(file);
    decode_contract(SHARED_NATIVE_CONTRACT_SHA256_HEX, expected);
    NativeManifestFailure detail;
    if (io_error || native_manifest_parse(wire, length, expected, &native_manifest, &detail) != NATIVE_MANIFEST_OK)
        return fail(why, capacity, "native manifest contract/image metadata rejected");
    if (native_manifest.title_id == TITLE_GENERIC && !generic_session_adapter(root, why, capacity)) return false;
    const TitleAdapter *adapter = title_adapter_find((enum TitleId)native_manifest.title_id);
    const char *requested = getenv("LARECOMP_TITLE");
    if (!adapter || (requested && strcmp(requested, adapter->key)))
        return fail(why, capacity, "selected title and native manifest differ");
    long page = sysconf(_SC_PAGESIZE);
    SessionLayoutSpec layout;
    TitleAdapterFailure policy;
    if (page < 4096 || page > UINT32_MAX ||
        title_adapter_build_native_layout(adapter->id, &native_manifest, expected,
            (uint32_t)page, &layout, &policy) != TITLE_ADAPTER_OK)
        return fail(why, capacity, "native title policy/layout rejected");
    for (uint32_t i = 0; i < adapter->module_count; ++i) {
        char original[4096];
        int n = snprintf(original, sizeof original, "%s/exefs/%s", root, adapter->modules[i].name);
        if (n < 0 || (size_t)n >= sizeof original || !shared_sha256_file(original, digest) ||
            memcmp(digest, native_manifest.modules[i].nso_sha256, sizeof digest))
            return fail(why, capacity, "original NSO differs from native manifest");
    }
    session_attempted = true; /* selection/reservation failure consumes this process */
    enum TitleAdapterStatus selected = title_adapter_select(adapter->id);
    if (selected != TITLE_ADAPTER_OK)
        return fail_format(why, capacity, "native title selection failed for %s: %s",
                           adapter->key, title_adapter_status_string(selected));
    SessionLayoutFailure layout_failure;
    enum SessionLayoutStatus activated = session_layout_activate(&layout, &layout_failure);
    if (activated != SESSION_LAYOUT_OK)
        return fail_format(why, capacity, "native layout activation failed for %s: %s",
                           adapter->key, session_layout_status_string(activated));
    const SessionLayout *active = session_layout_current();
    errno = 0;
    if (!reserve_window(active, NULL)) {
        const int error = errno;
        return fail_format(why, capacity,
            "native guest reservation failed for %s: [0x%llx,0x%llx), errno %d (%s)",
            adapter->key, (unsigned long long)active->window.lo,
            (unsigned long long)active->window.hi, error,
            error ? strerror(error) : "exact address unavailable");
    }
    if (aot_dispatch_bind_native(&native_manifest, expected, adapter->capabilities, &detail) != AOT_OK)
        return fail(why, capacity, "native text registry rejected");
    native_ready = true;
    LOG("[shared] selected %s, native-only CPU3, %u modules; original NSO hashes verified; no game DSO loaded\n",
        adapter->key, adapter->module_count);
    if (adapter->id == TITLE_GENERIC) {
        char line[1024];
        title_generic_describe((const TitleGenericAdapter *)adapter, line, sizeof line);
        LOG("%s\n", line);
    }
    return true;
}

bool gamelib_load(const char *root, char *why, size_t capacity) {
    const char *native_path = getenv("LARECOMP_NATIVE_MANIFEST");
    if (native_path && *native_path) return native_session_prepare(root, native_path, why, capacity);
    char library[4096], sidecar[4096];
    const char *selected = getenv("LARECOMP_GAME_LIB");
    int n = selected ? snprintf(library, sizeof library, "%s", selected)
                     : snprintf(library, sizeof library, "%s/liblasgame.so", root);
    if (n < 0 || (size_t)n >= sizeof library) return fail(why, capacity, "library path too long");
    selected = getenv("LARECOMP_SHARED_MANIFEST");
    n = selected ? snprintf(sidecar, sizeof sidecar, "%s", selected)
                 : snprintf(sidecar, sizeof sidecar, "%s.saotm", library);
    if (n < 0 || (size_t)n >= sizeof sidecar) return fail(why, capacity, "manifest path too long");
    return shared_session_prepare(root, library, sidecar, why, capacity);
}
const GameLibTables *gamelib_tables(void) { return shared_title_las() && descriptor ? &legacy_las_view : NULL; }
bool gamelib_present(const char *root) {
    char path[4096];
    int n = snprintf(path, sizeof path, "%s/liblasgame.so.saotm", root);
    return n >= 0 && (size_t)n < sizeof path && access(path, R_OK) == 0;
}
bool gamelib_pgo_dump(const char *path) {
    (void)path;
    /* Initial common producer does not enable PGO. No legacy getter is invoked. */
    return false;
}
