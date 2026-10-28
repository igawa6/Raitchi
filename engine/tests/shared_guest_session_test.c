/* API-contract tests with controlled loader/kernel stand-ins. These do NOT
 * establish production KThread, SVC, guest TLS, or game execution behavior.
 * Link with ports/shared-android/runtime/guest_session.c, -pthread, and include
 * native/, ports/shared-android/runtime/, sources/las-android/runtime/.
 * Run separate fresh processes with arguments: lifecycle, identity, no-session.
 */
#include "guest_session.h"
#include "kernel.h"
#include "runtime/aot_dispatch.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "guest session contract line %d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)

pthread_mutex_t g_klock = PTHREAD_MUTEX_INITIALIZER;
Module g_modules[4];
int g_nmodules;
static KThread fixture_thread;
static __thread KThread *fixture_current;
static __thread uint64_t caller_marker;
static atomic_int worker_gate;
static unsigned loader_calls, kernel_calls, stop_calls;
static bool session_available = true, identity_matches = true;
static TitleManifest manifest = {.title_id = TITLE_LAS, .profile = {.module_count = 1}};
static const TitleAdapter adapter = {.id = TITLE_LAS, .module_count = 1,
    .modules = {{.name = "fixture"}}};
static const SessionLayout layout;
static const ModuleAbiDescriptor descriptor;

