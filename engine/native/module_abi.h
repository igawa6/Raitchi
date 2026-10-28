/* Provisional metadata contract, independent of upstream GameLibTables. */
#ifndef SHARED_RECOMP_MODULE_ABI_H
#define SHARED_RECOMP_MODULE_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MODULE_ABI_VERSION 1u
#define MODULE_ABI_MAX_MODULES 4u
#define MODULE_ABI_NO_MODULE UINT32_MAX
#define MODULE_ABI_NO_FUNCTION UINT64_MAX

enum ModuleAbiTarget {
    MODULE_ABI_TARGET_ANDROID_ARM64_LP64 = 1
};

/* Each identity fixes ALL Cpu field offsets/semantics, not just sizeof(Cpu).
 * These provisional identities describe the audited prefix and cache layouts. */
enum ModuleAbiCpuIdentity {
    MODULE_ABI_CPU_A64_PREFIX_1024 = 1,
    MODULE_ABI_CPU_A64_CACHE_66560 = 2,
    /* shared-aot-v1: explicit excl_hi at336; original fields/cache offsets retained. */
    MODULE_ABI_CPU_A64_SHARED_66560 = 3
};

enum ModuleAbiModuleId {
    MODULE_ABI_MAIN = 1,
    MODULE_ABI_SDK = 2,
    MODULE_ABI_SUBSDK0 = 3,
    MODULE_ABI_SUBSDK1 = 4
};

typedef struct ModuleAbiIdentity {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t target;
    uint32_t cpu_identity;
    uint32_t cpu_size;
    uint32_t cpu_alignment;
    uint64_t guest_shift;
    uint64_t guest_window_lo; /* actual mapped addresses; half-open window */
    uint64_t guest_window_hi;
} ModuleAbiIdentity;

typedef struct ModuleAbiModuleSpec {
    uint32_t struct_size;
    uint32_t module_id;
    uint8_t nso_build_id[32]; /* full header bytes, including trailing zeroes */
    uint8_t nso_sha256[32];   /* complete original NSO file */
    uint64_t canonical_base;
    uint64_t mapped_size;    /* includes BSS and mapping padding */
    uint64_t text_offset;
    uint64_t text_size;
    uint64_t function_count;
} ModuleAbiModuleSpec;

struct Cpu;
typedef void (*ModuleAbiGuestFn)(struct Cpu *);

typedef struct ModuleAbiModule {
    uint32_t struct_size;
    uint32_t reserved;
    ModuleAbiModuleSpec image;
    uint64_t address_count;
    const uint64_t *addresses; /* actual shifted guest addresses, strictly sorted */
    const ModuleAbiGuestFn *functions; /* image.function_count entries */
} ModuleAbiModule;

typedef struct ModuleAbiDescriptor {
    uint32_t struct_size;
    uint32_t module_count;
    ModuleAbiIdentity identity;
    const ModuleAbiModule *modules;
} ModuleAbiDescriptor;

/* Runtime-owned expectations, derived independently of the loaded descriptor. */
typedef struct ModuleAbiProfile {
    uint32_t struct_size;
    uint32_t module_count;
    ModuleAbiIdentity identity;
    const ModuleAbiModuleSpec *modules;
} ModuleAbiProfile;

enum ModuleAbiStatus {
    MODULE_ABI_OK = 0,
    MODULE_ABI_NULL,
    MODULE_ABI_SIZE,
    MODULE_ABI_VERSION_MISMATCH,
    MODULE_ABI_TARGET_MISMATCH,
    MODULE_ABI_CPU_MISMATCH,
    MODULE_ABI_PROFILE_MISMATCH,
    MODULE_ABI_LAYOUT,
    MODULE_ABI_METADATA_MISMATCH,
    MODULE_ABI_TABLE,
    MODULE_ABI_FUNCTION
};

typedef struct ModuleAbiFailure {
    enum ModuleAbiStatus status;
    uint32_t module_index;
    uint64_t function_index;
} ModuleAbiFailure;

/* Pure validation of runtime-owned expectations BEFORE layout selection/dlopen.
 * Checks identity, the current two/four-module order, complete nonblank digests,
 * mapped/text ranges, declared function-table capacity and module overlaps.
 * No descriptor, generated addresses, function pointers or guest memory needed.
 * Profile and its declared spec array must already be readable and immutable. */
__attribute__((visibility("default")))
enum ModuleAbiStatus module_abi_validate_profile(const ModuleAbiProfile *profile,
                                               ModuleAbiFailure *failure);

/* Read-only; no allocations, guest calls, symbol lookup or module loading.
 * All supplied pointers must already refer to readable arrays of declared length.
 * Descriptor metadata is checked AFTER dlopen and BEFORE any guest function entry.
 * This is not a hostile-library verifier or a serialized/on-disk representation. */
enum ModuleAbiStatus module_abi_validate(const ModuleAbiDescriptor *descriptor,
                                       const ModuleAbiProfile *expected,
                                       ModuleAbiFailure *failure);
const char *module_abi_status_string(enum ModuleAbiStatus status);

#ifdef __cplusplus
}
#endif
#endif
