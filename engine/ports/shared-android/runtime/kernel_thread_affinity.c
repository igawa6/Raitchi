// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
/* Logical-affinity adaptation from Eden c6541721fa6307e99b3fdd9517dd4ed1b73927ed:
 * core/hle/kernel/svc/svc_thread.cpp and k_thread.cpp. Upstream notices are
 * GPL-3.0-or-later (Eden), GPL-2.0-or-later (yuzu); this adaptation GPL3-or-later.
 * Retains guest validation/sentinel behavior, deliberately has no host scheduler
 * integration. See LICENSE-kernel-thread-affinity.txt for the full license. */
#include "kernel_thread_affinity.h"
#include <stdbool.h>

static bool core_valid(int32_t core) { return core >= 0 && core < 4; }
static bool mask_valid(uint64_t mask) { return mask && !(mask & ~KERNEL_GUEST_CORE_MASK); }
static bool process_valid(int32_t core, uint64_t allowed) {
    return core_valid(core) && mask_valid(allowed) && (allowed & (UINT64_C(1) << core));
}
uint32_t kernel_thread_affinity_create(KernelThreadAffinity *out, int32_t create_core,
                                       int32_t process_default, uint64_t allowed_mask) {
    if (!out || !process_valid(process_default, allowed_mask)) return KERNEL_AFFINITY_INVALID_CORE;
    if (create_core == KERNEL_IDEAL_PROCESS_DEFAULT) create_core = process_default;
    if (!core_valid(create_core) || !(allowed_mask & (UINT64_C(1) << create_core)))
        return KERNEL_AFFINITY_INVALID_CORE;
    KernelThreadAffinity next = {create_core, create_core, UINT64_C(1) << create_core};
    *out = next;
    return 0;
}
uint32_t kernel_thread_affinity_prepare(KernelThreadAffinityRequest *out, int32_t ideal,
                                        uint64_t mask, int32_t process_default, uint64_t allowed_mask) {
    if (!out || !process_valid(process_default, allowed_mask)) return KERNEL_AFFINITY_INVALID_CORE;
    if (ideal == KERNEL_IDEAL_PROCESS_DEFAULT) {
        ideal = process_default;
        mask = UINT64_C(1) << ideal;
    } else {
        if (mask & ~allowed_mask) return KERNEL_AFFINITY_INVALID_CORE;
        if (!mask) return KERNEL_AFFINITY_INVALID_COMBINATION;
        if (core_valid(ideal)) {
            if (!(mask & (UINT64_C(1) << ideal))) return KERNEL_AFFINITY_INVALID_COMBINATION;
        } else if (ideal != KERNEL_IDEAL_NO_UPDATE && ideal != KERNEL_IDEAL_DONT_CARE) {
            return KERNEL_AFFINITY_INVALID_CORE;
        }
    }
    KernelThreadAffinityRequest next = {ideal, mask};
    *out = next;
    return 0;
}
uint32_t kernel_thread_affinity_apply(KernelThreadAffinity *state,
                                      const KernelThreadAffinityRequest *request) {
    if (!state || !request || !mask_valid(state->mask) || !core_valid(state->current) ||
        !(state->mask & (UINT64_C(1) << state->current)) || !mask_valid(request->mask))
        return KERNEL_AFFINITY_INVALID_COMBINATION;
    int32_t ideal = request->ideal;
    if (ideal == KERNEL_IDEAL_NO_UPDATE) ideal = state->ideal;
    if (core_valid(ideal)) {
        if (!(request->mask & (UINT64_C(1) << ideal))) return KERNEL_AFFINITY_INVALID_COMBINATION;
    } else if (request->ideal == KERNEL_IDEAL_NO_UPDATE || ideal != KERNEL_IDEAL_DONT_CARE) {
        return KERNEL_AFFINITY_INVALID_COMBINATION;
    }
    int32_t current = state->current;
    // There is no guest scheduler to migrate toward an updated preference later.
    // Choose an explicit permitted ideal immediately; DontCare/NoUpdate keep a
    // still-permitted current processor. This is deterministic logical policy,
    // not a host-thread placement request or exact scheduling-timing emulation.
    if (core_valid(request->ideal)) current = ideal;
    else if (!(request->mask & (UINT64_C(1) << current))) {
        current = ideal;
        if (current < 0) {
            for (current = 3; current >= 0; --current)
                if (request->mask & (UINT64_C(1) << current)) break;
        }
    }
    KernelThreadAffinity next = {ideal, current, request->mask};
    *state = next;
    return 0;
}
