#include "adapters/title_adapter.h"
#include "runtime/aot_cpu.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "adapter check line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

typedef struct Fixture {
    ModuleAbiProfile profile;
    ModuleAbiModuleSpec modules[4];
} Fixture;

static void fixture(Fixture *out, enum TitleId id) {
    memset(out, 0, sizeof *out);
    const TitleAdapter *adapter = title_adapter_find(id);
    out->profile = (ModuleAbiProfile){.struct_size = sizeof out->profile,
        .module_count = adapter->module_count, .modules = out->modules,
        .identity = {.struct_size = sizeof(ModuleAbiIdentity), .abi_version = MODULE_ABI_VERSION,
            .target = MODULE_ABI_TARGET_ANDROID_ARM64_LP64,
            .cpu_identity = MODULE_ABI_CPU_A64_SHARED_66560, .cpu_size = sizeof(Cpu),
            .cpu_alignment = _Alignof(Cpu), .guest_shift = adapter->guest_shift,
            .guest_window_lo = adapter->canonical_window_lo - adapter->guest_shift,
            .guest_window_hi = adapter->canonical_window_hi - adapter->guest_shift}};
    for (uint32_t i = 0; i < adapter->module_count; ++i) {
        out->modules[i] = (ModuleAbiModuleSpec){.struct_size = sizeof out->modules[i],
            .module_id = adapter->modules[i].module_id,
            .canonical_base = adapter->modules[i].canonical_base,
            .mapped_size = 0x8000, .text_offset = 0, .text_size = 0x4000, .function_count = 4};
        out->modules[i].nso_build_id[0] = (uint8_t)(i + 1);
        out->modules[i].nso_sha256[0] = (uint8_t)(i + 17);
    }
    memcpy(out->modules[0].nso_build_id, adapter->main_build_prefix, adapter->main_build_prefix_size);
    /* Remaining build-ID/hash bytes are deliberately synthetic. The adapter's
     * public prefix recognition does not pretend to verify these as game files. */
    out->modules[0].nso_build_id[31] = 0xe3;
}

static void reject(enum TitleId id, Fixture *f, uint32_t caps, uint32_t page,
    enum TitleAdapterStatus want) {
    SessionLayoutSpec out, zero = {0};
    TitleAdapterFailure failure;
    memset(&out, 0xa5, sizeof out);
    CHECK(title_adapter_build_layout(id, &f->profile, caps, page, &out, &failure) == want);
    CHECK(failure.status == want);
    CHECK(!memcmp(&out, &zero, sizeof out));
    CHECK(title_adapter_selected() == NULL);
    CHECK(session_layout_current() == NULL);
}

