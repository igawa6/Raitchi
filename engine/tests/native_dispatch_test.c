/* Each case runs in a fresh process. No game files, mappings or native entry. */
#include "runtime/aot_hooks.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static const uint8_t contract[32] = {0x67, 0x28};
static NativeManifest manifest;
static NativeManifestFailure failure;
static void backend(Cpu *cpu) {
    ++cpu->x[0]; cpu->x[1] = cpu->pc; cpu->x[2] = cpu->x[30];
    if (aot_original_call_take(cpu, cpu->pc)) ++cpu->x[24];
}
static void host_a(Cpu *cpu) { cpu->x[0] += 10; }
static void host_b(Cpu *cpu) { cpu->x[0] += 100; }
static void guard(Cpu *cpu) { ++cpu->x[25]; }
static int instruction(Cpu *cpu, uint64_t pc) { cpu->x[26] = pc; return 1; }
static int function(Cpu *cpu, uint64_t pc) { (void)cpu; (void)pc; return 0; }
static uint64_t base(unsigned i) { return manifest.modules[i].canonical_base - manifest.identity.guest_shift; }
static uint64_t text(unsigned i) { return base(i) + manifest.modules[i].text_offset; }
static void fixture(void) {
    manifest = (NativeManifest){.struct_size = sizeof manifest, .version = NATIVE_MANIFEST_VERSION,
        .title_id = 3, .module_count = 4, .program_id = UINT64_C(0x1234567812345678),
        .mapping_alignment = 16384,
        .identity = {.struct_size = sizeof(ModuleAbiIdentity), .abi_version = MODULE_ABI_VERSION,
            .target = MODULE_ABI_TARGET_ANDROID_ARM64_LP64, .cpu_identity = MODULE_ABI_CPU_A64_SHARED_66560,
            .cpu_size = sizeof(Cpu), .cpu_alignment = _Alignof(Cpu), .guest_shift = 0x10000000,
            .guest_window_lo = 0x10000000, .guest_window_hi = 0x70000000}};
    memcpy(manifest.contract_sha256, contract, 32);
    const uint32_t ids[] = {MODULE_ABI_MAIN, MODULE_ABI_SUBSDK0, MODULE_ABI_SUBSDK1, MODULE_ABI_SDK};
    const uint64_t bases[] = {0x20000000, 0x28000000, 0x2a000000, 0x2c000000};
    const uint64_t sizes[] = {0x4004000, 0xa8c000, 0x6b4000, 0xe04000};
    for (unsigned i = 0; i < 4; ++i) {
        manifest.modules[i] = (NativeModuleSpec){.struct_size = sizeof(NativeModuleSpec), .module_id = ids[i],
            .canonical_base = bases[i], .mapped_size = sizes[i], .text_offset = 0x1000, .text_size = 0x40000};
        manifest.modules[i].nso_build_id[31] = (uint8_t)(1 + i);
        manifest.modules[i].nso_sha256[0] = (uint8_t)(0x80 + i);
    }
}
static void bind(uint32_t caps) {
    CHECK(aot_dispatch_bind_native(&manifest, contract, caps, &failure) == AOT_OK);
    CHECK(failure.status == NATIVE_MANIFEST_OK);
    CHECK(aot_dispatch_is_bound() && aot_dispatch_capabilities() == caps);
}
static void init_cpu(Cpu *cpu) {
    aot_cpu_initialize(cpu); CHECK(aot_dispatch_attach_cpu(cpu) == AOT_OK);
}
static void call(Cpu *cpu, uint64_t pc) { cpu->pc = pc; rt_dispatch(cpu); }

