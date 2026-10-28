#include "module_loader.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>

enum ProcessState { PROCESS_EMPTY, PROCESS_OPENING, PROCESS_READY, PROCESS_REJECTED };
static pthread_mutex_t process_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    enum ProcessState state;
    void *handle; /* owned forever: no explicit close, including after rejection */
} process;

static enum ModuleLoaderStatus report(ModuleLoaderFailure *failure,
                                     enum ModuleLoaderStatus status, const char *diagnostic) {
    if (failure) {
        failure->status = status;
        snprintf(failure->diagnostic, sizeof failure->diagnostic, "%s",
                 diagnostic ? diagnostic : module_loader_status_string(status));
    }
    return status;
}

static void finish_attempt(void *handle, int accepted) {
    pthread_mutex_lock(&process_lock);
    process.handle = handle;
    process.state = accepted ? PROCESS_READY : PROCESS_REJECTED;
    pthread_mutex_unlock(&process_lock);
}

enum ModuleLoaderStatus module_loader_open(const char *path, const ModuleAbiProfile *expected,
                                          const ModuleAbiDescriptor **validated,
                                          ModuleLoaderFailure *failure) {
    if (validated) *validated = NULL;
    if (failure) {
        failure->status = MODULE_LOADER_OK;
        failure->abi = (ModuleAbiFailure){MODULE_ABI_OK, MODULE_ABI_NO_MODULE, MODULE_ABI_NO_FUNCTION};
        failure->diagnostic[0] = 0;
    }
    if (!path || !*path || !expected || !validated)
        return report(failure, MODULE_LOADER_INVALID_ARGUMENT, NULL);

    /* Do not hold this mutex across dlopen: a host constructor can reenter us. */
    pthread_mutex_lock(&process_lock);
    enum ProcessState state = process.state;
    if (state == PROCESS_EMPTY) process.state = PROCESS_OPENING;
    pthread_mutex_unlock(&process_lock);
    if (state == PROCESS_OPENING) return report(failure, MODULE_LOADER_IN_PROGRESS, NULL);
    if (state == PROCESS_READY) return report(failure, MODULE_LOADER_ALREADY_LOADED, NULL);
    if (state == PROCESS_REJECTED) return report(failure, MODULE_LOADER_FRESH_PROCESS_REQUIRED, NULL);

    dlerror();
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        /* Copy the thread-local loader error before any other loader operation. */
        enum ModuleLoaderStatus status = report(failure, MODULE_LOADER_OPEN_FAILED, dlerror());
        finish_attempt(NULL, 0);
        return status;
    }

    dlerror();
    const ModuleAbiDescriptor *descriptor = dlsym(handle, MODULE_LOADER_DESCRIPTOR_SYMBOL);
    const char *lookup_error = dlerror();
    if (lookup_error || !descriptor) {
        enum ModuleLoaderStatus status = report(failure, MODULE_LOADER_SYMBOL_FAILED, lookup_error);
        finish_attempt(handle, 0);
        return status;
    }

    ModuleAbiFailure validation;
    enum ModuleAbiStatus abi_status = module_abi_validate(descriptor, expected, &validation);
    if (abi_status != MODULE_ABI_OK) {
        if (failure) failure->abi = validation;
        enum ModuleLoaderStatus status = report(failure, MODULE_LOADER_ABI_REJECTED,
                                                module_abi_status_string(abi_status));
        finish_attempt(handle, 0);
        return status;
    }

    finish_attempt(handle, 1);
    *validated = descriptor;
    return report(failure, MODULE_LOADER_OK, NULL);
}

const char *module_loader_status_string(enum ModuleLoaderStatus status) {
    switch (status) {
    case MODULE_LOADER_OK: return "ok";
    case MODULE_LOADER_INVALID_ARGUMENT: return "invalid loader argument";
    case MODULE_LOADER_IN_PROGRESS: return "a module open is already in progress";
    case MODULE_LOADER_ALREADY_LOADED: return "a title is already loaded; start a fresh process";
    case MODULE_LOADER_FRESH_PROCESS_REQUIRED: return "a prior load attempt consumed this process";
    case MODULE_LOADER_OPEN_FAILED: return "dynamic library resolution failed";
    case MODULE_LOADER_SYMBOL_FAILED: return "descriptor data symbol lookup failed";
    case MODULE_LOADER_ABI_REJECTED: return "module descriptor validation failed";
    default: return "unknown loader status";
    }
}
