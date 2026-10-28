#ifndef SHARED_RECOMP_SESSION_LAYOUT_H
#define SHARED_RECOMP_SESSION_LAYOUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SESSION_LAYOUT_VERSION 1u
#define SESSION_LAYOUT_MAX_MODULES 4u
#define SESSION_LAYOUT_NO_INDEX UINT32_MAX
#define SESSION_LAYOUT_API __attribute__((visibility("default")))

enum SessionRegionId {
    SESSION_HLE, SESSION_TLS, SESSION_HOST_HEAP, SESSION_ALIAS,
    SESSION_STACK, SESSION_APP_HEAP, SESSION_REGION_COUNT
};

typedef struct SessionRegionSpec {
    uint64_t canonical_base;
    uint64_t size;
} SessionRegionSpec;

typedef struct SessionModuleSpec {
    uint32_t module_id; /* ModuleAbiModuleId; order is selected independently */
    uint32_t reserved;
    SessionRegionSpec image;
} SessionModuleSpec;

typedef struct SessionLayoutSpec {
    uint32_t struct_size;
    uint32_t version;
    uint32_t module_count;
    uint32_t page_size;
    uint64_t guest_shift;
    uint64_t canonical_window_lo;
    uint64_t canonical_window_hi;
    SessionRegionSpec regions[SESSION_REGION_COUNT];
    SessionModuleSpec modules[SESSION_LAYOUT_MAX_MODULES];
} SessionLayoutSpec;

typedef struct SessionRange { uint64_t lo, hi; } SessionRange; /* half-open */
typedef struct SessionModuleRange {
    uint32_t module_id;
    uint32_t reserved;
    SessionRange image;
} SessionModuleRange;
typedef struct SessionLayout {
    uint32_t struct_size;
    uint32_t version;
    uint32_t module_count;
    uint32_t page_size;
    uint64_t guest_shift;
    SessionRange canonical_window;
    SessionRange window;
    SessionRange regions[SESSION_REGION_COUNT];
    SessionModuleRange modules[SESSION_LAYOUT_MAX_MODULES];
} SessionLayout;

enum SessionLayoutStatus {
    SESSION_LAYOUT_OK, SESSION_LAYOUT_NULL, SESSION_LAYOUT_VERSION_MISMATCH,
    SESSION_LAYOUT_INVALID, SESSION_LAYOUT_RANGE, SESSION_LAYOUT_OVERLAP,
    SESSION_LAYOUT_ALREADY_ACTIVE
};
typedef struct SessionLayoutFailure {
    enum SessionLayoutStatus status;
    uint32_t first_index;  /* regions first, then modules; NO_INDEX for header */
    uint32_t second_index;
} SessionLayoutFailure;

/* Pure validation/normalization; output is cleared on failure. Does not reserve
 * memory. Requires readable, immutable input. Input/output must not alias. */
SESSION_LAYOUT_API enum SessionLayoutStatus session_layout_build(
    const SessionLayoutSpec *spec, SessionLayout *out, SessionLayoutFailure *failure);

/* Setup only, before guest threads. Copies the normalized layout into one
 * immutable process-wide instance; no reset or second activation is available.
 * Selection does not establish successful address reservation or module loading. */
SESSION_LAYOUT_API enum SessionLayoutStatus session_layout_activate(
    const SessionLayoutSpec *spec, SessionLayoutFailure *failure);
SESSION_LAYOUT_API const SessionLayout *session_layout_current(void);

/* Count must be nonzero; [address,address+count) must stay inside the window.
 * Results are cleared on failure. Translation does not make memory accessible. */
SESSION_LAYOUT_API int session_layout_to_actual(const SessionLayout *layout,
    uint64_t canonical, uint64_t count, uint64_t *actual);
SESSION_LAYOUT_API int session_layout_to_canonical(const SessionLayout *layout,
    uint64_t actual, uint64_t count, uint64_t *canonical);
SESSION_LAYOUT_API int session_range_contains(SessionRange range, uint64_t address, uint64_t count);
SESSION_LAYOUT_API const char *session_layout_status_string(enum SessionLayoutStatus status);


#ifdef __cplusplus
}
#endif
#endif