static ModuleAbiModuleSpec aot_specs[2];
static ModuleAbiModule aot_modules[2];
static uint64_t aot_addresses[2];
static const ModuleAbiGuestFn aot_functions[] = {host_a};
static ModuleAbiDescriptor aot_descriptor;
static ModuleAbiProfile aot_profile;
static void aot_fixture(void) {
    for (unsigned i = 0; i < 2; ++i) {
        aot_specs[i] = (ModuleAbiModuleSpec){.struct_size = sizeof(ModuleAbiModuleSpec), .module_id = i + 1,
            .canonical_base = 0x20000000 + i * 0x100000, .mapped_size = 0x10000,
            .text_offset = 0x1000, .text_size = 0x2000, .function_count = 1};
        aot_specs[i].nso_build_id[0] = (uint8_t)(i + 1); aot_specs[i].nso_sha256[0] = (uint8_t)(i + 33);
        aot_addresses[i] = aot_specs[i].canonical_base - manifest.identity.guest_shift + 0x1000;
        aot_modules[i] = (ModuleAbiModule){.struct_size = sizeof(ModuleAbiModule), .image = aot_specs[i],
            .address_count = 1, .addresses = &aot_addresses[i], .functions = aot_functions};
    }
    aot_descriptor = (ModuleAbiDescriptor){.struct_size = sizeof aot_descriptor, .module_count = 2,
        .identity = manifest.identity, .modules = aot_modules};
    aot_profile = (ModuleAbiProfile){.struct_size = sizeof aot_profile, .module_count = 2,
        .identity = manifest.identity, .modules = aot_specs};
}
static void unbound(void) {
    Cpu cpu; aot_cpu_initialize(&cpu);
    CHECK(!aot_dispatch_is_bound()); CHECK(!dispatch_lookup(text(0))); CHECK(!aot_dispatch_contains_text(text(0)));
    CHECK(aot_dispatch_attach_cpu(&cpu) == AOT_NOT_BOUND);
    CHECK(aot_dispatch_set_execution_backend(backend) == AOT_NOT_BOUND);
    CHECK(aot_dispatch_override(text(0), host_a) == AOT_NOT_BOUND);
    CHECK(aot_dispatch_add_hle(0x60000000, host_a) == AOT_NOT_BOUND);
    CHECK(aot_dispatch_set_hle_guard(guard) == AOT_NOT_BOUND);
}
static void order_case(unsigned selected) {
    NativeModuleSpec original[4]; memcpy(original, manifest.modules, sizeof original);
    unsigned index = 0;
    for (unsigned a = 0; a < 4; ++a) for (unsigned b = 0; b < 4; ++b)
        for (unsigned c = 0; c < 4; ++c) for (unsigned d = 0; d < 4; ++d)
            if (a != b && a != c && a != d && b != c && b != d && c != d && index++ == selected) {
                manifest.modules[0] = original[a]; manifest.modules[1] = original[b];
                manifest.modules[2] = original[c]; manifest.modules[3] = original[d];
            }
    CHECK(index == 24 && selected < 24); bind(0);
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(aot_dispatch_contains_text(text(i))); CHECK(aot_dispatch_contains_text(text(i) + 4));
        CHECK(aot_dispatch_contains_text(text(i) + manifest.modules[i].text_size - 4));
        CHECK(!aot_dispatch_contains_text(text(i) + 1)); CHECK(!aot_dispatch_contains_text(base(i)));
        CHECK(!aot_dispatch_contains_text(text(i) + manifest.modules[i].text_size));
        CHECK(dispatch_lookup(text(i)) == NULL && aot_dispatch_host_lookup(text(i)) == NULL);
    }
    CHECK(!aot_dispatch_contains_text(0) && !aot_dispatch_contains_text(UINT64_MAX));
}
static void invalid_case(void) {
    NativeManifest good = manifest;
    CHECK(aot_dispatch_bind_native(NULL, contract, 0, &failure) == AOT_ABI_MISMATCH);
    CHECK(failure.status == NATIVE_MANIFEST_ARGUMENT && !aot_dispatch_is_bound());
    CHECK(aot_dispatch_bind_native(&manifest, NULL, 0, &failure) == AOT_ABI_MISMATCH);
#define REJECT(change, expected_status) do { manifest = good; change; \
    CHECK(aot_dispatch_bind_native(&manifest, contract, 0, &failure) == AOT_ABI_MISMATCH); \
    CHECK(failure.status == (expected_status)); CHECK(!aot_dispatch_is_bound()); \
} while (0)
    REJECT(manifest.identity.cpu_identity = MODULE_ABI_CPU_A64_CACHE_66560, NATIVE_MANIFEST_IDENTITY);
    REJECT(manifest.identity.cpu_identity = MODULE_ABI_CPU_A64_PREFIX_1024; manifest.identity.cpu_size = 1024, NATIVE_MANIFEST_IDENTITY);
    REJECT(manifest.identity.cpu_size = 1024, NATIVE_MANIFEST_IDENTITY);
    REJECT(manifest.identity.target = 99, NATIVE_MANIFEST_IDENTITY);
    REJECT(manifest.modules[1].module_id = MODULE_ABI_MAIN, NATIVE_MANIFEST_MODULE);
    REJECT(manifest.modules[1].canonical_base = manifest.modules[0].canonical_base, NATIVE_MANIFEST_OVERLAP);
    REJECT(manifest.contract_sha256[31] ^= 1, NATIVE_MANIFEST_CONTRACT);
    REJECT(memset(manifest.modules[2].nso_sha256, 0, 32), NATIVE_MANIFEST_MODULE);