static void pure_tests(enum TitleId id) {
    const TitleAdapter *adapter = title_adapter_find(id);
    Fixture f;
    fixture(&f, id);
    SessionLayoutSpec out;
    SessionLayout normalized;
    TitleAdapterFailure failure;
    CHECK(adapter != NULL && adapter->id == id);
    CHECK(adapter->program_id != 0 && adapter->key && adapter->save_namespace);
    CHECK(adapter->startup.argument_count == 4 && adapter->startup.late_count == 1);
    CHECK(adapter->startup.late_modules[0] == MODULE_ABI_MAIN);
    CHECK(adapter->startup.early_count == adapter->module_count - 1);
    CHECK(adapter->startup.early_modules[0] == MODULE_ABI_SDK);
    if (id == TITLE_EEVEE) {
        CHECK(adapter->startup.early_modules[1] == MODULE_ABI_SUBSDK0);
        CHECK(adapter->startup.early_modules[2] == MODULE_ABI_SUBSDK1);
    }
    ModuleAbiProfile original = f.profile;
    ModuleAbiModuleSpec original_modules[4];
    memcpy(original_modules, f.modules, sizeof f.modules);
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t page = i ? 16384 : 4096;
        CHECK(title_adapter_build_layout(id, &f.profile, adapter->capabilities, page, &out, &failure) == TITLE_ADAPTER_OK);
        CHECK(failure.status == TITLE_ADAPTER_OK && failure.module_index == MODULE_ABI_NO_MODULE);
        CHECK(out.page_size == page && out.module_count == adapter->module_count);
        CHECK(out.guest_shift == adapter->guest_shift);
        CHECK(!memcmp(out.regions, adapter->regions, sizeof out.regions));
        CHECK(session_layout_build(&out, &normalized, NULL) == SESSION_LAYOUT_OK);
        CHECK(normalized.window.lo == (id == TITLE_LAS ? UINT64_C(0x1000000000) : UINT64_C(0x7000000000)));
        for (uint32_t j = 0; j < adapter->module_count; ++j) {
            CHECK(out.modules[j].module_id == f.modules[j].module_id);
            CHECK(out.modules[j].image.canonical_base == f.modules[j].canonical_base);
            CHECK(out.modules[j].image.size == f.modules[j].mapped_size);
            CHECK(normalized.modules[j].image.lo == f.modules[j].canonical_base - adapter->guest_shift);
        }
        CHECK(!memcmp(&f.profile, &original, sizeof original));
        CHECK(!memcmp(f.modules, original_modules, sizeof original_modules));
    }
    reject(id, &f, adapter->capabilities ^ AOT_CAP_PROFILING, 4096, TITLE_ADAPTER_CAPABILITIES);
    reject(id, &f, adapter->capabilities, 8193, TITLE_ADAPTER_LAYOUT);
    reject(id, &f, adapter->capabilities, 2048, TITLE_ADAPTER_LAYOUT);
    reject((enum TitleId)77, &f, adapter->capabilities, 4096, TITLE_ADAPTER_UNKNOWN);
    reject(id == TITLE_LAS ? TITLE_EEVEE : TITLE_LAS, &f, adapter->capabilities, 4096, TITLE_ADAPTER_CAPABILITIES);
    f.profile.identity.cpu_identity = MODULE_ABI_CPU_A64_CACHE_66560;
    reject(id, &f, adapter->capabilities, 4096, TITLE_ADAPTER_PROFILE);
    fixture(&f, id); f.modules[0].canonical_base += 0x100000;
    reject(id, &f, adapter->capabilities, 4096, TITLE_ADAPTER_PROFILE);
    fixture(&f, id); f.modules[0].nso_build_id[0] ^= 1;
    reject(id, &f, adapter->capabilities, 4096, TITLE_ADAPTER_BUILD);
    fixture(&f, id); memset(f.modules[0].nso_sha256, 0, 32);
    reject(id, &f, adapter->capabilities, 4096, TITLE_ADAPTER_PROFILE);
    fixture(&f, id); f.modules[0].text_size = 0;
    reject(id, &f, adapter->capabilities, 4096, TITLE_ADAPTER_PROFILE);
    fixture(&f, id); f.modules[0].mapped_size = 0x9000;
    reject(id, &f, adapter->capabilities, 16384, TITLE_ADAPTER_LAYOUT);
    fixture(&f, id); f.modules[1].canonical_base = f.modules[0].canonical_base;
    reject(id, &f, adapter->capabilities, 4096, TITLE_ADAPTER_PROFILE);
    /* An oversized SDK reaches the fixed HLE range while module-module and
     * profile checks still pass: session layout must catch the system overlap. */
    fixture(&f, id);
    f.modules[adapter->module_count - 1].mapped_size = UINT64_C(0x90000000);
    reject(id, &f, adapter->capabilities, 4096, TITLE_ADAPTER_LAYOUT);
    fixture(&f, id); f.profile.module_count = 3;
    reject(id, &f, adapter->capabilities, 4096, TITLE_ADAPTER_PROFILE);
    fixture(&f, id); f.profile.identity.guest_shift += 4096;
    reject(id, &f, adapter->capabilities, 4096, TITLE_ADAPTER_PROFILE);
    CHECK(title_adapter_build_layout(id, NULL, adapter->capabilities, 4096, &out, &failure) == TITLE_ADAPTER_ARGUMENT);
    CHECK(title_adapter_build_layout(id, &f.profile, adapter->capabilities, 4096, NULL, &failure) == TITLE_ADAPTER_ARGUMENT);
    CHECK(!title_adapter_find((enum TitleId)0));
    CHECK(title_adapter_selected() == NULL && session_layout_current() == NULL);
}

typedef struct SelectThread { pthread_barrier_t *barrier; enum TitleId id; enum TitleAdapterStatus status; } SelectThread;
static void *select_thread(void *argument) {
    SelectThread *thread = argument;
    pthread_barrier_wait(thread->barrier);
    thread->status = title_adapter_select(thread->id);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    enum TitleId selected = !strcmp(argv[1], "las") ? TITLE_LAS : TITLE_EEVEE;
    int concurrent = !strcmp(argv[1], "concurrent");
    if (!concurrent && strcmp(argv[1], "las") && strcmp(argv[1], "eevee")) return 2;
    pure_tests(TITLE_LAS);
    pure_tests(TITLE_EEVEE);
    CHECK(title_adapter_select((enum TitleId)0) == TITLE_ADAPTER_UNKNOWN);
    CHECK(title_adapter_selected() == NULL);
    if (concurrent) {
        pthread_barrier_t barrier;
        pthread_t threads[4];
        SelectThread calls[4];
        CHECK(!pthread_barrier_init(&barrier, NULL, 4));
        for (unsigned i = 0; i < 4; ++i) {
            calls[i] = (SelectThread){&barrier, i & 1 ? TITLE_LAS : TITLE_EEVEE, TITLE_ADAPTER_UNKNOWN};
            CHECK(!pthread_create(&threads[i], NULL, select_thread, &calls[i]));
        }
        unsigned winners = 0;
        for (unsigned i = 0; i < 4; ++i) {
            CHECK(!pthread_join(threads[i], NULL));
            CHECK(calls[i].status == TITLE_ADAPTER_OK || calls[i].status == TITLE_ADAPTER_ALREADY_SELECTED);
            if (calls[i].status == TITLE_ADAPTER_OK) { ++winners; selected = calls[i].id; }
        }
        CHECK(winners == 1);
        CHECK(!pthread_barrier_destroy(&barrier));
    } else CHECK(title_adapter_select(selected) == TITLE_ADAPTER_OK);
    CHECK(title_adapter_selected() == title_adapter_find(selected));
    CHECK(title_adapter_select(selected) == TITLE_ADAPTER_ALREADY_SELECTED);
    CHECK(title_adapter_select(selected == TITLE_LAS ? TITLE_EEVEE : TITLE_LAS) == TITLE_ADAPTER_ALREADY_SELECTED);
    CHECK(title_adapter_selected() == title_adapter_find(selected));
    CHECK(session_layout_current() == NULL); /* Selection is metadata-only. */
    printf("title adapters %s: %u checks passed\n", argv[1], checks);
    return 0;
}
