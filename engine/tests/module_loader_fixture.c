/* Tiny synthetic DSOs. Build seven copies with -DFIXTURE_KIND=0..6.
 * Metadata and addresses below are invented; no game data or generated game code. */
#include "module_loader.h"

#ifndef FIXTURE_KIND
#define FIXTURE_KIND 0
#endif

extern void module_loader_test_event(unsigned event);
#if FIXTURE_KIND == 6
extern void module_loader_fixture_missing_dependency(void);
#endif

__attribute__((constructor)) static void fixture_construct(void) {
#if FIXTURE_KIND == 6
    module_loader_fixture_missing_dependency(); /* RTLD_NOW must fail before this constructor */
#endif
    module_loader_test_event(1);
}

__attribute__((destructor)) static void fixture_destruct(void) {
    module_loader_test_event(4);
}

static void synthetic_guest(struct Cpu *cpu) { (void)cpu; module_loader_test_event(2); }
static const uint64_t main_addresses[] = {UINT64_C(0x1000000100), UINT64_C(0x1000000104)};
static const uint64_t sdk_addresses[] = {UINT64_C(0x1000010100), UINT64_C(0x1000010104)};
static const uint64_t subsdk0_addresses[] = {UINT64_C(0x1000020100), UINT64_C(0x1000020104)};
static const uint64_t subsdk1_addresses[] = {UINT64_C(0x1000030100), UINT64_C(0x1000030104)};
static const ModuleAbiGuestFn good_functions[] = {synthetic_guest, synthetic_guest};
#if FIXTURE_KIND == 5
static const ModuleAbiGuestFn main_functions[] = {NULL, synthetic_guest};
#else
#define main_functions good_functions
#endif

#if FIXTURE_KIND == 4
#define MAIN_HASH_LAST 0xb0
#else
#define MAIN_HASH_LAST 0xb1
#endif

#define IMAGE(id, base, hash_last) { \
    .struct_size = sizeof(ModuleAbiModuleSpec), .module_id = id, \
    .nso_build_id = {id, [31] = 0xa0 + id}, .nso_sha256 = {10 + id, [31] = hash_last}, \
    .canonical_base = UINT64_C(base), .mapped_size = 0x4000, \
    .text_offset = 0x100, .text_size = 0x100, .function_count = 2 }

static const ModuleAbiModule modules[] = {
    {.struct_size = sizeof(ModuleAbiModule), .image = IMAGE(1, 0x7000000000, MAIN_HASH_LAST),
     .address_count = 2, .addresses = main_addresses, .functions = main_functions},
    {.struct_size = sizeof(ModuleAbiModule), .image = IMAGE(2, 0x7000010000, 0xb2),
     .address_count = 2, .addresses = sdk_addresses, .functions = good_functions},
    {.struct_size = sizeof(ModuleAbiModule), .image = IMAGE(3, 0x7000020000, 0xb3),
     .address_count = 2, .addresses = subsdk0_addresses, .functions = good_functions},
    {.struct_size = sizeof(ModuleAbiModule), .image = IMAGE(4, 0x7000030000, 0xb4),
     .address_count = 2, .addresses = subsdk1_addresses, .functions = good_functions}
};

#if FIXTURE_KIND == 1
#define FIXTURE_MODULE_COUNT 4
#else
#define FIXTURE_MODULE_COUNT 2
#endif
#if FIXTURE_KIND == 3
#define FIXTURE_VERSION (MODULE_ABI_VERSION + 1)
#else
#define FIXTURE_VERSION MODULE_ABI_VERSION
#endif

#define DESCRIPTOR { .struct_size = sizeof(ModuleAbiDescriptor), .module_count = FIXTURE_MODULE_COUNT, \
    .identity = {.struct_size = sizeof(ModuleAbiIdentity), .abi_version = FIXTURE_VERSION, \
        .target = MODULE_ABI_TARGET_ANDROID_ARM64_LP64, .cpu_identity = MODULE_ABI_CPU_A64_CACHE_66560, \
        .cpu_size = 66560, .cpu_alignment = 16, .guest_shift = UINT64_C(0x6000000000), \
        .guest_window_lo = UINT64_C(0x1000000000), .guest_window_hi = UINT64_C(0x1000100000)}, \
    .modules = modules }

#if FIXTURE_KIND == 2
static const ModuleAbiDescriptor getter_descriptor = DESCRIPTOR;
/* A legacy getter exists, but the data-only gate must never call it. */
MODULE_LOADER_API const ModuleAbiDescriptor *lasgame_tables(void) {
    module_loader_test_event(3);
    return &getter_descriptor;
}
#else
MODULE_LOADER_API const ModuleAbiDescriptor recomp_module_descriptor_v1 = DESCRIPTOR;
#endif
