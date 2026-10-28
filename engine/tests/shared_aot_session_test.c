#include "runtime/shared_aot.h"

#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#ifndef FIXTURE_MODULE_COUNT
#error "select fixture profile"
#endif
#define CANONICAL UINT64_C(0x5000000000)
#define SHIFT (FIXTURE_MODULE_COUNT == 2 ? UINT64_C(0x2000000000) : UINT64_C(0))
#define BASE (CANONICAL - SHIFT)

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "session check line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

typedef struct OpenThread {
    const SharedAotRequest *request;
    pthread_barrier_t *barrier;
    enum SharedAotStatus status;
    const ModuleAbiDescriptor *descriptor;
    SharedAotFailure failure;
} OpenThread;

static void *concurrent_open(void *argument) {
    OpenThread *thread = argument;
    pthread_barrier_wait(thread->barrier);
    thread->status = shared_aot_open(thread->request, &thread->descriptor, &thread->failure);
    return NULL;
}
static unsigned reservation_calls;
static int reserve_fixture(const SessionLayout *layout, void *context) {
    ++reservation_calls;
    CHECK(layout == session_layout_current());
    CHECK(layout->window.lo == BASE && layout->guest_shift == SHIFT);
    CHECK(!aot_dispatch_is_bound());
    return *(const int *)context;
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    ModuleAbiModuleSpec modules[4] = {0};
    for (unsigned i = 0; i < FIXTURE_MODULE_COUNT; ++i) {
        modules[i] = (ModuleAbiModuleSpec){.struct_size = sizeof modules[i], .module_id = i + 1,
            .canonical_base = CANONICAL + UINT64_C(0x100000) * i,
            .mapped_size = 0x40000, .text_offset = 0, .text_size = 0x20000,
            .function_count = i ? 1 : 7};
        modules[i].nso_build_id[0] = (uint8_t)(i + 1);
        modules[i].nso_sha256[0] = (uint8_t)(i + 17);
    }
    ModuleAbiProfile profile = {.struct_size = sizeof profile, .module_count = FIXTURE_MODULE_COUNT,
        .identity = {.struct_size = sizeof(ModuleAbiIdentity), .abi_version = MODULE_ABI_VERSION,
            .target = MODULE_ABI_TARGET_ANDROID_ARM64_LP64,
            .cpu_identity = MODULE_ABI_CPU_A64_SHARED_66560, .cpu_size = sizeof(Cpu),
            .cpu_alignment = _Alignof(Cpu), .guest_shift = SHIFT,
            .guest_window_lo = BASE, .guest_window_hi = BASE + 0x1000000}, .modules = modules};
    SessionLayoutSpec layout = {.struct_size = sizeof layout, .version = SESSION_LAYOUT_VERSION,
        .module_count = FIXTURE_MODULE_COUNT, .page_size = 4096, .guest_shift = SHIFT,
        .canonical_window_lo = CANONICAL, .canonical_window_hi = CANONICAL + 0x1000000};
    for (unsigned i = 0; i < SESSION_REGION_COUNT; ++i)
        layout.regions[i] = (SessionRegionSpec){CANONICAL + 0x800000 + (uint64_t)i * 0x10000, 0x10000};
    for (unsigned i = 0; i < FIXTURE_MODULE_COUNT; ++i)
        layout.modules[i] = (SessionModuleSpec){i + 1, 0, {modules[i].canonical_base, modules[i].mapped_size}};
    SharedAotRequest request = {.struct_size = sizeof request, .version = SHARED_AOT_REQUEST_VERSION,
        .artifact_capabilities = AOT_PROFILE_CAPABILITIES, .required_capabilities = AOT_PROFILE_CAPABILITIES,
        .library_path = argv[1], .expected = &profile, .layout = &layout};
    const ModuleAbiDescriptor *descriptor = NULL;
    SharedAotFailure failure;
    const char *test = argv[2];
    ModuleAbiModuleSpec original_modules[4];
    memcpy(original_modules, modules, sizeof modules);
    const ModuleAbiProfile original_profile = profile;
    const SessionLayoutSpec original_layout = layout;
    const SharedAotRequest original_request = request;
    if (!strcmp(test, "concurrent")) {
        pthread_barrier_t barrier;
        pthread_t threads[4];
        OpenThread calls[4] = {0};
        CHECK(!pthread_barrier_init(&barrier, NULL, 4));
        for (unsigned i = 0; i < 4; ++i) {
            calls[i].request = &request;
            calls[i].barrier = &barrier;
            CHECK(!pthread_create(&threads[i], NULL, concurrent_open, &calls[i]));
        }
        unsigned successes = 0;
        for (unsigned i = 0; i < 4; ++i) {
            CHECK(!pthread_join(threads[i], NULL));
            CHECK(calls[i].status == SHARED_AOT_OK || calls[i].status == SHARED_AOT_IN_PROGRESS ||
                  calls[i].status == SHARED_AOT_FRESH_PROCESS_REQUIRED);
            CHECK(calls[i].failure.status == calls[i].status);
            CHECK((calls[i].descriptor != NULL) == (calls[i].status == SHARED_AOT_OK));
            successes += calls[i].status == SHARED_AOT_OK;
        }
        CHECK(successes == 1 && aot_dispatch_is_bound());
        CHECK(session_layout_current() != NULL);
        CHECK(shared_aot_open(&request, &descriptor, &failure) == SHARED_AOT_FRESH_PROCESS_REQUIRED);
        CHECK(!pthread_barrier_destroy(&barrier));
        printf("shared AOT session %u modules concurrent: %u checks passed\n", FIXTURE_MODULE_COUNT, checks);
        return 0;
    }
    enum SharedAotStatus expected = SHARED_AOT_OK;
    int preflight = 0;
    int reservation = -1;
    if (!strcmp(test, "layout")) { layout.regions[SESSION_TLS] = layout.regions[SESSION_HLE]; expected = SHARED_AOT_LAYOUT; preflight = 1; }
    else if (!strcmp(test, "profile")) { ++profile.identity.guest_shift; expected = SHARED_AOT_PROFILE; preflight = 1; }
    else if (!strcmp(test, "caps")) { request.artifact_capabilities ^= AOT_CAP_PROFILING; expected = SHARED_AOT_PROFILE; preflight = 1; }
    else if (!strcmp(test, "legacy")) { profile.identity.cpu_identity = MODULE_ABI_CPU_A64_CACHE_66560; expected = SHARED_AOT_PROFILE; preflight = 1; }
    else if (!strcmp(test, "mapping")) { layout.modules[0].image.size += 4096; expected = SHARED_AOT_PROFILE; preflight = 1; }
    else if (!strcmp(test, "profile-text")) { modules[0].text_size = 0; expected = SHARED_AOT_PROFILE; preflight = 1; }
    else if (!strcmp(test, "profile-digest")) { memset(modules[0].nso_sha256, 0, 32); expected = SHARED_AOT_PROFILE; preflight = 1; }
    else if (!strcmp(test, "profile-order")) {
        ModuleAbiModuleSpec swap = modules[0]; modules[0] = modules[1]; modules[1] = swap;
        SessionModuleSpec range = layout.modules[0]; layout.modules[0] = layout.modules[1]; layout.modules[1] = range;
        expected = SHARED_AOT_PROFILE; preflight = 1;
    }
    else if (!strcmp(test, "metadata")) { modules[0].nso_sha256[1] = 7; expected = SHARED_AOT_LOAD; }
    else if (!strcmp(test, "missing")) { request.library_path = "/no/such/shared-aot-library.so"; expected = SHARED_AOT_LOAD; }
    else if (!strcmp(test, "reserve-ok")) { reservation = 1; }
    else if (!strcmp(test, "reserve-fail")) { reservation = 0; expected = SHARED_AOT_RESERVATION; }
    else if (strcmp(test, "execution")) return 2;

    CHECK((reservation < 0 ? shared_aot_open(&request, &descriptor, &failure) :
        shared_aot_open_with_reservation(&request, reserve_fixture, &reservation, &descriptor, &failure)) == expected);
    CHECK(reservation_calls == (reservation < 0 ? 0u : 1u));
    CHECK(failure.status == expected);
    CHECK((descriptor != NULL) == (expected == SHARED_AOT_OK));
    CHECK(aot_dispatch_is_bound() == (expected == SHARED_AOT_OK));
    CHECK((session_layout_current() != NULL) == !preflight);
    if (expected != SHARED_AOT_OK) {
        CHECK(!aot_dispatch_is_bound() && descriptor == NULL);
        if (!preflight) {
            CHECK(shared_aot_open(&request, &descriptor, &failure) == SHARED_AOT_FRESH_PROCESS_REQUIRED);
        } else {
            memcpy(modules, original_modules, sizeof modules);
            profile = original_profile;
            layout = original_layout;
            request = original_request;
            CHECK(shared_aot_open(&request, &descriptor, &failure) == SHARED_AOT_OK);
            CHECK(descriptor != NULL && aot_dispatch_is_bound());
            CHECK(session_layout_current() != NULL);
        }
    } else {
        const SessionLayout *selected = session_layout_current();
        CHECK(selected->guest_shift == SHIFT && selected->module_count == FIXTURE_MODULE_COUNT);
        uint64_t actual;
        CHECK(session_layout_to_actual(selected, CANONICAL + 0x100, 4, &actual));
        Cpu *cpu = aligned_alloc(16, sizeof *cpu);
        CHECK(cpu != NULL);
        aot_cpu_initialize(cpu);
        CHECK(aot_dispatch_attach_cpu(cpu) == AOT_OK);
        cpu->pc = actual; rt_dispatch(cpu);
        CHECK(cpu->x[0] == 1 && cpu->x[10] == 1);
        cpu->pc = actual; rt_dispatch(cpu);
        CHECK(cpu->x[0] == 2 && cpu->x[10] == 2);
        CHECK(aot_dispatch_detach_cpu(cpu) == AOT_OK);
        free(cpu);
        CHECK(shared_aot_open(&request, &descriptor, &failure) == SHARED_AOT_FRESH_PROCESS_REQUIRED);
        CHECK(descriptor == NULL);
    }
    printf("shared AOT session %u modules %s: %u checks passed\n", FIXTURE_MODULE_COUNT, test, checks);
    return 0;
}
