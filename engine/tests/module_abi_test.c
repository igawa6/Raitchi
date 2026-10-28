/* Synthetic descriptors only: no game assets, lifted code, or guest execution. */
#include "module_abi.h"

#include <stdio.h>
#include <stdlib.h>

static unsigned guest_calls;
static unsigned checks;
static void synthetic_guest(struct Cpu *cpu) { (void)cpu; ++guest_calls; }

typedef struct Fixture {
    ModuleAbiProfile expected;
    ModuleAbiDescriptor descriptor;
    ModuleAbiModuleSpec specs[4];
    ModuleAbiModule modules[4];
    uint64_t addresses[4][3];
    ModuleAbiGuestFn functions[4][3];
} Fixture;

static void fixture_init(Fixture *f) {
    *f = (Fixture){0};
    f->expected.struct_size = sizeof f->expected;
    f->expected.module_count = 2;
    f->expected.identity = (ModuleAbiIdentity){
        .struct_size = sizeof(ModuleAbiIdentity), .abi_version = MODULE_ABI_VERSION,
        .target = MODULE_ABI_TARGET_ANDROID_ARM64_LP64,
        .cpu_identity = MODULE_ABI_CPU_A64_CACHE_66560, .cpu_size = 66560, .cpu_alignment = 16,
        .guest_shift = UINT64_C(0x6000000000),
        .guest_window_lo = UINT64_C(0x1000000000), .guest_window_hi = UINT64_C(0x1000100000)
    };
    f->expected.modules = f->specs;
    f->descriptor.struct_size = sizeof f->descriptor;
    f->descriptor.module_count = f->expected.module_count;
    f->descriptor.identity = f->expected.identity;
    f->descriptor.modules = f->modules;
    for (uint32_t i = 0; i < 4; ++i) {
        ModuleAbiModuleSpec *spec = &f->specs[i];
        spec->struct_size = sizeof *spec;
        spec->module_id = i + MODULE_ABI_MAIN;
        for (uint32_t b = 0; b < 32; ++b) {
            spec->nso_build_id[b] = (uint8_t)(i + b + 1);
            spec->nso_sha256[b] = (uint8_t)(i + b + 51);
        }
        spec->canonical_base = UINT64_C(0x7000000000) + (uint64_t)i * 0x10000;
        spec->mapped_size = 0x2000;
        spec->text_offset = 0x100;
        spec->text_size = 0x100;
        spec->function_count = 3;
        f->modules[i].struct_size = sizeof f->modules[i];
        f->modules[i].image = *spec;
        f->modules[i].address_count = 3;
        f->modules[i].addresses = f->addresses[i];
        f->modules[i].functions = f->functions[i];
        for (uint32_t j = 0; j < 3; ++j) {
            f->addresses[i][j] = spec->canonical_base - f->descriptor.identity.guest_shift + 0x100 + j * 4;
            f->functions[i][j] = synthetic_guest;
        }
    }
}

static void expect(const char *name, const ModuleAbiDescriptor *d, const ModuleAbiProfile *p,
                   enum ModuleAbiStatus status, uint32_t module, uint64_t function) {
    ModuleAbiFailure failure = {MODULE_ABI_FUNCTION, 100, 100};
    enum ModuleAbiStatus actual = module_abi_validate(d, p, &failure);
    ++checks;
    if (actual != status || failure.status != actual || failure.module_index != module ||
        failure.function_index != function) {
        fprintf(stderr, "%s: expected %s [%u,%llu], got %s [%u,%llu]\n", name,
                module_abi_status_string(status), module, (unsigned long long)function,
                module_abi_status_string(actual), failure.module_index,
                (unsigned long long)failure.function_index);
        exit(1);
    }
}

