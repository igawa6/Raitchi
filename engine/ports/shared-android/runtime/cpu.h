/* Runtime and generated producers share the CPU; only producers fix a shift. */
#pragma once
#include "runtime/aot_cpu.h"
#include "runtime/session_layout.h"
#include <stdlib.h>

static inline const SessionLayout *shared_required_layout(void) {
    const SessionLayout *layout = session_layout_current();
    if (!layout) abort();
    return layout;
}
static inline uint64_t shared_runtime_address(uint64_t canonical) {
    uint64_t shift = shared_required_layout()->guest_shift;
    if (canonical < shift) abort();
    return canonical - shift;
}
#define GUEST_SHIFT (shared_required_layout()->guest_shift)
#define GUEST(a) shared_runtime_address((uint64_t)(a))
