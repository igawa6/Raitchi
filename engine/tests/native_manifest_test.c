/* Synthetic original-image metadata only: no game bytes or extracted inputs. */
#include "runtime/native_manifest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static const uint8_t contract[32] = {0x46, 0x91, 0x23};
static NativeManifestFailure failure;

static NativeManifest fixture(void) {
    /* Echoes-shaped sizes/order, deliberately unrelated synthetic addresses. */
    NativeManifest m = {.struct_size = sizeof m, .version = NATIVE_MANIFEST_VERSION,
        .title_id = 3, .module_count = 4, .program_id = UINT64_C(0x1122334455667788),
        .mapping_alignment = 16384,
        .identity = {.struct_size = sizeof(ModuleAbiIdentity), .abi_version = MODULE_ABI_VERSION,
            .target = MODULE_ABI_TARGET_ANDROID_ARM64_LP64, .cpu_identity = MODULE_ABI_CPU_A64_SHARED_66560,
            .cpu_size = 66560, .cpu_alignment = 16, .guest_shift = UINT64_C(0x10000000),
            .guest_window_lo = UINT64_C(0x10000000), .guest_window_hi = UINT64_C(0x70000000)}};
    static const uint32_t ids[] = {MODULE_ABI_MAIN, MODULE_ABI_SUBSDK0, MODULE_ABI_SUBSDK1, MODULE_ABI_SDK};
    static const uint64_t bases[] = {0x20000000, 0x28000000, 0x2a000000, 0x2c000000};
    static const uint64_t sizes[] = {0x4004000, 0xa8c000, 0x6b4000, 0xe04000};
    memcpy(m.contract_sha256, contract, 32);
    for (unsigned i = 0; i < 4; ++i) {
        m.modules[i] = (NativeModuleSpec){.struct_size = sizeof(NativeModuleSpec), .module_id = ids[i],
            .canonical_base = bases[i], .mapped_size = sizes[i], .text_offset = 0x1000,
            .text_size = sizes[i] / 2 & ~UINT64_C(3)};
        m.modules[i].nso_build_id[0] = (uint8_t)(i + 1);
        m.modules[i].nso_build_id[31] = (uint8_t)(0x90 + i);
        m.modules[i].nso_sha256[3] = (uint8_t)(0xa0 + i);
    }
    return m;
}
static void valid(const NativeManifest *m) {
    NativeManifest before = *m;
    CHECK(native_manifest_validate(m, contract, &failure) == NATIVE_MANIFEST_OK);
    CHECK(failure.status == NATIVE_MANIFEST_OK && failure.module_index == UINT32_MAX &&
        failure.other_module_index == UINT32_MAX && failure.target_index == SIZE_MAX);
    CHECK(!memcmp(m, &before, sizeof *m));
}
static void invalid(NativeManifest m, enum NativeManifestStatus status) {
    NativeManifest before = m;
    CHECK(native_manifest_validate(&m, contract, &failure) == status);
    CHECK(failure.status == status);
    CHECK(!memcmp(&m, &before, sizeof m));
}
static uint64_t text(const NativeManifest *m, unsigned i) {
    return m->modules[i].canonical_base - m->identity.guest_shift + m->modules[i].text_offset;
}
static void validation_tests(void) {
    NativeManifest f = fixture(), m;
    valid(&f);
    CHECK(native_manifest_validate(NULL, contract, &failure) == NATIVE_MANIFEST_ARGUMENT);
    CHECK(native_manifest_validate(&f, NULL, &failure) == NATIVE_MANIFEST_ARGUMENT);
    CHECK(native_manifest_validate(&f, contract, NULL) == NATIVE_MANIFEST_OK);
    uint8_t zero[32] = {0};
    CHECK(native_manifest_validate(&f, zero, &failure) == NATIVE_MANIFEST_CONTRACT);
#define BAD(field, value, status) do { m = f; m.field = (value); invalid(m, (status)); } while (0)
    BAD(struct_size, 0, NATIVE_MANIFEST_FORMAT);
    BAD(version, 2, NATIVE_MANIFEST_VERSION_MISMATCH);
    BAD(reserved, 1, NATIVE_MANIFEST_FORMAT);
    BAD(title_id, 0, NATIVE_MANIFEST_FORMAT);
    BAD(program_id, 0, NATIVE_MANIFEST_FORMAT);
    BAD(module_count, 0, NATIVE_MANIFEST_FORMAT);
    BAD(module_count, 5, NATIVE_MANIFEST_FORMAT);
    BAD(identity.struct_size, 0, NATIVE_MANIFEST_IDENTITY);
    BAD(identity.abi_version, 2, NATIVE_MANIFEST_IDENTITY);
    BAD(identity.target, 99, NATIVE_MANIFEST_IDENTITY);
    BAD(identity.cpu_identity, MODULE_ABI_CPU_A64_CACHE_66560, NATIVE_MANIFEST_IDENTITY);
    BAD(identity.cpu_size, 1024, NATIVE_MANIFEST_IDENTITY);
    BAD(identity.cpu_alignment, 8, NATIVE_MANIFEST_IDENTITY);
    BAD(mapping_alignment, 0, NATIVE_MANIFEST_RANGE);
    BAD(mapping_alignment, 2048, NATIVE_MANIFEST_RANGE);
    BAD(mapping_alignment, 12288, NATIVE_MANIFEST_RANGE);
    BAD(mapping_alignment, 131072, NATIVE_MANIFEST_RANGE);
    BAD(identity.guest_window_lo, 0, NATIVE_MANIFEST_RANGE);
    BAD(identity.guest_window_lo, f.identity.guest_window_hi, NATIVE_MANIFEST_RANGE);
    BAD(identity.guest_window_hi, 0, NATIVE_MANIFEST_RANGE);
    BAD(identity.guest_window_hi, UINT64_MAX, NATIVE_MANIFEST_RANGE);
    BAD(identity.guest_window_hi, UINT64_MAX & ~UINT64_C(16383), NATIVE_MANIFEST_RANGE);
    BAD(identity.guest_window_lo, f.identity.guest_window_lo + 4, NATIVE_MANIFEST_RANGE);
    BAD(identity.guest_shift, 4, NATIVE_MANIFEST_RANGE);
    BAD(modules[0].struct_size, 0, NATIVE_MANIFEST_MODULE);
    BAD(modules[0].module_id, 0, NATIVE_MANIFEST_MODULE);
    BAD(modules[0].module_id, UINT32_MAX, NATIVE_MANIFEST_MODULE);
    BAD(modules[0].module_id, MODULE_ABI_SDK, NATIVE_MANIFEST_MODULE);
    BAD(modules[0].mapped_size, 0, NATIVE_MANIFEST_RANGE);
    BAD(modules[0].mapped_size, UINT64_MAX, NATIVE_MANIFEST_RANGE);
    BAD(modules[0].mapped_size, 0x4004004, NATIVE_MANIFEST_RANGE);
    BAD(modules[0].canonical_base, 0, NATIVE_MANIFEST_RANGE);
    BAD(modules[0].canonical_base, f.modules[0].canonical_base + 4, NATIVE_MANIFEST_RANGE);
    BAD(modules[0].canonical_base, UINT64_MAX & ~UINT64_C(16383), NATIVE_MANIFEST_RANGE);
    BAD(modules[0].canonical_base, f.identity.guest_shift, NATIVE_MANIFEST_RANGE);
    BAD(modules[0].canonical_base, f.identity.guest_shift + f.identity.guest_window_hi, NATIVE_MANIFEST_RANGE);
    BAD(modules[0].text_size, 0, NATIVE_MANIFEST_RANGE);
    BAD(modules[0].text_size, 5, NATIVE_MANIFEST_RANGE);
    BAD(modules[0].text_size, UINT64_MAX - 3, NATIVE_MANIFEST_RANGE);
    BAD(modules[0].text_offset, 1, NATIVE_MANIFEST_RANGE);
    BAD(modules[0].text_offset, f.modules[0].mapped_size, NATIVE_MANIFEST_RANGE);
    BAD(modules[0].text_offset, UINT64_MAX - 3, NATIVE_MANIFEST_RANGE);
    BAD(modules[1].canonical_base, f.modules[0].canonical_base, NATIVE_MANIFEST_OVERLAP);
    CHECK(failure.module_index == 1 && failure.other_module_index == 0);
    m = f; memset(m.modules[0].nso_build_id, 0, 32); invalid(m, NATIVE_MANIFEST_MODULE);
    m = f; memset(m.modules[0].nso_sha256, 0, 32); invalid(m, NATIVE_MANIFEST_MODULE);
    m = f; m.contract_sha256[31] ^= 1; invalid(m, NATIVE_MANIFEST_CONTRACT);
    /* Full build ID is retained, including nonzero last bytes. */
    m = f; memset(m.modules[0].nso_build_id, 0, 32); m.modules[0].nso_build_id[31] = 1; valid(&m);
    /* Any permutation preserves stable IDs and symbol order, no index=ID rule. */
    for (unsigned a = 0; a < 4; ++a) for (unsigned b = 0; b < 4; ++b)
        for (unsigned c = 0; c < 4; ++c) for (unsigned d = 0; d < 4; ++d)
            if (a != b && a != c && a != d && b != c && b != d && c != d) {
                m = f; m.modules[0] = f.modules[a]; m.modules[1] = f.modules[b];
                m.modules[2] = f.modules[c]; m.modules[3] = f.modules[d]; valid(&m);
            }
    /* Adjacent images and exact endpoint text are valid. */
    m = f; m.modules[1].canonical_base = m.modules[0].canonical_base + m.modules[0].mapped_size; valid(&m);
    m.modules[1].canonical_base -= 16384; invalid(m, NATIVE_MANIFEST_OVERLAP);
    m = f; m.module_count = 1; memset(m.modules + 1, 0, 3 * sizeof m.modules[0]); valid(&m);
    m.modules[0].module_id = MODULE_ABI_SDK; invalid(m, NATIVE_MANIFEST_MODULE);
    m.modules[0].module_id = MODULE_ABI_MAIN;
    m.modules[3].nso_sha256[31] = 1; invalid(m, NATIVE_MANIFEST_FORMAT);
    m.modules[3].nso_sha256[31] = 0; m.modules[3].text_size = 4; invalid(m, NATIVE_MANIFEST_FORMAT);
    m = f; m.identity.guest_shift = 0;
    for (unsigned i = 0; i < 4; ++i) m.modules[i].canonical_base -= f.identity.guest_shift;
    valid(&m);
    for (uint32_t alignment = 4096; alignment <= 65536; alignment *= 2) {
        m = f; m.mapping_alignment = alignment;
        for (unsigned i = 0; i < 4; ++i)
            m.modules[i].mapped_size = (m.modules[i].mapped_size + alignment - 1) & ~(uint64_t)(alignment - 1);
        valid(&m);
    }
    m = f; m.modules[0].canonical_base = m.identity.guest_shift + m.identity.guest_window_hi - m.modules[0].mapped_size;
    valid(&m); m.modules[0].canonical_base += 16384; invalid(m, NATIVE_MANIFEST_RANGE);
    /* A valid high-address window must not be truncated to 32 bits or signed. */
    m = f; m.module_count = 1; memset(m.modules + 1, 0, 3 * sizeof m.modules[0]);
    m.identity.guest_shift = 0;
    m.identity.guest_window_hi = UINT64_MAX & ~UINT64_C(16383);
    m.identity.guest_window_lo = m.identity.guest_window_hi - 0x8000000;
    m.modules[0].canonical_base = m.identity.guest_window_hi - m.modules[0].mapped_size;
    m.modules[0].text_offset = m.modules[0].mapped_size - 4;
    m.modules[0].text_size = 4; valid(&m);
    CHECK(native_manifest_is_code_address(&m, m.identity.guest_window_hi - 4));
    CHECK(!native_manifest_is_code_address(&m, m.identity.guest_window_hi));
#undef BAD
}
static void target_tests(void) {
    NativeManifest m = fixture();
    CHECK(!native_manifest_find_module(NULL, MODULE_ABI_MAIN));
    CHECK(!native_manifest_find_module(&m, 0));
    CHECK(!native_manifest_find_module(&m, 99));
    CHECK(!native_manifest_is_code_address(NULL, 4));
    CHECK(!native_manifest_is_code_address(&m, 0));
    uint64_t targets[9] = {0};
    for (unsigned i = 0; i < 4; ++i) {
        const NativeModuleSpec *module = native_manifest_find_module(&m, m.modules[i].module_id);
        CHECK(module == &m.modules[i]);
        uint64_t start = text(&m, i), end = start + module->text_size;
        CHECK(native_manifest_is_code_address(&m, start));
        CHECK(native_manifest_is_code_address(&m, start + 4));
        CHECK(native_manifest_is_code_address(&m, end - 4));
        CHECK(!native_manifest_is_code_address(&m, start - 4));
        CHECK(!native_manifest_is_code_address(&m, start + 1));
        CHECK(!native_manifest_is_code_address(&m, end));
        CHECK(!native_manifest_is_code_address(&m, UINT64_MAX));
        targets[2 * i + 1] = start; targets[2 * i + 2] = end - 4;
    }
    CHECK(native_manifest_validate_init_targets(&m, contract, targets, 9, &failure) == NATIVE_MANIFEST_OK);
    CHECK(native_manifest_validate_init_targets(&m, contract, NULL, 0, &failure) == NATIVE_MANIFEST_OK);
    CHECK(native_manifest_validate_init_targets(&m, contract, NULL, 1, &failure) == NATIVE_MANIFEST_ARGUMENT);
    CHECK(native_manifest_validate_init_targets(&m, contract, targets, SIZE_MAX, &failure) == NATIVE_MANIFEST_ARGUMENT);
    const uint64_t invalid_targets[] = {1, UINT64_MAX, text(&m, 0) + 2,
        m.modules[0].canonical_base - m.identity.guest_shift,
        text(&m, 0) + m.modules[0].text_size, UINT64_C(0x60000000)};
    for (unsigned i = 0; i < sizeof invalid_targets / sizeof invalid_targets[0]; ++i) {
        targets[5] = invalid_targets[i];
        CHECK(native_manifest_validate_init_targets(&m, contract, targets, 9, &failure) == NATIVE_MANIFEST_INIT_TARGET);
        CHECK(failure.target_index == 5 && failure.module_index == UINT32_MAX);
    }
    m.modules[0].text_size = UINT64_MAX;
    CHECK(native_manifest_validate_init_targets(&m, contract, NULL, 0, &failure) == NATIVE_MANIFEST_RANGE);
}
static void put32(uint8_t *p, uint32_t n) { for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (8 * i)); }
static void put64(uint8_t *p, uint64_t n) { for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(n >> (8 * i)); }
static size_t serialize(const NativeManifest *m, uint8_t *p) {
    size_t length = NATIVE_MANIFEST_HEADER_SIZE + m->module_count * NATIVE_MANIFEST_MODULE_SIZE;
    memset(p, 0, length); memcpy(p, "NATVM001", 8);
    put32(p + 8, (uint32_t)length); put32(p + 12, m->version); put32(p + 16, m->title_id);
    put32(p + 20, m->module_count); put64(p + 24, m->program_id);
    put32(p + 32, m->identity.target); put32(p + 36, m->identity.cpu_identity);
    put32(p + 40, m->identity.cpu_size); put32(p + 44, m->identity.cpu_alignment);
    put32(p + 48, m->mapping_alignment); put64(p + 56, m->identity.guest_shift);
    put64(p + 64, m->identity.guest_window_lo); put64(p + 72, m->identity.guest_window_hi);
    memcpy(p + 80, m->contract_sha256, 32);
    for (uint32_t i = 0; i < m->module_count; ++i) {
        const NativeModuleSpec *s = &m->modules[i];
        uint8_t *r = p + NATIVE_MANIFEST_HEADER_SIZE + i * NATIVE_MANIFEST_MODULE_SIZE;
        put32(r, s->module_id); memcpy(r + 8, s->nso_build_id, 32); memcpy(r + 40, s->nso_sha256, 32);
        put64(r + 72, s->canonical_base); put64(r + 80, s->mapped_size);
        put64(r + 88, s->text_offset); put64(r + 96, s->text_size);
    }
    return length;
}
static void parse_bad(const uint8_t *wire, size_t length, enum NativeManifestStatus status) {
    NativeManifest out, blank = {0}; memset(&out, 0xa5, sizeof out);
    CHECK(native_manifest_parse(wire, length, contract, &out, &failure) == status);
    CHECK(failure.status == status && !memcmp(&out, &blank, sizeof out));
}
static void wire_tests(void) {
    NativeManifest m = fixture(), out;
    uint8_t storage[NATIVE_MANIFEST_MAX_SIZE + 3], original[NATIVE_MANIFEST_MAX_SIZE + 1];
    uint8_t *wire = storage + 1; /* all wire reads must tolerate unaligned input */
    size_t length = serialize(&m, wire); memcpy(original, wire, length);
    CHECK(native_manifest_parse(wire, length, contract, &out, &failure) == NATIVE_MANIFEST_OK);
    CHECK(!memcmp(&m, &out, sizeof m)); valid(&out);
    /* Output is self-contained, survives copies and input replacement. */
    NativeManifest copy = out; memset(wire, 0, length); valid(&copy);
    memcpy(wire, original, length);
    for (size_t cut = 0; cut < length; ++cut) parse_bad(wire, cut, NATIVE_MANIFEST_FORMAT);
    parse_bad(wire, length + 1, NATIVE_MANIFEST_FORMAT);
    parse_bad(wire, SIZE_MAX, NATIVE_MANIFEST_FORMAT);
    parse_bad(NULL, 0, NATIVE_MANIFEST_ARGUMENT);
    CHECK(native_manifest_parse(wire, length, contract, NULL, &failure) == NATIVE_MANIFEST_ARGUMENT);
    CHECK(native_manifest_parse(wire, length, NULL, &out, &failure) == NATIVE_MANIFEST_ARGUMENT);
    const unsigned format_offsets[] = {0, 7, 8, 11, 16, 17, 18, 19, 20, 21, 22, 23, 52, 53, 54, 55};
    for (unsigned i = 0; i < sizeof format_offsets / sizeof format_offsets[0]; ++i) {
        unsigned at = format_offsets[i]; memcpy(wire, original, length);
        /* Setting title to zero is invalid; other positive IDs are policy inputs. */
        if (at >= 16 && at < 20) { memset(wire + 16, 0, 4); }
        else wire[at] ^= 0x80;
        parse_bad(wire, length, NATIVE_MANIFEST_FORMAT);
    }
    for (unsigned at = 112; at < 128; ++at) {
        memcpy(wire, original, length); wire[at] = 1; parse_bad(wire, length, NATIVE_MANIFEST_FORMAT);
    }
    for (unsigned i = 0; i < 4; ++i) for (unsigned j = 4; j < 8; ++j) {
        memcpy(wire, original, length); wire[128 + i * 104 + j] = 1;
        parse_bad(wire, length, NATIVE_MANIFEST_FORMAT); CHECK(failure.module_index == i);
    }
    memcpy(wire, original, length); put32(wire + 12, 2); parse_bad(wire, length, NATIVE_MANIFEST_VERSION_MISMATCH);
    memcpy(wire, original, length); wire[111] ^= 1; parse_bad(wire, length, NATIVE_MANIFEST_CONTRACT);
    memcpy(wire, original, length); put32(wire + 32, 2); parse_bad(wire, length, NATIVE_MANIFEST_IDENTITY);
    memcpy(wire, original, length); put32(wire + 128 + 104, MODULE_ABI_MAIN); parse_bad(wire, length, NATIVE_MANIFEST_MODULE);
    /* Deterministic malformed declarations: failures clear output; accepted
     * mutations must satisfy exactly the same validator, with input unchanged. */
    uint32_t rng = 0x123457;
    for (unsigned i = 0; i < 4000; ++i) {
        memcpy(wire, original, length); rng = rng * 1664525u + 1013904223u;
        size_t at = rng % length; rng = rng * 1664525u + 1013904223u;
        wire[at] ^= (uint8_t)(1u << (rng % 8));
        uint8_t before[NATIVE_MANIFEST_MAX_SIZE]; memcpy(before, wire, length);
        memset(&out, 0xa5, sizeof out);
        enum NativeManifestStatus status = native_manifest_parse(wire, length, contract, &out, &failure);
        CHECK(!memcmp(wire, before, length)); CHECK(failure.status == status);
        if (status == NATIVE_MANIFEST_OK) valid(&out);
        else { NativeManifest blank = {0}; CHECK(!memcmp(&out, &blank, sizeof out)); }
    }
    m.module_count = 1; memset(m.modules + 1, 0, 3 * sizeof m.modules[0]);
    length = serialize(&m, wire);
    CHECK(native_manifest_parse(wire, length, contract, &out, &failure) == NATIVE_MANIFEST_OK); valid(&out);
    for (int status = NATIVE_MANIFEST_OK; status <= NATIVE_MANIFEST_INIT_TARGET; ++status)
        CHECK(strncmp(native_manifest_status_string((enum NativeManifestStatus)status), "unknown", 7));
    CHECK(!strcmp(native_manifest_status_string((enum NativeManifestStatus)99), "unknown native manifest status"));
}
int main(void) {
    validation_tests(); target_tests(); wire_tests();
    printf("{\"state\":\"passed\",\"checks\":%u,\"wire_fuzz_cases\":4000,\"module_permutations\":24}\n", checks);
    return 0;
}
