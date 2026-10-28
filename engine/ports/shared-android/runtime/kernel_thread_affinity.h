/* SPDX-License-Identifier: GPL-3.0-or-later
 * Logical guest affinity only; license: LICENSE-kernel-thread-affinity.txt. */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define KERNEL_GUEST_CORE_MASK UINT64_C(0xf)
#define KERNEL_IDEAL_DONT_CARE (-1)
#define KERNEL_IDEAL_PROCESS_DEFAULT (-2)
#define KERNEL_IDEAL_NO_UPDATE (-3)
#define KERNEL_AFFINITY_INVALID_CORE UINT32_C(0xe201)
#define KERNEL_AFFINITY_INVALID_COMBINATION UINT32_C(0xe801)
typedef struct {
    int32_t ideal, current;
    uint64_t mask;
} KernelThreadAffinity;
typedef struct {
    int32_t ideal;
    uint64_t mask;
} KernelThreadAffinityRequest;
/* Caller serializes all access with the kernel thread-state lock. No host
 * scheduler, affinity, priority, wakeup or performance operation occurs here.
 * allowed_mask is the existing emulated process policy, not inferred NPDM caps.
 * Failed operations leave output/state unchanged. */
uint32_t kernel_thread_affinity_create(KernelThreadAffinity *out, int32_t create_core,
                                       int32_t process_default, uint64_t allowed_mask);
/* Independent SVC parameter validation before handle lookup. -2 overrides the
 * supplied mask with the configured default singleton. -1 remains the reported
 * ideal (not a chosen core); -3 defers preservation of the previous ideal. */
uint32_t kernel_thread_affinity_prepare(KernelThreadAffinityRequest *out, int32_t ideal,
                                        uint64_t mask, int32_t process_default, uint64_t allowed_mask);
/* Apply a successfully prepared request. Explicit ideals select the logical
 * current processor immediately (no deferred guest scheduler exists). DontCare
 * and NoUpdate retain a permitted current core; when excluded, choose the retained
 * concrete ideal or highest permitted core. NoUpdate requires that old ideal in
 * the new mask, failing explicitly instead of shifting by a negative core ID. */
uint32_t kernel_thread_affinity_apply(KernelThreadAffinity *state,
                                      const KernelThreadAffinityRequest *request);
#ifdef __cplusplus
}
#endif
