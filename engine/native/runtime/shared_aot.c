#include "shared_aot.h"

#include <pthread.h>
#include <string.h>

static pthread_mutex_t open_lock = PTHREAD_MUTEX_INITIALIZER;
static int session_state; /* 0 empty, 1 setup/host constructors, 2 consumed */

static enum SharedAotStatus report(SharedAotFailure *failure, enum SharedAotStatus status) {
    if (failure) failure->status = status;
    return status;
}

static int profile_matches_layout(const ModuleAbiProfile *profile,
    const SessionLayoutSpec *spec, const SessionLayout *layout) {
    if (profile->struct_size != sizeof *profile ||
        profile->identity.struct_size != sizeof profile->identity ||
        profile->identity.abi_version != MODULE_ABI_VERSION ||
        profile->identity.target != MODULE_ABI_TARGET_ANDROID_ARM64_LP64 ||
        profile->identity.cpu_identity != MODULE_ABI_CPU_A64_SHARED_66560 ||
        profile->identity.cpu_size != sizeof(Cpu) ||
        profile->identity.cpu_alignment != _Alignof(Cpu) ||
        profile->identity.guest_shift != spec->guest_shift ||
        profile->identity.guest_window_lo != layout->window.lo ||
        profile->identity.guest_window_hi != layout->window.hi ||
        (profile->module_count != 2 && profile->module_count != 4) ||
        profile->module_count != spec->module_count || !profile->modules) return 0;
    for (uint32_t i = 0; i < profile->module_count; ++i) {
        const ModuleAbiModuleSpec *module = &profile->modules[i];
        if (module->struct_size != sizeof *module ||
            module->module_id != spec->modules[i].module_id ||
            module->canonical_base != spec->modules[i].image.canonical_base ||
            module->mapped_size != spec->modules[i].image.size) return 0;
    }
    return 1;
}

enum SharedAotStatus shared_aot_open_with_reservation(const SharedAotRequest *request,
    SharedAotReserve reserve, void *context,
    const ModuleAbiDescriptor **validated, SharedAotFailure *failure) {
    if (validated) *validated = NULL;
    if (failure) {
        memset(failure, 0, sizeof *failure);
        failure->layout.first_index = failure->layout.second_index = SESSION_LAYOUT_NO_INDEX;
        failure->abi.module_index = failure->loader.abi.module_index = MODULE_ABI_NO_MODULE;
        failure->abi.function_index = failure->loader.abi.function_index = MODULE_ABI_NO_FUNCTION;
    }
    if (!request || !validated || request->struct_size != sizeof *request ||
        request->version != SHARED_AOT_REQUEST_VERSION ||
        !request->library_path || !*request->library_path || !request->expected || !request->layout)
        return report(failure, SHARED_AOT_ARGUMENT);
    if ((request->artifact_capabilities & ~AOT_CAP_ALL) ||
        (request->required_capabilities & ~AOT_CAP_ALL) ||
        request->artifact_capabilities != request->required_capabilities)
        return report(failure, SHARED_AOT_PROFILE);

    SessionLayout layout;
    SessionLayoutFailure layout_failure;
    if (session_layout_build(request->layout, &layout, &layout_failure) != SESSION_LAYOUT_OK) {
        if (failure) failure->layout = layout_failure;
        return report(failure, SHARED_AOT_LAYOUT);
    }
    ModuleAbiFailure profile_failure;
    if (module_abi_validate_profile(request->expected, &profile_failure) != MODULE_ABI_OK) {
        if (failure) failure->abi = profile_failure;
        return report(failure, SHARED_AOT_PROFILE);
    }
    if (!profile_matches_layout(request->expected, request->layout, &layout))
        return report(failure, SHARED_AOT_PROFILE);

    pthread_mutex_lock(&open_lock);
    int previous = session_state;
    if (!previous) session_state = 1;
    pthread_mutex_unlock(&open_lock);
    if (previous == 1) return report(failure, SHARED_AOT_IN_PROGRESS);
    if (previous == 2) return report(failure, SHARED_AOT_FRESH_PROCESS_REQUIRED);

    enum SharedAotStatus status = SHARED_AOT_OK;
    const ModuleAbiDescriptor *descriptor = NULL;
    ModuleLoaderFailure loader_failure;
    ModuleAbiFailure abi_failure = {MODULE_ABI_OK, MODULE_ABI_NO_MODULE, MODULE_ABI_NO_FUNCTION};
    if (session_layout_activate(request->layout, &layout_failure) != SESSION_LAYOUT_OK) {
        if (failure) failure->layout = layout_failure;
        status = SHARED_AOT_LAYOUT;
    } else if (reserve && !reserve(session_layout_current(), context)) {
        status = SHARED_AOT_RESERVATION;
    } else if (module_loader_open(request->library_path, request->expected,
                                  &descriptor, &loader_failure) != MODULE_LOADER_OK) {
        if (failure) failure->loader = loader_failure;
        status = SHARED_AOT_LOAD;
    } else {
        enum AotStatus dispatch = aot_dispatch_bind(descriptor, request->expected,
            request->artifact_capabilities, request->required_capabilities, &abi_failure);
        if (failure) { failure->dispatch = dispatch; failure->abi = abi_failure; }
        if (dispatch != AOT_OK) status = SHARED_AOT_DISPATCH;
    }
    pthread_mutex_lock(&open_lock);
    session_state = 2;
    pthread_mutex_unlock(&open_lock);
    if (status == SHARED_AOT_OK) *validated = descriptor;
    return report(failure, status);
}

enum SharedAotStatus shared_aot_open(const SharedAotRequest *request,
    const ModuleAbiDescriptor **validated, SharedAotFailure *failure) {
    return shared_aot_open_with_reservation(request, NULL, NULL, validated, failure);
}

const char *shared_aot_status_string(enum SharedAotStatus status) {
    switch (status) {
    case SHARED_AOT_OK: return "ok";
    case SHARED_AOT_ARGUMENT: return "invalid shared-AOT request";
    case SHARED_AOT_PROFILE: return "execution profile mismatch";
    case SHARED_AOT_LAYOUT: return "session layout rejected";
    case SHARED_AOT_LOAD: return "title metadata load rejected";
    case SHARED_AOT_DISPATCH: return "dispatch registry rejected";
    case SHARED_AOT_RESERVATION: return "guest address reservation failed";
    case SHARED_AOT_IN_PROGRESS: return "shared-AOT setup already in progress";
    case SHARED_AOT_FRESH_PROCESS_REQUIRED: return "shared-AOT session consumed; start a fresh process";
    default: return "unknown shared-AOT status";
    }
}
