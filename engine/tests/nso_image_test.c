/* Asset-free NSO0/LZ4 bounds and ownership tests. No guest mappings or calls. */
#include "runtime/nso_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } } while (0)
typedef struct Fixture {
    uint8_t raw[0x200];
    size_t size;
    ModuleAbiModuleSpec spec;
    ModuleAbiIdentity identity;
} Fixture;
static void put32(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}
static void init(Fixture *f, int compressed) {
    *f = (Fixture){0}; f->size = sizeof f->raw;
    f->spec = (ModuleAbiModuleSpec){.struct_size = sizeof f->spec, .module_id = MODULE_ABI_MAIN,
        .canonical_base = UINT64_C(0x5000000000), .mapped_size = 4096,
        .text_offset = 0, .text_size = 8, .function_count = 2};
    f->identity = (ModuleAbiIdentity){.struct_size = sizeof f->identity, .abi_version = MODULE_ABI_VERSION,
        .target = MODULE_ABI_TARGET_ANDROID_ARM64_LP64, .cpu_identity = MODULE_ABI_CPU_A64_SHARED_66560,
        .cpu_size = 66560, .cpu_alignment = 16, .guest_shift = UINT64_C(0x2000000000),
        .guest_window_lo = UINT64_C(0x3000000000), .guest_window_hi = UINT64_C(0x3000010000)};
    memcpy(f->raw, "NSO0", 4);
    for (unsigned i = 0; i < 32; ++i) f->raw[0x40 + i] = f->spec.nso_build_id[i] = (uint8_t)(i + 1);
    f->spec.nso_sha256[0] = 7; /* independently supplied digest; not checked cryptographically here */
    put32(f->raw + 0xc, compressed ? 7 : 0);
    for (unsigned i = 0; i < 3; ++i) {
        put32(f->raw + 0x10 + i * 0x10, 0x100 + i * 0x10);
        put32(f->raw + 0x14 + i * 0x10, i * 16);
        put32(f->raw + 0x18 + i * 0x10, 8);
        put32(f->raw + 0x60 + i * 4, compressed ? 9 : 8);
        unsigned start = 0x100 + i * 0x10;
        if (compressed) f->raw[start++] = 0x80;
        for (unsigned j = 0; j < 8; ++j) f->raw[start + j] = (uint8_t)(i * 16 + j + 1);
    }
    put32(f->raw + 0x3c, 8);
}
static NativeModuleSpec native_spec(const Fixture *f) {
    NativeModuleSpec m = {.struct_size = sizeof m, .module_id = f->spec.module_id,
        .canonical_base = f->spec.canonical_base, .mapped_size = f->spec.mapped_size,
        .text_offset = f->spec.text_offset, .text_size = f->spec.text_size};
    memcpy(m.nso_build_id, f->spec.nso_build_id, 32); memcpy(m.nso_sha256, f->spec.nso_sha256, 32);
    return m;
}
static void expect_native_failure(Fixture *f, const NativeModuleSpec *spec, uint32_t alignment,
    enum NsoImageStatus status, uint32_t segment) {
    NsoImage image; memset(&image, 0xa5, sizeof image);
    NsoImageFailure failure = {NSO_IMAGE_OK, 100};
    uint8_t before[sizeof f->raw]; memcpy(before, f->raw, sizeof before);
    enum NsoImageStatus got = nso_image_decode_native(f->raw, f->size, spec, &f->identity, alignment, &image, &failure);
    CHECK(got == status); CHECK(failure.status == got && failure.segment_index == segment);
    NsoImage zero = {0}; CHECK(memcmp(&image, &zero, sizeof image) == 0);
    CHECK(!memcmp(before, f->raw, sizeof before));
}
static void expect_failure(Fixture *f, uint32_t alignment, enum NsoImageStatus status, uint32_t segment) {
    NsoImage image; memset(&image, 0xa5, sizeof image);
    NsoImageFailure failure = {NSO_IMAGE_OK, 100};
    enum NsoImageStatus got = nso_image_decode(f->raw, f->size, &f->spec, &f->identity, alignment, &image, &failure);
    CHECK(got == status); CHECK(failure.status == got && failure.segment_index == segment);
    NsoImage zero = {0}; CHECK(memcmp(&image, &zero, sizeof image) == 0);
    /* Replay original-image failures through the genuine native entry point.
     * AOT-only function-table policy is tested separately below. */
    if (f->spec.function_count && f->spec.function_count <= SIZE_MAX / sizeof(ModuleAbiGuestFn) &&
        f->spec.function_count <= f->spec.text_size / 4) {
        NativeModuleSpec spec = native_spec(f);
        expect_native_failure(f, &spec, alignment, status, segment);
    }
}
#define REJECT(label, change, status, segment) do { \
    Fixture f; init(&f, 0); change; (void)(label); expect_failure(&f, 4096, status, segment); \
} while (0)
static void valid_images(void) {
    for (int compressed = 0; compressed <= 1; ++compressed) {
        Fixture f; init(&f, compressed);
        NsoImage image; NsoImageFailure failure;
        CHECK(nso_image_decode(f.raw, f.size, &f.spec, &f.identity, 4096, &image, &failure) == NSO_IMAGE_OK);
        CHECK(failure.status == NSO_IMAGE_OK && failure.segment_index == NSO_IMAGE_NO_SEGMENT);
        CHECK(image.size == 4096 && image.actual_base == f.identity.guest_window_lo);
        CHECK(image.image_end == 48 && image.bss_offset == 40 && image.bss_size == 8 && image.data_size_with_bss == 16);
        CHECK(memcmp(image.build_id, f.spec.nso_build_id, 32) == 0);
        for (unsigned i = 0; i < 3; ++i) {
            CHECK(image.segments[i].compressed == (unsigned)compressed);
            CHECK(image.segments[i].memory_offset == i * 16 && image.segments[i].size == 8);
            for (unsigned j = 0; j < 8; ++j) CHECK(image.bytes[i * 16 + j] == i * 16 + j + 1);
        }
        for (unsigned i = 0; i < image.size; ++i)
            if ((i >= 8 && i < 16) || (i >= 24 && i < 32) || i >= 40) CHECK(image.bytes[i] == 0);
        NativeModuleSpec spec = native_spec(&f); NsoImage native;
        CHECK(nso_image_decode_native(f.raw, f.size, &spec, &f.identity, 4096, &native, NULL) == NSO_IMAGE_OK);
        CHECK(native.bytes != image.bytes && native.size == image.size && native.actual_base == image.actual_base);
        CHECK(native.image_end == image.image_end && native.bss_offset == image.bss_offset &&
            native.bss_size == image.bss_size && native.data_size_with_bss == image.data_size_with_bss);
        CHECK(!memcmp(native.build_id, image.build_id, 32));
        CHECK(!memcmp(native.segments, image.segments, sizeof image.segments));
        CHECK(!memcmp(native.bytes, image.bytes, image.size));
        nso_image_destroy(&native);
        /* The result owns its bytes; it must survive source changes. */
        memset(f.raw, 0xff, sizeof f.raw); CHECK(image.bytes[0] == 1 && image.bytes[32] == 33);
        nso_image_destroy(&image); CHECK(image.bytes == NULL && image.size == 0);
        nso_image_destroy(&image); /* empty destroy is harmless */
    }
    Fixture f; init(&f, 0); NsoImage image;
    f.spec.mapped_size = 16384;
    CHECK(nso_image_decode(f.raw, f.size, &f.spec, &f.identity, 16384, &image, NULL) == NSO_IMAGE_OK);
    CHECK(image.size == 16384); nso_image_destroy(&image);
    init(&f, 0); f.identity.guest_shift = 0; f.identity.guest_window_lo = f.spec.canonical_base;
    f.identity.guest_window_hi = f.spec.canonical_base + 0x10000;
    CHECK(nso_image_decode(f.raw, f.size, &f.spec, &f.identity, 4096, &image, NULL) == NSO_IMAGE_OK);
    CHECK(image.actual_base == f.spec.canonical_base); free(image.bytes);
    init(&f, 0); put32(f.raw + 0xc, 0x38); memset(f.raw + 0xa0, 0x91, 96);
    CHECK(nso_image_decode(f.raw, f.size, &f.spec, &f.identity, 4096, &image, NULL) == NSO_IMAGE_OK);
    CHECK(image.segments[2].hash_requested == 1 && image.segments[2].header_sha256[31] == 0x91);
    nso_image_destroy(&image); /* hash flags recorded, never claimed verified */
    init(&f, 0); put32(f.raw + 0x38, 0); put32(f.raw + 0x3c, 0);
    CHECK(nso_image_decode(f.raw, f.size, &f.spec, &f.identity, 4096, &image, NULL) == NSO_IMAGE_OK);
    CHECK(image.segments[2].size == 0 && image.bss_size == 0 && image.image_end == 32);
    nso_image_destroy(&image);
}
static void malformed_images(void) {
    REJECT("short header", f.size = 0xff, NSO_IMAGE_HEADER, NSO_IMAGE_NO_SEGMENT);
    REJECT("zero length", f.size = 0, NSO_IMAGE_HEADER, NSO_IMAGE_NO_SEGMENT);
    REJECT("magic", f.raw[0] = 'X', NSO_IMAGE_HEADER, NSO_IMAGE_NO_SEGMENT);
    REJECT("version", put32(f.raw + 4, 1), NSO_IMAGE_HEADER, NSO_IMAGE_NO_SEGMENT);
    REJECT("reserved header", put32(f.raw + 8, 1), NSO_IMAGE_HEADER, NSO_IMAGE_NO_SEGMENT);
    REJECT("unknown flags", put32(f.raw + 0xc, 0x40), NSO_IMAGE_HEADER, NSO_IMAGE_NO_SEGMENT);
    REJECT("full build ID", f.raw[0x5f] ^= 1, NSO_IMAGE_BUILD_ID, NSO_IMAGE_NO_SEGMENT);
    REJECT("file start", put32(f.raw + 0x10, UINT32_MAX), NSO_IMAGE_FILE_RANGE, 0);
    REJECT("file end", put32(f.raw + 0x10, sizeof f.raw - 4), NSO_IMAGE_FILE_RANGE, 0);
    REJECT("payload in header", put32(f.raw + 0x10, 0xfc), NSO_IMAGE_FILE_RANGE, 0);
    REJECT("memory start", put32(f.raw + 0x14, UINT32_MAX), NSO_IMAGE_MEMORY_RANGE, 0);
    REJECT("memory end", put32(f.raw + 0x34, 4092), NSO_IMAGE_MEMORY_RANGE, 2);
    REJECT("wide end arithmetic", put32(f.raw + 0x34, UINT32_MAX - 3); put32(f.raw + 0x38, 8), NSO_IMAGE_MEMORY_RANGE, 2);
    REJECT("BSS bounds", put32(f.raw + 0x3c, UINT32_MAX), NSO_IMAGE_MEMORY_RANGE, 2);
    REJECT("memory overlap", put32(f.raw + 0x24, 4), NSO_IMAGE_OVERLAP, 1);
    REJECT("file overlap", put32(f.raw + 0x20, 0x104), NSO_IMAGE_OVERLAP, 1);
    REJECT("BSS overlaps rodata", put32(f.raw + 0x34, 8), NSO_IMAGE_OVERLAP, 1);
    REJECT("text mismatch", f.spec.text_size = 4; f.spec.function_count = 1, NSO_IMAGE_EXPECTATION, 0);
    REJECT("mapping padding mismatch", f.spec.mapped_size = 8192, NSO_IMAGE_MAPPED_SIZE, NSO_IMAGE_NO_SEGMENT);
    REJECT("identity size", f.identity.struct_size--, NSO_IMAGE_EXPECTATION, NSO_IMAGE_NO_SEGMENT);
    REJECT("identity target", f.identity.target = 999, NSO_IMAGE_EXPECTATION, NSO_IMAGE_NO_SEGMENT);
    REJECT("identity CPU", f.identity.cpu_size = 1024, NSO_IMAGE_EXPECTATION, NSO_IMAGE_NO_SEGMENT);
    REJECT("window wrap", f.identity.guest_window_hi = 0, NSO_IMAGE_EXPECTATION, NSO_IMAGE_NO_SEGMENT);
    REJECT("shift underflow", f.identity.guest_shift = f.spec.canonical_base + 4, NSO_IMAGE_EXPECTATION, NSO_IMAGE_NO_SEGMENT);
    REJECT("mapping wrap", f.spec.mapped_size = UINT64_MAX, NSO_IMAGE_EXPECTATION, NSO_IMAGE_NO_SEGMENT);
    REJECT("zero text expectation", f.spec.text_size = 0, NSO_IMAGE_EXPECTATION, NSO_IMAGE_NO_SEGMENT);
    REJECT("table capacity expectation", f.spec.function_count = UINT64_MAX, NSO_IMAGE_EXPECTATION, NSO_IMAGE_NO_SEGMENT);
    Fixture f; init(&f, 0); expect_failure(&f, 3, NSO_IMAGE_EXPECTATION, NSO_IMAGE_NO_SEGMENT);
    expect_failure(&f, 0, NSO_IMAGE_EXPECTATION, NSO_IMAGE_NO_SEGMENT);
    init(&f, 1); put32(f.raw + 0x60, UINT32_MAX); expect_failure(&f, 4096, NSO_IMAGE_FILE_RANGE, 0);
    init(&f, 1); f.raw[0x100] = 0xf0; put32(f.raw + 0x60, 1); expect_failure(&f, 4096, NSO_IMAGE_LZ4, 0);
    init(&f, 1); f.raw[0x100] = 0x70; put32(f.raw + 0x60, 8); expect_failure(&f, 4096, NSO_IMAGE_DECODE_SIZE, 0);
    NsoImage image; NsoImageFailure failure;
    CHECK(nso_image_decode(NULL, f.size, &f.spec, &f.identity, 4096, &image, &failure) == NSO_IMAGE_ARGUMENT);
    CHECK(image.bytes == NULL && failure.segment_index == NSO_IMAGE_NO_SEGMENT);
    CHECK(nso_image_decode(f.raw, f.size, NULL, &f.identity, 4096, &image, NULL) == NSO_IMAGE_ARGUMENT);
    CHECK(nso_image_decode(f.raw, f.size, &f.spec, NULL, 4096, &image, NULL) == NSO_IMAGE_ARGUMENT);
    CHECK(nso_image_decode(f.raw, f.size, &f.spec, &f.identity, 4096, NULL, NULL) == NSO_IMAGE_ARGUMENT);
}
static void bad_lz4(const uint8_t *src, size_t size, size_t capacity) {
    uint8_t destination[300]; memset(destination, 0xa5, sizeof destination);
    size_t written = 99;
    CHECK(capacity < sizeof destination);
    CHECK(nso_lz4_decompress(src, size, destination, capacity, &written) == NSO_IMAGE_LZ4);
    CHECK(written == 0 && destination[capacity] == 0xa5);
}
static void native_expectations(void) {
    Fixture f; init(&f, 1); NativeModuleSpec spec = native_spec(&f);
    NsoImage image; NsoImageFailure failure;
    /* No generated function count is present anywhere in the native request.
     * Identical image with zero/invalid AOT counts must still decode natively. */
    const uint64_t counts[] = {0, 3, UINT64_MAX};
    for (unsigned i = 0; i < sizeof counts / sizeof counts[0]; ++i) {
        f.spec.function_count = counts[i];
        expect_failure(&f, 4096, NSO_IMAGE_EXPECTATION, NSO_IMAGE_NO_SEGMENT);
        CHECK(nso_image_decode_native(f.raw, f.size, &spec, &f.identity, 4096, &image, &failure) == NSO_IMAGE_OK);
        CHECK(image.bytes[0] == 1 && image.bss_size == 8); nso_image_destroy(&image);
    }
    for (uint32_t id = MODULE_ABI_MAIN; id <= MODULE_ABI_SUBSDK1; ++id) {
        spec.module_id = id;
        CHECK(nso_image_decode_native(f.raw, f.size, &spec, &f.identity, 4096, &image, NULL) == NSO_IMAGE_OK);
        nso_image_destroy(&image);
    }
    spec.module_id = MODULE_ABI_MAIN;
#define NATIVE_BAD(change) do { \
    init(&f, 1); spec = native_spec(&f); change; \
    expect_native_failure(&f, &spec, 4096, NSO_IMAGE_EXPECTATION, NSO_IMAGE_NO_SEGMENT); \
} while (0)
    NATIVE_BAD(spec.struct_size = 0);
    NATIVE_BAD(spec.module_id = 0);
    NATIVE_BAD(spec.module_id = UINT32_MAX);
    NATIVE_BAD(memset(spec.nso_build_id, 0, 32));
    NATIVE_BAD(memset(spec.nso_sha256, 0, 32));
    NATIVE_BAD(spec.text_size = 0);
    NATIVE_BAD(spec.text_size = UINT64_MAX);
    NATIVE_BAD(spec.text_offset = UINT64_MAX);
    NATIVE_BAD(spec.canonical_base += 4);
    NATIVE_BAD(f.identity.guest_shift += 4);
    NATIVE_BAD(f.identity.guest_window_lo += 4);
    NATIVE_BAD(f.identity.guest_window_hi -= 4);
    NATIVE_BAD(f.identity.guest_window_hi = UINT64_MAX & ~UINT64_C(4095));
    NATIVE_BAD(f.identity.cpu_identity = MODULE_ABI_CPU_A64_CACHE_66560);
    NATIVE_BAD(f.identity.cpu_identity = MODULE_ABI_CPU_A64_PREFIX_1024; f.identity.cpu_size = 1024);
    NATIVE_BAD(f.identity.cpu_alignment = 8);
    NATIVE_BAD(f.identity.abi_version = 2);
    NATIVE_BAD(spec.mapped_size = UINT64_MAX);
    NATIVE_BAD(spec.canonical_base = UINT64_MAX & ~UINT64_C(4095); f.identity.guest_shift = 0;
        f.identity.guest_window_lo = spec.canonical_base; f.identity.guest_window_hi = UINT64_MAX);
