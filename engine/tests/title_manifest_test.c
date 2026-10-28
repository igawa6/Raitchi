#include "runtime/title_manifest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "manifest line%d: %s\n", __LINE__, #x); exit(1); } } while (0)
static void put32(uint8_t *p, uint32_t x) { for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(x >> (8 * i)); }
static void put64(uint8_t *p, uint64_t x) { for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(x >> (8 * i)); }
static uint8_t contract[32] = {0x17, 0x92};
static size_t fixture(uint8_t *wire, unsigned count) {
    size_t size = TITLE_MANIFEST_HEADER_SIZE + count * TITLE_MANIFEST_MODULE_SIZE;
    memset(wire, 0, TITLE_MANIFEST_MAX_SIZE);
    memcpy(wire, "SAOTM001", 8);
    put32(wire + 8, (uint32_t)size); put32(wire + 12, 1); put32(wire + 16, 1);
    put32(wire + 20, count == 2 ? 1 : 2); put32(wire + 24, 1); put32(wire + 28, 30);
    put32(wire + 32, 3); put32(wire + 36, 66560); put32(wire + 40, 16); put32(wire + 44, 1);
    put32(wire + 48, count == 2 ? 1 : 2); put32(wire + 52, count);
    uint64_t shift = count == 2 ? UINT64_C(0x2000000000) : 0;
    put64(wire + 56, shift); put64(wire + 64, UINT64_C(0x5000000000) - shift);
    put64(wire + 72, UINT64_C(0x5001000000) - shift);
    memcpy(wire + 80, contract, 32); wire[112] = 0x28; wire[144] = 0x39;
    for (unsigned i = 0; i < count; ++i) {
        uint8_t *record = wire + TITLE_MANIFEST_HEADER_SIZE + i * TITLE_MANIFEST_MODULE_SIZE;
        put32(record, i + 1); record[8] = (uint8_t)(i + 1); record[39] = (uint8_t)(i + 5);
        record[40] = (uint8_t)(i + 17); record[71] = (uint8_t)(i + 21);
        put64(record + 72, UINT64_C(0x5000000000) + i * 0x100000);
        put64(record + 80, 0x40000); put64(record + 88, 0x1000);
        put64(record + 96, 0x20000); put64(record + 104, 7);
    }
    return size;
}
static void reject(const uint8_t *wire, size_t size, enum TitleManifestStatus expected) {
    TitleManifest manifest, zero = {0};
    memset(&manifest, 0xa5, sizeof manifest);
    CHECK(title_manifest_parse(wire, size, contract, &manifest, NULL) == expected);
    CHECK(!memcmp(&manifest, &zero, sizeof manifest));
}
int main(void) {
    uint8_t wire[TITLE_MANIFEST_MAX_SIZE], original[sizeof wire];
    TitleManifest manifest;
    for (unsigned count = 2; count <= 4; count += 2) {
        size_t size = fixture(wire, count);
        memcpy(original, wire, sizeof wire);
        CHECK(title_manifest_parse(wire, size, contract, &manifest, NULL) == TITLE_MANIFEST_OK);
        CHECK(manifest.profile.modules == manifest.modules && manifest.profile.module_count == count);
        CHECK(manifest.profile.identity.cpu_identity == 3 && manifest.capabilities == (count == 2 ? 1 : 2));
        CHECK(manifest.modules[count - 1].nso_build_id[31] == count + 4);
        CHECK(manifest.modules[count - 1].nso_sha256[31] == count + 20);
        CHECK(!memcmp(original, wire, sizeof wire));
        for (size_t truncated = 0; truncated < size; ++truncated)
            reject(wire, truncated, TITLE_MANIFEST_FORMAT);
        for (unsigned offset = 8; offset <= 52; offset += 4) {
            memcpy(wire, original, sizeof wire);
            put32(wire + offset, UINT32_MAX);
            reject(wire, size, TITLE_MANIFEST_FORMAT);
        }
        memcpy(wire, original, sizeof wire); wire[0] ^= 1; reject(wire, size, TITLE_MANIFEST_FORMAT);
        memcpy(wire, original, sizeof wire); wire[80] ^= 1; reject(wire, size, TITLE_MANIFEST_CONTRACT);
        memcpy(wire, original, sizeof wire); memset(wire + 112, 0, 32); reject(wire, size, TITLE_MANIFEST_CONTRACT);
        memcpy(wire, original, sizeof wire); memset(wire + 144, 0, 32); reject(wire, size, TITLE_MANIFEST_CONTRACT);
        for (unsigned i = 0; i < count; ++i) {
            size_t record = TITLE_MANIFEST_HEADER_SIZE + i * TITLE_MANIFEST_MODULE_SIZE;
            memcpy(wire, original, sizeof wire); put32(wire + record + 4, 1); reject(wire, size, TITLE_MANIFEST_FORMAT);
            memcpy(wire, original, sizeof wire); memset(wire + record + 8, 0, 32); reject(wire, size, TITLE_MANIFEST_PROFILE);
            memcpy(wire, original, sizeof wire); put64(wire + record + 96, 0); reject(wire, size, TITLE_MANIFEST_PROFILE);
            memcpy(wire, original, sizeof wire); put64(wire + record + 104, UINT64_MAX); reject(wire, size, TITLE_MANIFEST_PROFILE);
        }
        memcpy(wire, original, sizeof wire);
        put64(wire + 56, UINT64_MAX - 3); reject(wire, size, TITLE_MANIFEST_PROFILE);
    }
    CHECK(title_manifest_parse(NULL, 0, contract, &manifest, NULL) == TITLE_MANIFEST_ARGUMENT);
    CHECK(title_manifest_parse(wire, sizeof wire, NULL, &manifest, NULL) == TITLE_MANIFEST_ARGUMENT);
    CHECK(title_manifest_parse(wire, sizeof wire, contract, NULL, NULL) == TITLE_MANIFEST_ARGUMENT);
    printf("shared title manifest: %u checks passed\n", checks);
    return 0;
}