static void expect_profile(const char *name, const ModuleAbiProfile *profile,
                           enum ModuleAbiStatus status, uint32_t module, uint64_t function) {
    ModuleAbiFailure failure = {MODULE_ABI_FUNCTION, 100, 100};
    enum ModuleAbiStatus actual = module_abi_validate_profile(profile, &failure);
    ++checks;
    if (actual != status || failure.status != actual || failure.module_index != module ||
        failure.function_index != function) {
        fprintf(stderr, "profile %s: expected %s [%u,%llu], got %s [%u,%llu]\n", name,
                module_abi_status_string(status), module, (unsigned long long)function,
                module_abi_status_string(actual), failure.module_index,
                (unsigned long long)failure.function_index);
        exit(1);
    }
}

#define NONE MODULE_ABI_NO_MODULE, MODULE_ABI_NO_FUNCTION
#define AT(m) m, MODULE_ABI_NO_FUNCTION
#define RUN(label, changes, status, ...) do { \
    Fixture f; fixture_init(&f); changes; \
    expect(label, &f.descriptor, &f.expected, status, __VA_ARGS__); \
} while (0)
#define PROFILE(label, changes, status, ...) do { \
    Fixture f; fixture_init(&f); changes; \
    expect_profile(label, &f.expected, status, __VA_ARGS__); \
} while (0)

