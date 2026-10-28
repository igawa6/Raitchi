/* Run each CASE in a fresh process: module_loader_test FIXTURE_DIR CASE.
 * Link against the shared gate and use -rdynamic to expose fixture sentinels.
 * Synthetic ARM64 metadata here exercises the contract on the host; these tiny
 * host-compiled callbacks are not an Android execution or architecture proof. */
#include "module_loader.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned events[5];
static unsigned checks;
static const char *reentry_path;
static const ModuleAbiProfile *reentry_profile;
static enum ModuleLoaderStatus reentry_status;
static pthread_mutex_t constructor_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t constructor_condition = PTHREAD_COND_INITIALIZER;
static int block_constructor, constructor_entered, release_constructor;

#define CHECK(condition) do { ++checks; if (!(condition)) { \
    fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)

MODULE_LOADER_API void module_loader_test_event(unsigned event) {
    if (event < 5) ++events[event];
    if (event == 1 && reentry_path) {
        const ModuleAbiDescriptor *descriptor = NULL;
        ModuleLoaderFailure failure;
        reentry_status = module_loader_open(reentry_path, reentry_profile, &descriptor, &failure);
        CHECK(reentry_status == MODULE_LOADER_IN_PROGRESS && descriptor == NULL);
    }
    if (event == 1 && block_constructor) {
        pthread_mutex_lock(&constructor_lock);
        constructor_entered = 1;
        pthread_cond_broadcast(&constructor_condition);
        while (!release_constructor) pthread_cond_wait(&constructor_condition, &constructor_lock);
        pthread_mutex_unlock(&constructor_lock);
    }
}

/* Construct expectations independently; never read fixture metadata to create them. */
static ModuleAbiProfile profile(ModuleAbiModuleSpec specs[4], uint32_t count) {
    memset(specs, 0, 4 * sizeof *specs);
    for (uint32_t i = 0; i < 4; ++i) {
        specs[i].struct_size = sizeof specs[i];
        specs[i].module_id = i + MODULE_ABI_MAIN;
        specs[i].nso_build_id[0] = (uint8_t)(i + 1);
        specs[i].nso_build_id[31] = (uint8_t)(0xa1 + i);
        specs[i].nso_sha256[0] = (uint8_t)(11 + i);
        specs[i].nso_sha256[31] = (uint8_t)(0xb1 + i);
        specs[i].canonical_base = UINT64_C(0x7000000000) + (uint64_t)i * 0x10000;
        specs[i].mapped_size = 0x4000;
        specs[i].text_offset = 0x100;
        specs[i].text_size = 0x100;
        specs[i].function_count = 2;
    }
    return (ModuleAbiProfile){.struct_size = sizeof(ModuleAbiProfile), .module_count = count,
        .identity = {.struct_size = sizeof(ModuleAbiIdentity), .abi_version = MODULE_ABI_VERSION,
            .target = MODULE_ABI_TARGET_ANDROID_ARM64_LP64, .cpu_identity = MODULE_ABI_CPU_A64_CACHE_66560,
            .cpu_size = 66560, .cpu_alignment = 16, .guest_shift = UINT64_C(0x6000000000),
            .guest_window_lo = UINT64_C(0x1000000000), .guest_window_hi = UINT64_C(0x1000100000)},
        .modules = specs};
}

static void fixture_path(char out[4096], const char *directory, const char *name) {
    int length = snprintf(out, 4096, "%s/%s.so", directory, name);
    CHECK(length > 0 && length < 4096);
}

static void check_status(enum ModuleLoaderStatus actual, enum ModuleLoaderStatus wanted,
                         const ModuleLoaderFailure *failure, const ModuleAbiDescriptor *descriptor) {
    if (actual != wanted)
        fprintf(stderr, "wanted %s, got %s: %s\n", module_loader_status_string(wanted),
                module_loader_status_string(actual), failure->diagnostic);
    CHECK(actual == wanted && failure->status == wanted);
    CHECK(failure->diagnostic[0] != 0);
    CHECK((descriptor != NULL) == (wanted == MODULE_LOADER_OK));
}

typedef struct Worker {
    const char *path;
    const ModuleAbiProfile *expected;
    const ModuleAbiDescriptor *descriptor;
    ModuleLoaderFailure failure;
    enum ModuleLoaderStatus status;
} Worker;

