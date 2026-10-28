/* Production native setup and crash text, synthetic original NSOs and a
 * controlled platform reservation. No guest, renderer or Android execution. */
#define _GNU_SOURCE
#include "../ports/shared-android/runtime/gamelib.c"
#include "runtime/native_prepare.h"
#include <sys/stat.h>

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "native session line %d: %s\n", __LINE__, #value); abort(); \
} } while (0)

static const TitleAdapter *fixture_adapter;
static unsigned reservations, bindings;
static int reservation_error;
static bool reservation_ok;

bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    ++reservations;
    CHECK(title_adapter_selected() == fixture_adapter);
    CHECK(lo == fixture_adapter->canonical_window_lo - fixture_adapter->guest_shift);
    CHECK(hi == fixture_adapter->canonical_window_hi - fixture_adapter->guest_shift);
    CHECK(!shared_session_ready() && !bindings);
    errno = reservation_error;
    return reservation_ok;
}

enum AotStatus aot_dispatch_bind_native(const NativeManifest *prepared,
    const uint8_t expected[32], uint32_t capabilities, NativeManifestFailure *failure) {
    ++bindings;
    CHECK(reservations == 1 && reservation_ok);
    CHECK(prepared->title_id == (uint32_t)fixture_adapter->id);
    CHECK(capabilities == fixture_adapter->capabilities);
    CHECK(native_manifest_validate(prepared, expected, failure) == NATIVE_MANIFEST_OK);
    return AOT_OK;
}

bool plat_module_name(uint64_t pc, char *out, size_t size) {
    (void)pc; (void)out; (void)size;
    return false;
}
const char *rt_host_fn_name(uint64_t pc, uint64_t *offset) {
    (void)pc; (void)offset;
    return NULL;
}
extern void rt_crash_explain(char *out, size_t size, const char *log_path);
extern int rt_romfs_files;

static void put32(uint8_t *bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (uint8_t)(value >> (i * 8));
}
static void original_fixture(const char *root) {
    char directory[4096];
    snprintf(directory, sizeof directory, "%s/exefs", root);
    CHECK(!mkdir(directory, 0700));
    for (unsigned i = 0; i < fixture_adapter->module_count; ++i) {
        uint8_t bytes[512] = {0};
        memcpy(bytes, "NSO0", 4);
        unsigned count = fixture_adapter->module_build_prefix_size[i];
        const uint8_t *prefix = fixture_adapter->module_build_prefix[i];
        if (!count && fixture_adapter->modules[i].module_id == MODULE_ABI_MAIN) {
            count = fixture_adapter->main_build_prefix_size;
            prefix = fixture_adapter->main_build_prefix;
        }
        memcpy(bytes + 0x40, prefix, count);
        bytes[0x5f] = (uint8_t)(i + 1);
        for (unsigned segment = 0; segment < 3; ++segment) {
            put32(bytes + 0x10 + 16 * segment, 0x100 + 0x20 * segment);
            put32(bytes + 0x14 + 16 * segment, 0x1000 * segment);
            put32(bytes + 0x18 + 16 * segment, 16);
            put32(bytes + 0x60 + 4 * segment, 16);
        }
        put32(bytes + 0x3c, 32);
        char path[4096];
        snprintf(path, sizeof path, "%s/exefs/%s", root, fixture_adapter->modules[i].name);
        FILE *file = fopen(path, "wb");
        CHECK(file && fwrite(bytes, 1, sizeof bytes, file) == sizeof bytes);
        CHECK(!fclose(file));
    }
}

