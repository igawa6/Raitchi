/* Provisional POSIX loader plumbing; independent of current title ABIs. */
#ifndef SHARED_RECOMP_MODULE_LOADER_H
#define SHARED_RECOMP_MODULE_LOADER_H

#include "module_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MODULE_LOADER_DESCRIPTOR_SYMBOL "recomp_module_descriptor_v1"
#define MODULE_LOADER_API __attribute__((visibility("default")))
#define MODULE_LOADER_DIAGNOSTIC_SIZE 256

/* A title library exports this DATA object, not a getter or entry callback:
 *   MODULE_LOADER_API const ModuleAbiDescriptor recomp_module_descriptor_v1 = ...;
 * All referenced arrays must also be immutable/readable and remain alive.
 * This declaration does not require a definition in the runtime itself. */
extern const ModuleAbiDescriptor recomp_module_descriptor_v1;

enum ModuleLoaderStatus {
    MODULE_LOADER_OK = 0,
    MODULE_LOADER_INVALID_ARGUMENT,
    MODULE_LOADER_IN_PROGRESS,
    MODULE_LOADER_ALREADY_LOADED,
    MODULE_LOADER_FRESH_PROCESS_REQUIRED,
    MODULE_LOADER_OPEN_FAILED,
    MODULE_LOADER_SYMBOL_FAILED,
    MODULE_LOADER_ABI_REJECTED
};

typedef struct ModuleLoaderFailure {
    enum ModuleLoaderStatus status;
    ModuleAbiFailure abi; /* meaningful for ABI_REJECTED; sentinel indices otherwise */
    char diagnostic[MODULE_LOADER_DIAGNOSTIC_SIZE]; /* copied, possibly truncated dlerror/status */
} ModuleLoaderFailure;

/* Runtime-owned expected metadata must be independent of the library descriptor.
 * On success only, *validated receives the checked data object. This function never
 * calls a descriptor function pointer. All failure paths clear *validated.
 *
 * Process policy lives inside this runtime artifact, not a caller-owned loader:
 * invalid null/empty arguments do not consume it; the FIRST ACTUAL dlopen attempt
 * consumes the process even if opening, symbol lookup or validation fails. Concurrent
 * or constructor-reentrant calls return IN_PROGRESS. After success, further opens
 * return ALREADY_LOADED; after rejection, FRESH_PROCESS_REQUIRED. No reset/unload API.
 * Any acquired dlopen handle remains resident until process exit, including rejected
 * libraries: this avoids invoking their destructors as a supposed clean retry.
 *
 * Trust boundary: dlopen already executes host constructors/resolvers; this is NOT
 * safe loading of hostile libraries. POSIX dlsym can find a dependency's symbol and
 * does not verify ELF OBJECT type, read-only mappings, pointer extents or ownership.
 * Those properties are a trusted producer/packaging contract here. Sidecar/file
 * checks, ELF symbol ownership/type checks and real runtime entry integration remain
 * separate work. dlclose of this runtime or loading another copy/namespace must not
 * be used to evade the process policy. Forking after an attempt is not a fresh session.
 * Android runs need a new application process, not reinitialization of this API. */
MODULE_LOADER_API enum ModuleLoaderStatus module_loader_open(
    const char *path, const ModuleAbiProfile *expected,
    const ModuleAbiDescriptor **validated, ModuleLoaderFailure *failure);
MODULE_LOADER_API const char *module_loader_status_string(enum ModuleLoaderStatus status);

#ifdef __cplusplus
}
#endif
#endif