static void *worker_open(void *argument) {
    Worker *w = argument;
    w->status = module_loader_open(w->path, w->expected, &w->descriptor, &w->failure);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s FIXTURE_DIR CASE\n", argv[0]); return 2; }
    const char *name = argv[2];
    ModuleAbiModuleSpec specs[4];
    ModuleAbiProfile expected = profile(specs, !strcmp(name, "good4") ? 4 : 2);
    char path[4096], second_path[4096];
    fixture_path(path, argv[1], "good2");
    fixture_path(second_path, argv[1], "good4");
    const ModuleAbiDescriptor *descriptor = NULL;
    ModuleLoaderFailure failure;
    enum ModuleLoaderStatus status;

    if (!strcmp(name, "invalid_args")) {
        status = module_loader_open(NULL, &expected, &descriptor, &failure);
        check_status(status, MODULE_LOADER_INVALID_ARGUMENT, &failure, descriptor);
        status = module_loader_open("", &expected, &descriptor, &failure);
        check_status(status, MODULE_LOADER_INVALID_ARGUMENT, &failure, descriptor);
        status = module_loader_open(path, NULL, &descriptor, &failure);
        check_status(status, MODULE_LOADER_INVALID_ARGUMENT, &failure, descriptor);
        status = module_loader_open(path, &expected, NULL, &failure);
        check_status(status, MODULE_LOADER_INVALID_ARGUMENT, &failure, NULL);
        CHECK(events[1] == 0);
        status = module_loader_open(path, &expected, &descriptor, NULL);
        CHECK(status == MODULE_LOADER_OK && descriptor != NULL && events[1] == 1);
    } else if (!strcmp(name, "concurrent")) {
        Worker worker = {.path = path, .expected = &expected};
        block_constructor = 1;
        pthread_t thread;
        CHECK(pthread_create(&thread, NULL, worker_open, &worker) == 0);
        pthread_mutex_lock(&constructor_lock);
        while (!constructor_entered) pthread_cond_wait(&constructor_condition, &constructor_lock);
        pthread_mutex_unlock(&constructor_lock);
        status = module_loader_open(second_path, &expected, &descriptor, &failure);
        check_status(status, MODULE_LOADER_IN_PROGRESS, &failure, descriptor);
        pthread_mutex_lock(&constructor_lock);
        release_constructor = 1;
        pthread_cond_broadcast(&constructor_condition);
        pthread_mutex_unlock(&constructor_lock);
        CHECK(pthread_join(thread, NULL) == 0);
        check_status(worker.status, MODULE_LOADER_OK, &worker.failure, worker.descriptor);
        CHECK(events[1] == 1);
    } else {
        enum ModuleLoaderStatus wanted = MODULE_LOADER_OK;
        enum ModuleAbiStatus abi = MODULE_ABI_OK;
        uint32_t bad_module = MODULE_ABI_NO_MODULE;
        uint64_t bad_function = MODULE_ABI_NO_FUNCTION;
        if (!strcmp(name, "good4")) fixture_path(path, argv[1], "good4");
        else if (!strcmp(name, "missing")) { fixture_path(path, argv[1], "missing"); wanted = MODULE_LOADER_SYMBOL_FAILED; }
        else if (!strcmp(name, "bad_version") || !strcmp(name, "second_open_after_reject")) {
            fixture_path(path, argv[1], "bad_version"); wanted = MODULE_LOADER_ABI_REJECTED; abi = MODULE_ABI_VERSION_MISMATCH;
        } else if (!strcmp(name, "bad_hash")) {
            fixture_path(path, argv[1], "bad_hash"); wanted = MODULE_LOADER_ABI_REJECTED;
            abi = MODULE_ABI_METADATA_MISMATCH; bad_module = 0;
        } else if (!strcmp(name, "null_function")) {
            fixture_path(path, argv[1], "null_function"); wanted = MODULE_LOADER_ABI_REJECTED;
            abi = MODULE_ABI_FUNCTION; bad_module = 0; bad_function = 0;
        } else if (!strcmp(name, "profile_mismatch")) {
            specs[0].nso_sha256[31] ^= 1; wanted = MODULE_LOADER_ABI_REJECTED;
            abi = MODULE_ABI_METADATA_MISMATCH; bad_module = 0;
        } else if (!strcmp(name, "unresolved")) {
            fixture_path(path, argv[1], "unresolved"); wanted = MODULE_LOADER_OPEN_FAILED;
        } else if (!strcmp(name, "missing_file") || !strcmp(name, "second_open_after_open_failure")) {
            fixture_path(path, argv[1], "does_not_exist"); wanted = MODULE_LOADER_OPEN_FAILED;
        } else if (!strcmp(name, "reentrant")) {
            reentry_path = second_path; reentry_profile = &expected;
        } else if (strcmp(name, "good2") && strcmp(name, "second_open")) {
            fprintf(stderr, "unknown test case: %s\n", name); return 2;
        }
        status = module_loader_open(path, &expected, &descriptor, &failure);
        check_status(status, wanted, &failure, descriptor);
        CHECK(events[1] == (wanted == MODULE_LOADER_OPEN_FAILED ? 0u : 1u));
        CHECK(failure.abi.status == abi && failure.abi.module_index == bad_module && failure.abi.function_index == bad_function);
        if (!strcmp(name, "reentrant")) CHECK(reentry_status == MODULE_LOADER_IN_PROGRESS);
        if (!strcmp(name, "good2") || !strcmp(name, "good4")) {
            CHECK(descriptor->module_count == expected.module_count && events[2] == 0);
            /* The caller may explicitly enter only AFTER a successful gate. */
            descriptor->modules[0].functions[0](NULL);
            CHECK(events[2] == 1);
        }
        if (!strcmp(name, "second_open") || !strcmp(name, "second_open_after_reject") ||
            !strcmp(name, "second_open_after_open_failure")) {
            unsigned constructors = events[1];
            ModuleAbiModuleSpec new_specs[4];
            ModuleAbiProfile new_expected = profile(new_specs, 4);
            descriptor = NULL;
            status = module_loader_open(second_path, &new_expected, &descriptor, &failure);
            check_status(status, wanted == MODULE_LOADER_OK ? MODULE_LOADER_ALREADY_LOADED : MODULE_LOADER_FRESH_PROCESS_REQUIRED,
                         &failure, descriptor);
            CHECK(events[1] == constructors);
        }
    }
    CHECK(events[3] == 0); /* legacy descriptor getter was never invoked */
    CHECK(events[4] == 0); /* rejected/successful handles were never closed by the gate */
    if (strcmp(name, "good2") && strcmp(name, "good4")) CHECK(events[2] == 0);
    printf("module loader %s: %u checks passed\n", name, checks);
    return 0;
}
