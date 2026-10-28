#include "runtime/session_layout.h"
#include "module_abi.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "layout check line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static SessionLayoutSpec example(unsigned kind) {
    SessionLayoutSpec s = {.struct_size = sizeof s, .version = SESSION_LAYOUT_VERSION,
        .module_count = kind == 0 ? 2 : 4, .page_size = 4096,
        .guest_shift = kind == 0 ? UINT64_C(0x6000000000) : 0,
        .canonical_window_lo = UINT64_C(0x7000000000),
        .canonical_window_hi = UINT64_C(0x7320000000),
        .regions = {
            [SESSION_HLE] = {UINT64_C(0x7080000000), 0x100000},
            [SESSION_TLS] = {UINT64_C(0x7090000000), 0x100000},
            [SESSION_HOST_HEAP] = {UINT64_C(0x7092000000), 0xe000000},
            [SESSION_ALIAS] = {UINT64_C(0x7100000000), UINT64_C(0x100000000)},
            [SESSION_STACK] = {UINT64_C(0x7200000000), 0x40000000},
            [SESSION_APP_HEAP] = {UINT64_C(0x7240000000), 0xd0000000}}};
    const uint64_t bases[4] = {UINT64_C(0x7000000000), UINT64_C(0x7004000000),
                              UINT64_C(0x7006000000), UINT64_C(0x7008000000)};
    for (unsigned i = 0; i < s.module_count; ++i)
        s.modules[i] = (SessionModuleSpec){i + 1, 0, {bases[i], 0x2000}};
    if (kind == 2) { /* hypothetical future profile; no title execution claim */
        s.modules[0].image.size = 0x4004000;
        s.modules[1] = (SessionModuleSpec){MODULE_ABI_SUBSDK0, 0, {UINT64_C(0x7008000000), 0x2000000}};
        s.modules[2] = (SessionModuleSpec){MODULE_ABI_SUBSDK1, 0, {UINT64_C(0x700a000000), 0x2000000}};
        s.modules[3] = (SessionModuleSpec){MODULE_ABI_SDK, 0, {UINT64_C(0x700c000000), 0x2000000}};
    }
    return s;
}

static void reject(SessionLayoutSpec s, enum SessionLayoutStatus wanted) {
    SessionLayout output;
    SessionLayoutFailure failure;
    memset(&output, 0xa5, sizeof output);
    CHECK(session_layout_build(&s, &output, &failure) == wanted);
    CHECK(failure.status == wanted);
    const SessionLayout zero = {0};
    CHECK(memcmp(&output, &zero, sizeof output) == 0);
}

typedef struct Selection {
    SessionLayoutSpec spec;
    enum SessionLayoutStatus status;
} Selection;
static void *select_layout(void *p) {
    Selection *selection = p;
    selection->status = session_layout_activate(&selection->spec, NULL);
    return NULL;
}