#undef REJECT
    manifest = good;
    CHECK(aot_dispatch_bind_native(&manifest, contract, AOT_CAP_ALL | 0x80, &failure) == AOT_CAPABILITY_MISMATCH);
    CHECK(!aot_dispatch_is_bound() && !aot_dispatch_capabilities());
    bind(AOT_CAP_ALL);
}
static void exclusivity(int aot_first) {
    aot_fixture();
    if (aot_first) {
        CHECK(aot_dispatch_bind(&aot_descriptor, &aot_profile, 0, 0, NULL) == AOT_OK);
        CHECK(dispatch_lookup(aot_addresses[0]) == host_a);
        CHECK(aot_dispatch_bind_native(&manifest, contract, 0, &failure) == AOT_ALREADY_BOUND);
        Cpu cpu; init_cpu(&cpu); call(&cpu, aot_addresses[0]); CHECK(cpu.x[0] == 10);
        CHECK(aot_dispatch_detach_cpu(&cpu) == AOT_OK);
    } else {
        bind(0);
        CHECK(aot_dispatch_bind(&aot_descriptor, &aot_profile, 0, 0, NULL) == AOT_ALREADY_BOUND);
        CHECK(dispatch_lookup(aot_addresses[0]) == NULL);
    }
    CHECK(aot_dispatch_bind_native(&manifest, contract, 0, NULL) == AOT_ALREADY_BOUND);
}
static void execution(void) {
    bind(AOT_CAP_ALL); Cpu first, second; init_cpu(&first); init_cpu(&second);
    CHECK(aot_dispatch_attach_cpu(&first) == AOT_OK);
    CHECK(aot_dispatch_override(text(0), host_a) == AOT_ARGUMENT);
    CHECK(aot_dispatch_set_execution_backend(NULL) == AOT_ARGUMENT);
    CHECK(aot_dispatch_set_execution_backend(backend) == AOT_OK);
    CHECK(aot_dispatch_set_execution_backend(backend) == AOT_ALREADY_BOUND);
    for (unsigned i = 0; i < 4; ++i) {
        call(&first, text(i) + 4); CHECK(first.x[0] == i + 1 && first.x[1] == text(i) + 4);
        CHECK(dispatch_lookup(text(i) + 4) == NULL);
    }
    first.x[30] = 0xface; guest_call(&first, text(0) + 8);
    CHECK(first.x[0] == 5 && !first.x[2] && first.x[30] == 0xface);
    call(&second, text(0) + 8);
    CHECK(first.dcache[aot_dispatch_cache_slot(text(0) + 8)].fn == backend);
    CHECK(second.dcache[aot_dispatch_cache_slot(text(0) + 8)].fn == backend);
    CHECK(aot_dispatch_override(text(0) + 8, host_a) == AOT_OK);
    CHECK(!first.dcache[aot_dispatch_cache_slot(text(0) + 8)].fn);
    CHECK(!second.dcache[aot_dispatch_cache_slot(text(0) + 8)].fn);
    call(&first, text(0) + 8); CHECK(first.x[0] == 15);
    CHECK(!first.dcache[aot_dispatch_cache_slot(text(0) + 8)].fn);
    CHECK(aot_dispatch_host_lookup(text(0) + 8) == host_a);
    CHECK(aot_dispatch_override(text(0) + 8, host_b) == AOT_OK);
    call(&first, text(0) + 8); CHECK(first.x[0] == 115);
    CHECK(aot_dispatch_override_count() == 1);
    uint64_t address = 0; CHECK(aot_dispatch_override_address(0, &address) && address == text(0) + 8);
    CHECK(!aot_dispatch_override_address(1, &address));
    CHECK(aot_dispatch_override(text(0) + 1, host_a) == AOT_ARGUMENT);
    CHECK(aot_dispatch_override(base(0), host_a) == AOT_ARGUMENT);
    CHECK(aot_dispatch_override(text(0), NULL) == AOT_ARGUMENT);
    aot_native_call_original(&first, text(0) + 8);
    CHECK(first.x[0] == 116 && first.x[24] == 1 && first.x[30] == 0xface);
    CHECK(!aot_original_call_take(&first, text(0) + 8));
    CHECK(aot_dispatch_detach_cpu(&first) == AOT_OK); CHECK(aot_dispatch_detach_cpu(&first) == AOT_NOT_FOUND);
    CHECK(aot_dispatch_detach_cpu(&second) == AOT_OK);
}
static void hle_case(void) {
    bind(0); Cpu cpu; init_cpu(&cpu);
    for (unsigned i = 0; i < 4; ++i) {
        uint64_t end = base(i) + manifest.modules[i].mapped_size;
        CHECK(aot_dispatch_add_hle(base(i), host_a) == AOT_ARGUMENT);
        CHECK(aot_dispatch_add_hle(text(i), host_a) == AOT_ARGUMENT);
        CHECK(aot_dispatch_add_hle(text(i) + manifest.modules[i].text_size, host_a) == AOT_ARGUMENT);
        CHECK(aot_dispatch_add_hle(end - 4, host_a) == AOT_ARGUMENT);
        CHECK(aot_dispatch_add_hle(end, host_a) == AOT_OK);
        CHECK(dispatch_lookup(end) == NULL && aot_dispatch_host_lookup(end) == host_a);
    }
    const uint64_t pc = UINT64_C(0x60000000);
    CHECK(!aot_dispatch_host_lookup(pc));
    CHECK(aot_dispatch_add_hle(0, host_a) == AOT_ARGUMENT);
    CHECK(aot_dispatch_add_hle(pc + 1, host_a) == AOT_ARGUMENT);
    CHECK(aot_dispatch_add_hle(pc, NULL) == AOT_ARGUMENT);
    CHECK(aot_dispatch_add_hle(pc, host_a) == AOT_OK);
    CHECK(aot_dispatch_set_hle_guard(guard) == AOT_OK);
    call(&cpu, pc); CHECK(cpu.x[0] == 10 && cpu.x[25] == 1);
    CHECK(!cpu.dcache[aot_dispatch_cache_slot(pc)].fn);
    CHECK(aot_dispatch_add_hle(pc, host_b) == AOT_OK);
    call(&cpu, pc); CHECK(cpu.x[0] == 110 && cpu.x[25] == 2);
    CHECK(aot_dispatch_host_lookup(pc) == host_b && dispatch_lookup(pc) == NULL);
    CHECK(aot_dispatch_detach_cpu(&cpu) == AOT_OK);
}
static void hooks_case(int enabled) {
    bind(enabled ? AOT_CAP_ALL : 0); Cpu cpu; init_cpu(&cpu);
    CHECK(rt_register_instruction_hook(text(0), instruction) == (enabled ? AOT_OK : AOT_CAPABILITY_MISMATCH));
    if (enabled) {
        CHECK(rt_hook(&cpu, text(0)) == 1 && cpu.x[26] == text(0));
        CHECK(rt_register_instruction_hook(base(0), instruction) == AOT_ARGUMENT);
        CHECK(aot_instruction_hook_count() == 1);
        /* Generated-entry hooks remain a generated-code protocol; no fake
         * lookup is synthesized merely to admit this separate interface. */
        CHECK(rt_register_fnhook(text(0), function) == AOT_ARGUMENT);
    } else CHECK(rt_register_fnhook(text(0), function) == AOT_CAPABILITY_MISMATCH);
    CHECK(aot_dispatch_detach_cpu(&cpu) == AOT_OK);
}
typedef struct ThreadCase { Cpu cpu; unsigned errors; } ThreadCase;
static void *run_cpu(void *opaque) {
    ThreadCase *t = opaque;
    for (unsigned i = 0; i < 10000; ++i) {
        t->cpu.pc = text(i % 4); rt_dispatch(&t->cpu);
        if (t->cpu.x[0] != (uint64_t)i + 1 || t->cpu.x[1] != text(i % 4) || dispatch_lookup(text(i % 4))) ++t->errors;
    }
    return NULL;
}
static void thread_case(void) {
    bind(0); CHECK(aot_dispatch_set_execution_backend(backend) == AOT_OK);
    ThreadCase *cases = calloc(4, sizeof *cases); CHECK(cases != NULL); pthread_t threads[4];
    for (unsigned i = 0; i < 4; ++i) { init_cpu(&cases[i].cpu); CHECK(!pthread_create(&threads[i], NULL, run_cpu, &cases[i])); }
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(!pthread_join(threads[i], NULL)); CHECK(!cases[i].errors && cases[i].cpu.x[0] == 10000);
        CHECK(aot_dispatch_detach_cpu(&cases[i].cpu) == AOT_OK);
    }
    free(cases);
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    fixture();
    if (!strncmp(argv[1], "order-", 6)) order_case((unsigned)strtoul(argv[1] + 6, NULL, 10));
    else if (!strcmp(argv[1], "unbound")) unbound();
    else if (!strcmp(argv[1], "invalid")) invalid_case();
    else if (!strcmp(argv[1], "native-first")) exclusivity(0);
    else if (!strcmp(argv[1], "aot-first")) exclusivity(1);
    else if (!strcmp(argv[1], "execution")) execution();
    else if (!strcmp(argv[1], "hle")) hle_case();
    else if (!strcmp(argv[1], "hooks")) hooks_case(1);
    else if (!strcmp(argv[1], "no-hooks")) hooks_case(0);
    else if (!strcmp(argv[1], "threads")) thread_case();
    else return 2;
    printf("{\"state\":\"passed\",\"case\":\"%s\",\"checks\":%u}\n", argv[1], checks);
    return 0;
}