const NativeManifest *shared_native_manifest_current(void) { return NULL; }
const TitleManifest *shared_manifest_current(void) { return session_available ? &manifest : NULL; }
bool shared_session_ready(void) { return session_available; }
const TitleAdapter *title_adapter_selected(void) { return &adapter; }
const ModuleAbiDescriptor *shared_gamelib_descriptor(void) { return &descriptor; }
const SessionLayout *session_layout_current(void) { return &layout; }
int aot_dispatch_is_bound(void) { return 1; }
bool shared_sha256_file(const char *path, uint8_t digest[32]) {
    CHECK(path && strstr(path, "/fixture"));
    memset(digest, identity_matches ? 0 : 1, 32);
    return true;
}
void loader_load(const char *directory) { CHECK(directory && *directory == '/'); ++loader_calls; }
void kernel_init(void) { ++kernel_calls; }
KThread *kernel_current_thread(void) { return fixture_current; }
Cpu *kernel_current_cpu(void) { return fixture_current ? &fixture_current->cpu : NULL; }
KObject *handle_get(uint32_t handle) {
    /* The public API must reject pseudo handles before reaching this stand-in. */
    CHECK(handle >= 0x101 && handle < 0x1100);
    return handle == fixture_thread.handle ? &fixture_thread.obj : NULL;
}
bool kernel_threads_all_finished(void) {
    pthread_mutex_lock(&g_klock);
    bool finished = !fixture_thread.started || fixture_thread.finished;
    pthread_mutex_unlock(&g_klock);
    return finished;
}
void kernel_stop_threads(void (*idle)(void)) {
    CHECK(!idle && kernel_threads_all_finished());
    CHECK(!fixture_thread.started || fixture_thread.joined);
    ++stop_calls;
}
static SharedGuestThreadSnapshot blank_snapshot(void) {
    return (SharedGuestThreadSnapshot){.struct_size = sizeof(SharedGuestThreadSnapshot),
        .version = SHARED_GUEST_API_VERSION};
}
static void *worker(void *unused) {
    (void)unused;
    fixture_current = &fixture_thread;
    caller_marker = 0x1234;
    SharedGuestThreadSnapshot snapshot = blank_snapshot();
    CHECK(shared_guest_current(&fixture_thread.cpu, &snapshot) == SHARED_GUEST_OK);
    CHECK((snapshot.flags & (SHARED_GUEST_THREAD_CURRENT | SHARED_GUEST_THREAD_CPU_STATE_VALID)) ==
        (SHARED_GUEST_THREAD_CURRENT | SHARED_GUEST_THREAD_CPU_STATE_VALID));
    CHECK(snapshot.guest_sp == 0x12345000 && snapshot.guest_tpidr == 0x76543000);
    Cpu *other = calloc(1, sizeof *other);
    CHECK(other);
    other->thread = &fixture_thread;
    CHECK(shared_guest_current(other, &snapshot) == SHARED_GUEST_CURRENT_MISMATCH);
    free(other);
    CHECK(shared_guest_join_finished(fixture_thread.handle, &snapshot) == SHARED_GUEST_IN_PROGRESS);
    CHECK(caller_marker == 0x1234);
    atomic_store(&worker_gate, 1);
    while (atomic_load(&worker_gate) == 1) usleep(1000);
    pthread_mutex_lock(&g_klock);
    fixture_thread.finished = true;
    fixture_thread.obj.signaled = true;
    pthread_mutex_unlock(&g_klock);
    fixture_current = NULL;
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    SharedGuestThreadSnapshot snapshot = blank_snapshot();
    char why[128];
    CHECK(shared_guest_query(0x101, &snapshot) == SHARED_GUEST_NOT_READY);
    CHECK(shared_guest_prepare(NULL, why, sizeof why) == SHARED_GUEST_ARGUMENT && *why);
    if (!strcmp(argv[1], "no-session")) {
        session_available = false;
        CHECK(shared_guest_prepare(".", why, sizeof why) == SHARED_GUEST_NO_SESSION);
        CHECK(!loader_calls && !kernel_calls);
        session_available = true;
        CHECK(shared_guest_prepare(".", why, sizeof why) == SHARED_GUEST_OK);
        CHECK(shared_guest_shutdown() == SHARED_GUEST_OK && stop_calls == 1);
    } else if (!strcmp(argv[1], "identity")) {
        identity_matches = false;
        CHECK(shared_guest_prepare(".", why, sizeof why) == SHARED_GUEST_INPUT);
        CHECK(!loader_calls && !kernel_calls);
        identity_matches = true;
        CHECK(shared_guest_prepare(".", why, sizeof why) == SHARED_GUEST_ALREADY_USED);
        CHECK(shared_guest_shutdown() == SHARED_GUEST_NOT_READY && !stop_calls);
    } else if (!strcmp(argv[1], "lifecycle")) {
        CHECK(shared_guest_prepare(".", why, sizeof why) == SHARED_GUEST_OK);
        CHECK(loader_calls == 1 && kernel_calls == 1 && !*why);
        CHECK(shared_guest_prepare(".", why, sizeof why) == SHARED_GUEST_ALREADY_USED);
        CHECK(shared_guest_query(0xffff8000u, &snapshot) == SHARED_GUEST_INVALID_HANDLE);
        CHECK(shared_guest_query(0x1100, &snapshot) == SHARED_GUEST_INVALID_HANDLE);
        --snapshot.version;
        CHECK(shared_guest_query(0x101, &snapshot) == SHARED_GUEST_ARGUMENT);
        snapshot = blank_snapshot();
        fixture_thread = (KThread){.obj = {.type = KOBJ_THREAD}, .handle = 0x101,
            .entry = 0x44440000, .tlr = 0x55550000, .id = 17, .prio = 44, .core = 2};
        fixture_thread.cpu.sp = 0x12345000;
        fixture_thread.cpu.tpidr = 0x76543000;
        fixture_thread.cpu.thread = &fixture_thread;
        CHECK(shared_guest_query(0x101, &snapshot) == SHARED_GUEST_OK);
        CHECK(snapshot.flags == SHARED_GUEST_THREAD_CPU_STATE_VALID);
        CHECK(snapshot.thread_id == 17 && snapshot.tls_base == 0x55550000 && snapshot.core == 2);
        CHECK(snapshot.entry == 0x44440000 && snapshot.cpu_address == (uintptr_t)&fixture_thread.cpu);
        CHECK(shared_guest_join_finished(0x101, &snapshot) == SHARED_GUEST_IN_PROGRESS);
        CHECK(shared_guest_current(&fixture_thread.cpu, &snapshot) == SHARED_GUEST_CURRENT_MISMATCH);
        caller_marker = 0x99887766;
        fixture_thread.started = true;
        CHECK(!pthread_create(&fixture_thread.host, NULL, worker, NULL));
        while (atomic_load(&worker_gate) != 1) usleep(1000);
        CHECK(shared_guest_query(0x101, &snapshot) == SHARED_GUEST_OK);
        CHECK(snapshot.flags == SHARED_GUEST_THREAD_STARTED);
        CHECK(!snapshot.guest_sp && !snapshot.guest_tpidr && snapshot.priority == -1);
        CHECK(shared_guest_join_finished(0x101, &snapshot) == SHARED_GUEST_IN_PROGRESS);
        CHECK(shared_guest_shutdown() == SHARED_GUEST_IN_PROGRESS && !stop_calls);
        atomic_store(&worker_gate, 2);
        do {
            usleep(1000);
            CHECK(shared_guest_query(0x101, &snapshot) == SHARED_GUEST_OK);
        } while (!(snapshot.flags & SHARED_GUEST_THREAD_FINISHED));
        CHECK(shared_guest_join_finished(0x101, &snapshot) == SHARED_GUEST_OK);
        CHECK(snapshot.flags == (SHARED_GUEST_THREAD_STARTED | SHARED_GUEST_THREAD_FINISHED |
            SHARED_GUEST_THREAD_SIGNALED | SHARED_GUEST_THREAD_JOINED | SHARED_GUEST_THREAD_CPU_STATE_VALID));
        CHECK(shared_guest_join_finished(0x101, &snapshot) == SHARED_GUEST_OK);
        CHECK(caller_marker == 0x99887766 && !fixture_current);
        CHECK(shared_guest_shutdown() == SHARED_GUEST_OK && stop_calls == 1);
        CHECK(shared_guest_shutdown() == SHARED_GUEST_NOT_READY && stop_calls == 1);
        CHECK(shared_guest_query(0x101, &snapshot) == SHARED_GUEST_NOT_READY);
    } else return 2;
    puts("headless session API contract passed; controlled stand-ins only");
    return 0;
}