static void profile_tests(void) {
    PROFILE("two modules", (void)0, MODULE_ABI_OK, NONE);
    PROFILE("four modules", f.expected.module_count = 4, MODULE_ABI_OK, NONE);
    PROFILE("shared CPU", f.expected.identity.cpu_identity = MODULE_ABI_CPU_A64_SHARED_66560,
        MODULE_ABI_OK, NONE);
    PROFILE("prefix CPU metadata remains supported", f.expected.identity.cpu_identity = MODULE_ABI_CPU_A64_PREFIX_1024;
        f.expected.identity.cpu_size = 1024, MODULE_ABI_OK, NONE);
    PROFILE("no generated tables required", f.descriptor.modules = NULL;
        for (unsigned i = 0; i < 4; ++i) { f.modules[i].addresses = NULL; f.modules[i].functions = NULL; },
        MODULE_ABI_OK, NONE);
    PROFILE("touching module ranges", f.specs[0].mapped_size = 0x10000, MODULE_ABI_OK, NONE);
    PROFILE("last byte nonzero digests", for (unsigned b = 0; b < 31; ++b)
        f.specs[0].nso_build_id[b] = f.specs[0].nso_sha256[b] = 0, MODULE_ABI_OK, NONE);
    expect_profile("null profile", NULL, MODULE_ABI_NULL, NONE);
    PROFILE("profile size", --f.expected.struct_size, MODULE_ABI_SIZE, NONE);
    PROFILE("identity size", --f.expected.identity.struct_size, MODULE_ABI_SIZE, NONE);
    PROFILE("ABI version", ++f.expected.identity.abi_version, MODULE_ABI_VERSION_MISMATCH, NONE);
    PROFILE("target", f.expected.identity.target = 999, MODULE_ABI_TARGET_MISMATCH, NONE);
    PROFILE("CPU identity", f.expected.identity.cpu_identity = 999, MODULE_ABI_CPU_MISMATCH, NONE);
    PROFILE("CPU size", f.expected.identity.cpu_size = 1024, MODULE_ABI_CPU_MISMATCH, NONE);
    PROFILE("CPU alignment", f.expected.identity.cpu_alignment = 8, MODULE_ABI_CPU_MISMATCH, NONE);
    PROFILE("zero window", f.expected.identity.guest_window_lo = 0, MODULE_ABI_LAYOUT, NONE);
    PROFILE("empty window", f.expected.identity.guest_window_hi = f.expected.identity.guest_window_lo,
        MODULE_ABI_LAYOUT, NONE);
    PROFILE("unaligned shift", ++f.expected.identity.guest_shift, MODULE_ABI_LAYOUT, NONE);
    PROFILE("unaligned window", ++f.expected.identity.guest_window_hi, MODULE_ABI_LAYOUT, NONE);
    PROFILE("zero modules", f.expected.module_count = 0, MODULE_ABI_PROFILE_MISMATCH, NONE);
    PROFILE("three modules", f.expected.module_count = 3, MODULE_ABI_PROFILE_MISMATCH, NONE);
    PROFILE("huge module count", f.expected.module_count = UINT32_MAX, MODULE_ABI_PROFILE_MISMATCH, NONE);
    PROFILE("null module specs", f.expected.modules = NULL, MODULE_ABI_NULL, NONE);
    PROFILE("module spec size", --f.specs[1].struct_size, MODULE_ABI_SIZE, AT(1));
    PROFILE("wrong module order", f.specs[0].module_id = MODULE_ABI_SDK, MODULE_ABI_PROFILE_MISMATCH, AT(0));
    PROFILE("blank build ID", for (unsigned b = 0; b < 32; ++b) f.specs[0].nso_build_id[b] = 0,
        MODULE_ABI_METADATA_MISMATCH, AT(0));
    PROFILE("blank hash", for (unsigned b = 0; b < 32; ++b) f.specs[1].nso_sha256[b] = 0,
        MODULE_ABI_METADATA_MISMATCH, AT(1));
    PROFILE("zero mapping", f.specs[0].mapped_size = 0, MODULE_ABI_LAYOUT, AT(0));
    PROFILE("zero text", f.specs[0].text_size = 0, MODULE_ABI_LAYOUT, AT(0));
    PROFILE("zero functions", f.specs[0].function_count = 0, MODULE_ABI_LAYOUT, AT(0));
    PROFILE("shift underflow", f.specs[0].canonical_base = f.expected.identity.guest_shift - 4,
        MODULE_ABI_LAYOUT, AT(0));
    PROFILE("below window", f.specs[0].canonical_base -= 4, MODULE_ABI_LAYOUT, AT(0));
    PROFILE("crosses window", f.specs[0].mapped_size = UINT64_MAX, MODULE_ABI_LAYOUT, AT(0));
    PROFILE("text offset outside mapping", f.specs[0].text_offset = UINT64_MAX, MODULE_ABI_LAYOUT, AT(0));
    PROFILE("text crosses mapping", f.specs[0].text_offset = 0x1f80, MODULE_ABI_LAYOUT, AT(0));
    PROFILE("unaligned base", ++f.specs[0].canonical_base, MODULE_ABI_LAYOUT, AT(0));
    PROFILE("unaligned text", ++f.specs[0].text_size, MODULE_ABI_LAYOUT, AT(0));
    PROFILE("unrepresentable table", f.specs[0].function_count = UINT64_MAX, MODULE_ABI_TABLE, AT(0));
    PROFILE("functions exceed text capacity", f.specs[0].function_count = 65, MODULE_ABI_TABLE, AT(0));
    PROFILE("overlap", f.specs[1].canonical_base = f.specs[0].canonical_base + 0x1000,
        MODULE_ABI_LAYOUT, AT(1));
    PROFILE("fourth module order checked", f.expected.module_count = 4; f.specs[3].module_id = MODULE_ABI_SUBSDK0,
        MODULE_ABI_PROFILE_MISMATCH, AT(3));
    PROFILE("fourth module overlap checked", f.expected.module_count = 4;
        f.specs[3].canonical_base = f.specs[2].canonical_base, MODULE_ABI_LAYOUT, AT(3));
    Fixture f; fixture_init(&f);
    if (module_abi_validate_profile(&f.expected, NULL) != MODULE_ABI_OK) exit(1);
    ++checks;
}