#undef NATIVE_BAD
    init(&f, 1); spec = native_spec(&f);
    expect_native_failure(&f, &spec, 131072, NSO_IMAGE_EXPECTATION, NSO_IMAGE_NO_SEGMENT);
    CHECK(nso_image_decode_native(NULL, f.size, &spec, &f.identity, 4096, &image, &failure) == NSO_IMAGE_ARGUMENT);
    CHECK(image.bytes == NULL && failure.segment_index == NSO_IMAGE_NO_SEGMENT);
    CHECK(nso_image_decode_native(f.raw, f.size, NULL, &f.identity, 4096, &image, NULL) == NSO_IMAGE_ARGUMENT);
    CHECK(nso_image_decode_native(f.raw, f.size, &spec, NULL, 4096, &image, NULL) == NSO_IMAGE_ARGUMENT);
    CHECK(nso_image_decode_native(f.raw, f.size, &spec, &f.identity, 4096, NULL, NULL) == NSO_IMAGE_ARGUMENT);
    /* The existing AOT decoder still supports its originally accepted legacy
     * CPU contracts; native CPU3 policy does not narrow the old entry point. */
    const uint32_t identities[] = {MODULE_ABI_CPU_A64_PREFIX_1024, MODULE_ABI_CPU_A64_CACHE_66560};
    for (unsigned i = 0; i < 2; ++i) {
        init(&f, 0); f.identity.cpu_identity = identities[i]; f.identity.cpu_size = i ? 66560 : 1024;
        CHECK(nso_image_decode(f.raw, f.size, &f.spec, &f.identity, 4096, &image, NULL) == NSO_IMAGE_OK);
        nso_image_destroy(&image);
    }
    /* Native mapping policy supports actual Android page sizes explicitly. */
    for (uint32_t alignment = 4096; alignment <= 65536; alignment *= 4) {
        init(&f, 1); spec = native_spec(&f); spec.mapped_size = alignment;
        CHECK(nso_image_decode_native(f.raw, f.size, &spec, &f.identity, alignment, &image, NULL) == NSO_IMAGE_OK);
        CHECK(image.size == alignment && image.bytes[0] == 1 && image.bytes[alignment - 1] == 0);
        nso_image_destroy(&image);
    }
}
static void lz4_cases(void) {
    uint8_t out[300]; size_t written;
    const uint8_t literals[] = {0x80, 1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(nso_lz4_decompress(literals, sizeof literals, out, sizeof out, &written) == NSO_IMAGE_OK);
    CHECK(written == 8 && memcmp(out, literals + 1, 8) == 0);
    for (size_t i = 1; i < sizeof literals; ++i) bad_lz4(literals, i, sizeof out - 1);
    bad_lz4(literals, sizeof literals, 7);
    const uint8_t overlap[] = {0x1a, 'A', 1, 0, 0x50, 'A', 'B', 'C', 'D', 'E'};
    CHECK(nso_lz4_decompress(overlap, sizeof overlap, out, sizeof out, &written) == NSO_IMAGE_OK);
    CHECK(written == 20 && memcmp(out, "AAAAAAAAAAAAAAAA" "BCDE", 20) == 0);
    const uint8_t extended_match[] = {0x1f, 'Q', 1, 0, 250, 0x50, 'a', 'b', 'c', 'd', 'e'};
    CHECK(nso_lz4_decompress(extended_match, sizeof extended_match, out, sizeof out, &written) == NSO_IMAGE_OK);
    CHECK(written == 275 && out[269] == 'Q' && memcmp(out + 270, "abcde", 5) == 0);
    uint8_t extended_literal[22] = {0xf0, 5}; for (unsigned i = 2; i < sizeof extended_literal; ++i) extended_literal[i] = (uint8_t)i;
    CHECK(nso_lz4_decompress(extended_literal, sizeof extended_literal, out, sizeof out, &written) == NSO_IMAGE_OK);
    CHECK(written == 20 && out[0] == 2 && out[19] == 21);
    const uint8_t zero_offset[] = {0x10, 'a', 0, 0, 0}; bad_lz4(zero_offset, sizeof zero_offset, 20);
    const uint8_t before_start[] = {0x10, 'a', 2, 0, 0}; bad_lz4(before_start, sizeof before_start, 20);
    const uint8_t short_offset[] = {0x10, 'a', 1}; bad_lz4(short_offset, sizeof short_offset, 20);
    const uint8_t missing_literal_extension[] = {0xf0}; bad_lz4(missing_literal_extension, 1, 20);
    const uint8_t truncated_literal_extension[] = {0xf0, 255}; bad_lz4(truncated_literal_extension, 2, 20);
    const uint8_t missing_match_extension[] = {0x1f, 'a', 1, 0}; bad_lz4(missing_match_extension, 4, 20);
    const uint8_t truncated_match_extension[] = {0x1f, 'a', 1, 0, 255}; bad_lz4(truncated_match_extension, 5, 20);
    const uint8_t missing_final_sequence[] = {0x10, 'a', 1, 0}; bad_lz4(missing_final_sequence, 4, 20);
    bad_lz4(overlap, sizeof overlap, 14);
    CHECK(nso_lz4_decompress(NULL, 0, NULL, 0, &written) == NSO_IMAGE_OK && written == 0);
    CHECK(nso_lz4_decompress(NULL, 1, out, sizeof out, &written) == NSO_IMAGE_ARGUMENT);
    CHECK(nso_lz4_decompress(literals, sizeof literals, NULL, 8, &written) == NSO_IMAGE_ARGUMENT);
    CHECK(nso_lz4_decompress(literals, sizeof literals, out, sizeof out, NULL) == NSO_IMAGE_ARGUMENT);
    memcpy(out, literals, sizeof literals);
    CHECK(nso_lz4_decompress(out, sizeof literals, out + 1, 8, &written) == NSO_IMAGE_ARGUMENT);
}
int main(void) {
    valid_images(); malformed_images(); lz4_cases(); native_expectations();
    printf("NSO image: %u synthetic checks passed; no guest mappings or execution\n", checks);
    return 0;
}
