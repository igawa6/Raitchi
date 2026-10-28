#ifndef SHARED_RECOMP_AOT_SESSION_H
#define SHARED_RECOMP_AOT_SESSION_H

#include "aot_dispatch.h"
#include "session_layout.h"
#include "../module_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SHARED_AOT_REQUEST_VERSION 1u
typedef struct SharedAotRequest {
    uint32_t struct_size;
    uint32_t version;
    uint32_t artifact_capabilities;
    uint32_t required_capabilities;
    const char *library_path;
    const ModuleAbiProfile *expected;
    const SessionLayoutSpec *layout;
} SharedAotRequest;

enum SharedAotStatus {
    SHARED_AOT_OK, SHARED_AOT_ARGUMENT, SHARED_AOT_PROFILE,
    SHARED_AOT_LAYOUT, SHARED_AOT_LOAD, SHARED_AOT_DISPATCH,
    SHARED_AOT_IN_PROGRESS, SHARED_AOT_FRESH_PROCESS_REQUIRED,
    SHARED_AOT_RESERVATION
};
typedef struct SharedAotFailure {
    enum SharedAotStatus status;
    SessionLayoutFailure layout;
    ModuleLoaderFailure loader;
    ModuleAbiFailure abi;
    enum AotStatus dispatch;
} SharedAotFailure;

/* First executable seam, not a complete Switch runtime. Trusted producer/file
 * verification must supply expected layout, module hashes and code capabilities
 * independently; this API does not parse or verify a serialized sidecar/ELF.
 * Validates layout/profile agreement, selects immutable per-process addresses,
 * loads and validates descriptor DATA, and binds the generic dispatch registry.
 * No guest entry, OS services, NSO mapping/relocation or constructor/init policy
 * is implemented here. dlopen CAN run host constructors before validation.
 * One setup attempt after successful preflight consumes the session, including
 * load/bind failure; start a new application process to retry or switch titles. */
AOT_API enum SharedAotStatus shared_aot_open(const SharedAotRequest *request,
    const ModuleAbiDescriptor **validated, SharedAotFailure *failure);
/* Production integration: reserve the selected address window after immutable
 * activation but BEFORE dlopen. Callback must reserve safely without replacing
 * foreign mappings; zero rejects and consumes this process. It owns cleanup of
 * any partial reservation. The ordinary open API performs no memory reservation. */
typedef int (*SharedAotReserve)(const SessionLayout *, void *context);
AOT_API enum SharedAotStatus shared_aot_open_with_reservation(const SharedAotRequest *request,
    SharedAotReserve reserve, void *context,
    const ModuleAbiDescriptor **validated, SharedAotFailure *failure);
AOT_API const char *shared_aot_status_string(enum SharedAotStatus status);


#ifdef __cplusplus
}
#endif
#endif
