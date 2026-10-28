#include "session_layout.h"
#include "../module_abi.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

static SessionLayout active_layout;
static _Atomic int active;
static pthread_mutex_t selection_lock = PTHREAD_MUTEX_INITIALIZER;

static enum SessionLayoutStatus fail(SessionLayoutFailure *out,
    enum SessionLayoutStatus status, uint32_t first, uint32_t second) {
    if (out) *out = (SessionLayoutFailure){status, first, second};
    return status;
}

int session_range_contains(SessionRange range, uint64_t address, uint64_t count) {
    return count && range.lo < range.hi && address >= range.lo &&
           address < range.hi && count <= range.hi - address;
}

static enum SessionLayoutStatus normalize(const SessionLayoutSpec *spec,
    SessionRegionSpec region, SessionRange *out) {
    if (!region.size || (region.canonical_base & (spec->page_size - 1u)) ||
        (region.size & (spec->page_size - 1u)) ||
        !session_range_contains((SessionRange){spec->canonical_window_lo,
                                              spec->canonical_window_hi},
                                region.canonical_base, region.size) ||
        region.canonical_base < spec->guest_shift)
        return SESSION_LAYOUT_RANGE;
    out->lo = region.canonical_base - spec->guest_shift;
    out->hi = out->lo + region.size; /* canonical containment proves no overflow */
    return SESSION_LAYOUT_OK;
}

enum SessionLayoutStatus session_layout_build(const SessionLayoutSpec *spec,
    SessionLayout *out, SessionLayoutFailure *failure) {
    fail(failure, SESSION_LAYOUT_OK, SESSION_LAYOUT_NO_INDEX, SESSION_LAYOUT_NO_INDEX);
    if (out) memset(out, 0, sizeof *out);
    if (!spec || !out)
        return fail(failure, SESSION_LAYOUT_NULL, SESSION_LAYOUT_NO_INDEX, SESSION_LAYOUT_NO_INDEX);
    if (spec->struct_size != sizeof *spec || spec->version != SESSION_LAYOUT_VERSION)
        return fail(failure, SESSION_LAYOUT_VERSION_MISMATCH,
                    SESSION_LAYOUT_NO_INDEX, SESSION_LAYOUT_NO_INDEX);
    uint32_t page = spec->page_size;
    if (!page || (page & (page - 1u)) || page < 4096 ||
        !spec->module_count || spec->module_count > SESSION_LAYOUT_MAX_MODULES ||
        !spec->canonical_window_lo || spec->canonical_window_lo >= spec->canonical_window_hi ||
        spec->canonical_window_lo <= spec->guest_shift ||
        (spec->guest_shift & (page - 1u)) ||
        (spec->canonical_window_lo & (page - 1u)) ||
        (spec->canonical_window_hi & (page - 1u)))
        return fail(failure, SESSION_LAYOUT_INVALID, SESSION_LAYOUT_NO_INDEX, SESSION_LAYOUT_NO_INDEX);

    SessionLayout result = {.struct_size = sizeof result, .version = SESSION_LAYOUT_VERSION,
        .module_count = spec->module_count, .page_size = page, .guest_shift = spec->guest_shift,
        .canonical_window = {spec->canonical_window_lo, spec->canonical_window_hi},
        .window = {spec->canonical_window_lo - spec->guest_shift,
                   spec->canonical_window_hi - spec->guest_shift}};
    SessionRange all[SESSION_REGION_COUNT + SESSION_LAYOUT_MAX_MODULES];
    for (uint32_t i = 0; i < SESSION_REGION_COUNT; ++i) {
        enum SessionLayoutStatus status = normalize(spec, spec->regions[i], &all[i]);
        if (status != SESSION_LAYOUT_OK)
            return fail(failure, status, i, SESSION_LAYOUT_NO_INDEX);
        result.regions[i] = all[i];
    }
    for (uint32_t i = 0; i < spec->module_count; ++i) {
        const SessionModuleSpec *module = &spec->modules[i];
        uint32_t index = SESSION_REGION_COUNT + i;
        if (module->reserved || module->module_id < MODULE_ABI_MAIN ||
            module->module_id > MODULE_ABI_SUBSDK1)
            return fail(failure, SESSION_LAYOUT_INVALID, index, SESSION_LAYOUT_NO_INDEX);
        for (uint32_t j = 0; j < i; ++j)
            if (module->module_id == spec->modules[j].module_id)
                return fail(failure, SESSION_LAYOUT_INVALID, index, SESSION_REGION_COUNT + j);
        enum SessionLayoutStatus status = normalize(spec, module->image, &all[index]);
        if (status != SESSION_LAYOUT_OK)
            return fail(failure, status, index, SESSION_LAYOUT_NO_INDEX);
        result.modules[i] = (SessionModuleRange){module->module_id, 0, all[index]};
    }
    for (uint32_t i = spec->module_count; i < SESSION_LAYOUT_MAX_MODULES; ++i) {
        const SessionModuleSpec *module = &spec->modules[i];
        if (module->module_id || module->reserved || module->image.canonical_base || module->image.size)
            return fail(failure, SESSION_LAYOUT_INVALID, SESSION_REGION_COUNT + i, SESSION_LAYOUT_NO_INDEX);
    }
    for (uint32_t i = 0; i < SESSION_REGION_COUNT + spec->module_count; ++i)
        for (uint32_t j = 0; j < i; ++j)
            if (all[i].lo < all[j].hi && all[j].lo < all[i].hi)
                return fail(failure, SESSION_LAYOUT_OVERLAP, i, j);
    *out = result;
    return SESSION_LAYOUT_OK;
}