int main(int argc, char **argv) {
    unsigned kind = 0;
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "letsgo")) kind = 1;
    else if (!strcmp(argv[1], "future")) kind = 2;
    else if (strcmp(argv[1], "las")) return 2;
    CHECK(session_layout_current() == NULL);
    SessionLayoutSpec s = example(kind);
    SessionLayout out;
    SessionLayoutFailure failure;
    CHECK(session_layout_build(&s, &out, &failure) == SESSION_LAYOUT_OK);
    CHECK(failure.first_index == SESSION_LAYOUT_NO_INDEX && failure.second_index == SESSION_LAYOUT_NO_INDEX);
    CHECK(out.module_count == s.module_count && out.guest_shift == s.guest_shift);
    uint64_t actual = 1, canonical = 1;
    CHECK(session_layout_to_actual(&out, s.canonical_window_lo + 4, 8, &actual));
    CHECK(actual == s.canonical_window_lo + 4 - s.guest_shift);
    CHECK(session_layout_to_canonical(&out, actual, 8, &canonical));
    CHECK(canonical == s.canonical_window_lo + 4);
    CHECK(session_layout_to_actual(&out, s.canonical_window_hi - 1, 1, &actual));
    CHECK(!session_layout_to_actual(&out, s.canonical_window_hi - 1, 2, &actual) && actual == 0);
    CHECK(!session_layout_to_actual(&out, s.canonical_window_hi, 1, &actual) && actual == 0);
    CHECK(!session_layout_to_actual(&out, 0, UINT64_MAX, &actual));
    CHECK(!session_layout_to_actual(&out, s.canonical_window_lo, 0, &actual));
    CHECK(!session_layout_to_canonical(&out, out.window.lo - 1, 1, &canonical) && canonical == 0);
    CHECK(!session_layout_to_canonical(&out, out.window.hi - 1, 2, &canonical));
    CHECK(!session_layout_to_actual(NULL, 1, 1, &actual));
    CHECK(!session_layout_to_actual(&out, s.canonical_window_lo, 1, NULL));
    CHECK(!session_range_contains((SessionRange){4, 4}, 4, 1));
    CHECK(session_layout_build(NULL, &out, &failure) == SESSION_LAYOUT_NULL);
    CHECK(session_layout_build(&s, NULL, &failure) == SESSION_LAYOUT_NULL);

    SessionLayoutSpec bad = s;
    ++bad.version; reject(bad, SESSION_LAYOUT_VERSION_MISMATCH);
    bad = s; --bad.struct_size; reject(bad, SESSION_LAYOUT_VERSION_MISMATCH);
    bad = s; bad.page_size = 0; reject(bad, SESSION_LAYOUT_INVALID);
    bad = s; bad.page_size = 6000; reject(bad, SESSION_LAYOUT_INVALID);
    bad = s; bad.page_size = 2048; reject(bad, SESSION_LAYOUT_INVALID);
    bad = s; bad.module_count = 0; reject(bad, SESSION_LAYOUT_INVALID);
    bad = s; bad.module_count = 5; reject(bad, SESSION_LAYOUT_INVALID);
    bad = s; bad.guest_shift = bad.canonical_window_lo; reject(bad, SESSION_LAYOUT_INVALID);
    bad = s; bad.guest_shift += 4; reject(bad, SESSION_LAYOUT_INVALID);
    bad = s; bad.canonical_window_hi = bad.canonical_window_lo; reject(bad, SESSION_LAYOUT_INVALID);
    bad = s; ++bad.canonical_window_hi; reject(bad, SESSION_LAYOUT_INVALID);
    bad = s; bad.regions[SESSION_HLE].size = UINT64_MAX; reject(bad, SESSION_LAYOUT_RANGE);
    bad = s; bad.regions[SESSION_TLS].size = 0; reject(bad, SESSION_LAYOUT_RANGE);
    bad = s; --bad.modules[0].image.canonical_base; reject(bad, SESSION_LAYOUT_RANGE);
    bad = s; bad.modules[0].image.size = 4; reject(bad, SESSION_LAYOUT_RANGE);
    bad = s; bad.modules[0].image.size = UINT64_MAX - 4095; reject(bad, SESSION_LAYOUT_RANGE);
    bad = s; bad.modules[0].module_id = 0; reject(bad, SESSION_LAYOUT_INVALID);
    bad = s; bad.modules[0].module_id = bad.modules[1].module_id; reject(bad, SESSION_LAYOUT_INVALID);
    bad = s; bad.modules[0].reserved = 1; reject(bad, SESSION_LAYOUT_INVALID);
    bad = s; bad.modules[1].image = bad.modules[0].image; reject(bad, SESSION_LAYOUT_OVERLAP);
    bad = s; bad.modules[0].image = bad.regions[SESSION_HLE]; reject(bad, SESSION_LAYOUT_OVERLAP);
    bad = s; bad.regions[SESSION_TLS] = bad.regions[SESSION_HLE]; reject(bad, SESSION_LAYOUT_OVERLAP);
    if (s.module_count < 4) {
        bad = s; bad.modules[3].reserved = 1; reject(bad, SESSION_LAYOUT_INVALID);
    }
    bad = s; bad.modules[0].image = (SessionRegionSpec){UINT64_C(0x7000000000), 0x4000000};
    if (kind == 0 || kind == 1) CHECK(session_layout_build(&bad, &out, NULL) == SESSION_LAYOUT_OK);

    /* Concurrent setup publishes exactly one immutable, copied selection. */
    Selection first = {s, SESSION_LAYOUT_INVALID}, second = {s, SESSION_LAYOUT_INVALID};
    pthread_t a, b;
    CHECK(pthread_create(&a, NULL, select_layout, &first) == 0);
    CHECK(pthread_create(&b, NULL, select_layout, &second) == 0);
    CHECK(pthread_join(a, NULL) == 0 && pthread_join(b, NULL) == 0);
    CHECK((first.status == SESSION_LAYOUT_OK && second.status == SESSION_LAYOUT_ALREADY_ACTIVE) ||
          (second.status == SESSION_LAYOUT_OK && first.status == SESSION_LAYOUT_ALREADY_ACTIVE));
    const SessionLayout *selected = session_layout_current();
    CHECK(selected && selected->guest_shift == s.guest_shift && selected->module_count == s.module_count);
    s.modules[0].image.size = 0;
    CHECK(selected->modules[0].image.hi > selected->modules[0].image.lo);
    s = example(kind);
    CHECK(session_layout_activate(&s, NULL) == SESSION_LAYOUT_ALREADY_ACTIVE);
    printf("session layout %s: %u checks passed\n", argv[1], checks);
    return 0;
}