int main(void) {
    profile_tests();
    RUN("two modules", (void)0, MODULE_ABI_OK, NONE);
    RUN("four modules", f.expected.module_count = f.descriptor.module_count = 4, MODULE_ABI_OK, NONE);
    RUN("prefix CPU identity", f.expected.identity.cpu_identity = f.descriptor.identity.cpu_identity = MODULE_ABI_CPU_A64_PREFIX_1024;
        f.expected.identity.cpu_size = f.descriptor.identity.cpu_size = 1024, MODULE_ABI_OK, NONE);
    RUN("shared execution CPU identity", f.expected.identity.cpu_identity = f.descriptor.identity.cpu_identity =
        MODULE_ABI_CPU_A64_SHARED_66560, MODULE_ABI_OK, NONE);
    RUN("legacy cache is not shared CPU", f.expected.identity.cpu_identity = MODULE_ABI_CPU_A64_SHARED_66560,
        MODULE_ABI_CPU_MISMATCH, NONE);
    RUN("zero shift", f.expected.identity.guest_shift = f.descriptor.identity.guest_shift = 0;
        f.expected.identity.guest_window_lo += UINT64_C(0x6000000000);
        f.expected.identity.guest_window_hi += UINT64_C(0x6000000000);
        f.descriptor.identity = f.expected.identity;
        for (unsigned i = 0; i < 4; ++i) for (unsigned j = 0; j < 3; ++j)
            f.addresses[i][j] += UINT64_C(0x6000000000), MODULE_ABI_OK, NONE);
    RUN("last aligned text instruction", f.addresses[0][2] = f.addresses[0][0] + 0xfc, MODULE_ABI_OK, NONE);
    RUN("touching module boundaries", f.modules[0].image.mapped_size = f.specs[0].mapped_size = 0x10000,
        MODULE_ABI_OK, NONE);

    Fixture f;
    fixture_init(&f);
    expect("null descriptor", NULL, &f.expected, MODULE_ABI_NULL, NONE);
    expect("null expected", &f.descriptor, NULL, MODULE_ABI_NULL, NONE);
    if (module_abi_validate(&f.descriptor, &f.expected, NULL) != MODULE_ABI_OK) return 1;
    ++checks;
    RUN("descriptor size", --f.descriptor.struct_size, MODULE_ABI_SIZE, NONE);
    RUN("profile size", ++f.expected.struct_size, MODULE_ABI_SIZE, NONE);
    RUN("identity size", --f.descriptor.identity.struct_size, MODULE_ABI_SIZE, NONE);
    RUN("expected identity size", --f.expected.identity.struct_size, MODULE_ABI_SIZE, NONE);
    RUN("module size", --f.modules[1].struct_size, MODULE_ABI_SIZE, AT(1));
    RUN("image spec size", --f.modules[0].image.struct_size, MODULE_ABI_SIZE, AT(0));
    RUN("expected spec size", --f.specs[0].struct_size, MODULE_ABI_SIZE, AT(0));
    RUN("unknown ABI", ++f.descriptor.identity.abi_version, MODULE_ABI_VERSION_MISMATCH, NONE);
    RUN("unknown expected ABI", ++f.expected.identity.abi_version, MODULE_ABI_VERSION_MISMATCH, NONE);
    RUN("wrong target", f.descriptor.identity.target = 999, MODULE_ABI_TARGET_MISMATCH, NONE);
    RUN("wrong expected target", f.expected.identity.target = 999, MODULE_ABI_TARGET_MISMATCH, NONE);
    RUN("valid incompatible CPU", f.descriptor.identity.cpu_identity = MODULE_ABI_CPU_A64_PREFIX_1024;
        f.descriptor.identity.cpu_size = 1024, MODULE_ABI_CPU_MISMATCH, NONE);
    RUN("CPU size contradiction", f.descriptor.identity.cpu_size = 1024, MODULE_ABI_CPU_MISMATCH, NONE);
    RUN("CPU alignment", f.descriptor.identity.cpu_alignment = 8, MODULE_ABI_CPU_MISMATCH, NONE);
    RUN("unknown CPU", f.descriptor.identity.cpu_identity = 999, MODULE_ABI_CPU_MISMATCH, NONE);
    RUN("shift mismatch", f.descriptor.identity.guest_shift += 4, MODULE_ABI_PROFILE_MISMATCH, NONE);
    RUN("window mismatch", f.descriptor.identity.guest_window_hi += 4, MODULE_ABI_PROFILE_MISMATCH, NONE);
    RUN("empty window", f.descriptor.identity.guest_window_hi = f.descriptor.identity.guest_window_lo,
        MODULE_ABI_LAYOUT, NONE);
    RUN("reversed window", f.descriptor.identity.guest_window_hi = f.descriptor.identity.guest_window_lo - 4,
        MODULE_ABI_LAYOUT, NONE);
    RUN("null guest window", f.descriptor.identity.guest_window_lo = 0, MODULE_ABI_LAYOUT, NONE);
    RUN("unaligned shift", ++f.descriptor.identity.guest_shift, MODULE_ABI_LAYOUT, NONE);
    RUN("unaligned window", ++f.descriptor.identity.guest_window_hi, MODULE_ABI_LAYOUT, NONE);
    RUN("module count mismatch", f.descriptor.module_count = 4, MODULE_ABI_PROFILE_MISMATCH, NONE);
    RUN("invalid module count", f.expected.module_count = f.descriptor.module_count = 3,
        MODULE_ABI_PROFILE_MISMATCH, NONE);
    RUN("too many modules", f.expected.module_count = f.descriptor.module_count = UINT32_MAX,
        MODULE_ABI_PROFILE_MISMATCH, NONE);
    RUN("no modules", f.expected.module_count = f.descriptor.module_count = 0,
        MODULE_ABI_PROFILE_MISMATCH, NONE);
    RUN("null module array", f.descriptor.modules = NULL, MODULE_ABI_NULL, NONE);
    RUN("null expected array", f.expected.modules = NULL, MODULE_ABI_NULL, NONE);
    RUN("module order", f.modules[0].image.module_id = MODULE_ABI_SDK, MODULE_ABI_PROFILE_MISMATCH, AT(0));
    RUN("expected module order", f.specs[0].module_id = MODULE_ABI_SDK, MODULE_ABI_PROFILE_MISMATCH, AT(0));
    RUN("reserved bits", f.modules[0].reserved = 1, MODULE_ABI_PROFILE_MISMATCH, AT(0));
    RUN("full build ID", f.modules[0].image.nso_build_id[31] ^= 1, MODULE_ABI_METADATA_MISMATCH, AT(0));
    RUN("full hash", f.modules[0].image.nso_sha256[31] ^= 1, MODULE_ABI_METADATA_MISMATCH, AT(0));
    RUN("missing build ID", for (unsigned b = 0; b < 32; ++b) f.modules[0].image.nso_build_id[b] = 0,
        MODULE_ABI_METADATA_MISMATCH, AT(0));
    RUN("missing hash", for (unsigned b = 0; b < 32; ++b) f.modules[0].image.nso_sha256[b] = 0,
        MODULE_ABI_METADATA_MISMATCH, AT(0));
    RUN("base mismatch", f.modules[0].image.canonical_base += 4, MODULE_ABI_METADATA_MISMATCH, AT(0));
    RUN("mapping size mismatch", f.modules[0].image.mapped_size += 4, MODULE_ABI_METADATA_MISMATCH, AT(0));
    RUN("text offset mismatch", f.modules[0].image.text_offset += 4, MODULE_ABI_METADATA_MISMATCH, AT(0));
    RUN("text size mismatch", f.modules[0].image.text_size += 4, MODULE_ABI_METADATA_MISMATCH, AT(0));
    RUN("function count mismatch", f.modules[0].image.function_count = 2, MODULE_ABI_METADATA_MISMATCH, AT(0));
    RUN("shift underflow", f.modules[0].image.canonical_base = f.descriptor.identity.guest_shift - 4,
        MODULE_ABI_LAYOUT, AT(0));
    RUN("module below window", f.modules[0].image.canonical_base -= 4, MODULE_ABI_LAYOUT, AT(0));
    RUN("module starts at window end", f.modules[0].image.canonical_base =
        f.descriptor.identity.guest_shift + f.descriptor.identity.guest_window_hi, MODULE_ABI_LAYOUT, AT(0));
    RUN("module crosses window", f.modules[0].image.mapped_size = UINT64_C(0x100004), MODULE_ABI_LAYOUT, AT(0));
    RUN("mapping addition overflow", f.modules[0].image.mapped_size = UINT64_MAX, MODULE_ABI_LAYOUT, AT(0));
    RUN("empty mapping", f.modules[0].image.mapped_size = 0, MODULE_ABI_LAYOUT, AT(0));
    RUN("empty text", f.modules[0].image.text_size = 0, MODULE_ABI_LAYOUT, AT(0));
    RUN("text offset outside mapping", f.modules[0].image.text_offset = UINT64_MAX, MODULE_ABI_LAYOUT, AT(0));
    RUN("text addition overflow", f.modules[0].image.text_size = UINT64_MAX, MODULE_ABI_LAYOUT, AT(0));
    RUN("text crosses mapping", f.modules[0].image.text_offset = 0x1f80, MODULE_ABI_LAYOUT, AT(0));
    RUN("unaligned base", ++f.modules[0].image.canonical_base, MODULE_ABI_LAYOUT, AT(0));
    RUN("unaligned text", ++f.modules[0].image.text_offset, MODULE_ABI_LAYOUT, AT(0));
    RUN("overlapping modules", f.modules[1].image.canonical_base = f.modules[0].image.canonical_base + 0x1000,
        MODULE_ABI_LAYOUT, AT(1));
    RUN("overlapping expected modules", f.specs[1].canonical_base = f.specs[0].canonical_base + 0x1000,
        MODULE_ABI_LAYOUT, AT(1));
    RUN("zero functions", f.modules[0].image.function_count = 0, MODULE_ABI_LAYOUT, AT(0));
    RUN("unrepresentable table", f.modules[0].image.function_count = UINT64_MAX, MODULE_ABI_TABLE, AT(0));
    RUN("too many functions for text", f.modules[0].image.function_count = 65, MODULE_ABI_TABLE, AT(0));
    RUN("address count mismatch", f.modules[0].address_count = 2, MODULE_ABI_TABLE, AT(0));
    RUN("null address table", f.modules[0].addresses = NULL, MODULE_ABI_NULL, AT(0));
    RUN("null function table", f.modules[0].functions = NULL, MODULE_ABI_NULL, AT(0));
    RUN("null function entry", f.functions[1][2] = NULL, MODULE_ABI_FUNCTION, 1, 2);
    RUN("unsorted addresses", f.addresses[0][1] = f.addresses[0][2] + 4, MODULE_ABI_TABLE, 0, 2);
    RUN("duplicate address", f.addresses[0][1] = f.addresses[0][0], MODULE_ABI_TABLE, 0, 1);
    RUN("unaligned function", ++f.addresses[0][1], MODULE_ABI_TABLE, 0, 1);
    RUN("address below text", f.addresses[0][0] -= 4, MODULE_ABI_TABLE, 0, 0);
    RUN("address at text end", f.addresses[0][2] = f.addresses[0][0] + 0x100, MODULE_ABI_TABLE, 0, 2);
    RUN("canonical unshifted address", f.addresses[0][0] += UINT64_C(0x6000000000), MODULE_ABI_TABLE, 0, 0);
    RUN("address wraparound", f.addresses[0][0] = UINT64_MAX - 3, MODULE_ABI_TABLE, 0, 0);
    RUN("fourth module checked", f.expected.module_count = f.descriptor.module_count = 4;
        f.functions[3][2] = NULL, MODULE_ABI_FUNCTION, 3, 2);
    if (guest_calls != 0) {
        fprintf(stderr, "validator invoked a guest function\n");
        return 1;
    }
    printf("module ABI: %u synthetic checks passed; no guest functions invoked\n", checks);
    return 0;
}