enum SessionLayoutStatus session_layout_activate(const SessionLayoutSpec *spec,
    SessionLayoutFailure *failure) {
    SessionLayout candidate;
    enum SessionLayoutStatus status = session_layout_build(spec, &candidate, failure);
    if (status != SESSION_LAYOUT_OK) return status;
    pthread_mutex_lock(&selection_lock);
    if (atomic_load_explicit(&active, memory_order_relaxed)) {
        pthread_mutex_unlock(&selection_lock);
        return fail(failure, SESSION_LAYOUT_ALREADY_ACTIVE,
                    SESSION_LAYOUT_NO_INDEX, SESSION_LAYOUT_NO_INDEX);
    }
    active_layout = candidate;
    atomic_store_explicit(&active, 1, memory_order_release);
    pthread_mutex_unlock(&selection_lock);
    return SESSION_LAYOUT_OK;
}

const SessionLayout *session_layout_current(void) {
    return atomic_load_explicit(&active, memory_order_acquire) ? &active_layout : NULL;
}

static int valid_layout(const SessionLayout *layout) {
    return layout && layout->struct_size == sizeof *layout &&
           layout->version == SESSION_LAYOUT_VERSION &&
           layout->canonical_window.lo < layout->canonical_window.hi &&
           layout->window.lo && layout->canonical_window.lo >= layout->guest_shift &&
           layout->window.lo == layout->canonical_window.lo - layout->guest_shift &&
           layout->window.hi == layout->canonical_window.hi - layout->guest_shift;
}

int session_layout_to_actual(const SessionLayout *layout, uint64_t canonical,
    uint64_t count, uint64_t *actual) {
    if (actual) *actual = 0;
    if (!actual || !valid_layout(layout) ||
        !session_range_contains(layout->canonical_window, canonical, count)) return 0;
    *actual = canonical - layout->guest_shift;
    return 1;
}

int session_layout_to_canonical(const SessionLayout *layout, uint64_t actual,
    uint64_t count, uint64_t *canonical) {
    if (canonical) *canonical = 0;
    if (!canonical || !valid_layout(layout) ||
        !session_range_contains(layout->window, actual, count)) return 0;
    *canonical = actual + layout->guest_shift; /* validated canonical endpoint bounds it */
    return 1;
}

const char *session_layout_status_string(enum SessionLayoutStatus status) {
    switch (status) {
    case SESSION_LAYOUT_OK: return "ok";
    case SESSION_LAYOUT_NULL: return "null layout input/output";
    case SESSION_LAYOUT_VERSION_MISMATCH: return "layout size/version mismatch";
    case SESSION_LAYOUT_INVALID: return "invalid layout policy";
    case SESSION_LAYOUT_RANGE: return "invalid layout range";
    case SESSION_LAYOUT_OVERLAP: return "overlapping layout regions";
    case SESSION_LAYOUT_ALREADY_ACTIVE: return "layout already selected; start a fresh process";
    default: return "unknown layout status";
    }
}