int main(int argc, char **argv) {
    CHECK(argc == 4);
    fixture_adapter = title_adapter_find(!strcmp(argv[2], "eevee") ? TITLE_EEVEE : TITLE_LAS);
    const char *test = argv[3];
    char exefs[4096], path[4096], why[512];
    snprintf(exefs, sizeof exefs, "%s/exefs", argv[1]);
    snprintf(path, sizeof path, "%s/native.manifest", argv[1]);
    original_fixture(argv[1]);
    uint8_t contract[32];
    decode_contract(SHARED_NATIVE_CONTRACT_SHA256_HEX, contract);
    NativePrepareResult result;
    CHECK(native_prepare_exefs(fixture_adapter->key, exefs, path, contract,
                               shared_sha256_bytes, &result, why, sizeof why));
    CHECK(!setenv("LARECOMP_CPU_BACKEND", "continuous-nce", 1));
    CHECK(!setenv("LARECOMP_TITLE", fixture_adapter->key, 1));
    if (!strcmp(test, "selected")) {
        CHECK(title_adapter_select(fixture_adapter->id) == TITLE_ADAPTER_OK);
    } else if (!strcmp(test, "active")) {
        uint8_t wire[NATIVE_MANIFEST_MAX_SIZE];
        FILE *file = fopen(path, "rb"); CHECK(file);
        size_t size = fread(wire, 1, sizeof wire, file); CHECK(!fclose(file));
        NativeManifest prepared; NativeManifestFailure failure;
        CHECK(native_manifest_parse(wire, size, contract, &prepared, &failure) == NATIVE_MANIFEST_OK);
        SessionLayoutSpec layout;
        CHECK(title_adapter_build_native_layout(fixture_adapter->id, &prepared, contract,
              (uint32_t)sysconf(_SC_PAGESIZE), &layout, NULL) == TITLE_ADAPTER_OK);
        CHECK(session_layout_activate(&layout, NULL) == SESSION_LAYOUT_OK);
    } else if (!strcmp(test, "collision")) reservation_error = EEXIST;
    else if (!strcmp(test, "memory")) reservation_error = ENOMEM;
    else if (!strcmp(test, "hint")) reservation_error = 0;
    else { CHECK(!strcmp(test, "ok") || !strcmp(test, "files")); reservation_ok = true; }

    bool ok = native_session_prepare(argv[1], path, why, sizeof why);
    CHECK(ok == reservation_ok);
    CHECK(bindings == (ok ? 1u : 0u));
    CHECK(shared_session_ready() == ok);
    if (!ok) {
        CHECK(strstr(why, fixture_adapter->key));
        char expected[128];
        if (!strcmp(test, "selected")) {
            CHECK(!reservations && strstr(why, "native title selection failed"));
        } else if (!strcmp(test, "active")) {
            CHECK(!reservations && strstr(why, "native layout activation failed"));
        } else {
            CHECK(reservations == 1 && strstr(why, "native guest reservation failed"));
            snprintf(expected, sizeof expected, "[0x%llx,0x%llx)",
                (unsigned long long)(fixture_adapter->canonical_window_lo - fixture_adapter->guest_shift),
                (unsigned long long)(fixture_adapter->canonical_window_hi - fixture_adapter->guest_shift));
            CHECK(strstr(why, expected));
            snprintf(expected, sizeof expected, "errno %d", reservation_error);
            CHECK(strstr(why, expected));
            if (!reservation_error) CHECK(strstr(why, "exact address unavailable"));
        }
        /* Check the actual player-facing explanation, including title/error
         * details, even when the generic missing-files heuristic would apply. */
        rt_fatal_msg("%s\n", why);
        rt_romfs_files = 0;
        char notice[1024]; rt_crash_explain(notice, sizeof notice, NULL);
        CHECK(strstr(notice, why) && !strstr(notice, argv[1]));
        if (reservations) CHECK(strstr(notice, "memory layout is unavailable"));
        else CHECK(strstr(notice, "game session could not start"));
    }
    if (ok && !strcmp(test, "files")) {
        rt_romfs_files = 11;
        rt_fatal_msg("controlled graphics recording failure\n");
        char notice[1024]; rt_crash_explain(notice, sizeof notice, NULL);
        if (fixture_adapter->id == TITLE_LAS) {
            CHECK(strstr(notice, "game files are incomplete") && strstr(notice, "3836"));
        } else {
            CHECK(!strstr(notice, "game files are incomplete") && !strstr(notice, "3836"));
            CHECK(strstr(notice, "stopped because of an error"));
        }
    }
    CHECK(!native_session_prepare(argv[1], path, why, sizeof why));
    CHECK(strstr(why, "fresh process"));
    CHECK(bindings == (ok ? 1u : 0u));
    printf("%s/%s: production native setup, terminal gate and player notice passed\n", fixture_adapter->key, test);
    return 0;
}
