/* shared-aot-v1 synthetic execution seam; trusted local metadata, not a sandbox. */
#ifndef SHARED_RECOMP_AOT_DISPATCH_H
#define SHARED_RECOMP_AOT_DISPATCH_H
#include "aot_cpu.h"
#include "../module_abi.h"
#include "native_manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AOT_MAX_CPUS 256u
#define AOT_MAX_DISPATCH_OVERRIDES 4096u
#define AOT_MAX_HLE_FUNCTIONS 4096u

enum AotStatus {
    AOT_OK = 0, AOT_ARGUMENT, AOT_ALREADY_BOUND, AOT_NOT_BOUND,
    AOT_ABI_MISMATCH, AOT_CPU_MISMATCH, AOT_CAPABILITY_MISMATCH,
    AOT_CAPACITY, AOT_NOT_FOUND
};

/* One successful bind per process, no reset/unload. Descriptor and arrays must remain
 * immutable/live. expected and required_caps originate independently from trusted
 * runtime profile/sidecar checks. This seam does not itself inspect ELF/files. */
AOT_API enum AotStatus aot_dispatch_bind(const ModuleAbiDescriptor *descriptor,
    const ModuleAbiProfile *expected, uint32_t artifact_caps, uint32_t required_caps,
    ModuleAbiFailure *failure);
/* Genuine original-image registry; mutually exclusive with AOT binding.
 * Manifest must remain immutable/live for the entire process, and must have
 * passed independent original-file hash and title-policy checks before entry.
 * NativeManifest validation is repeated here against the caller's expected
 * runtime contract. caps is the title policy's declared host-hook capability
 * mask (known AOT_CAP_* bits), not a generated-artifact capability claim.
 * No descriptor, generated addresses/functions or title library is required.
 * dispatch_lookup stays NULL; bind an execution backend before guest entry.
 * Setup is serialized while all guest execution is quiescent, as for AOT. */
AOT_API enum AotStatus aot_dispatch_bind_native(const NativeManifest *manifest,
    const uint8_t expected_contract_sha256[32], uint32_t caps,
    NativeManifestFailure *failure);
AOT_API uint32_t aot_dispatch_capabilities(void);
AOT_API int aot_dispatch_is_bound(void);
AOT_API GuestFn dispatch_lookup(uint64_t actual_address);
AOT_API int aot_dispatch_contains_text(uint64_t actual_address);
AOT_API unsigned aot_dispatch_cache_slot(uint64_t actual_address);
/* Optional original-code backend. Configure once while quiescent, before any
 * guest execution. Host lookup never returns a generated guest function. */
AOT_API enum AotStatus aot_dispatch_set_execution_backend(GuestFn backend);
AOT_API GuestFn aot_dispatch_execution_backend(void);
/* Indexed host-only lookup: HLE before overrides, live atomic HLE replacements,
 * no negative caching. Addresses use entry indices rather than a zero sentinel.
 * Original-call bypass remains the execution backend caller's responsibility. */
AOT_API GuestFn aot_dispatch_host_lookup(uint64_t actual_address);
/* Quiescent read-only inventory for ahead-of-execution entry patches. */
AOT_API size_t aot_dispatch_override_count(void);
AOT_API int aot_dispatch_override_address(size_t index, uint64_t *address);

/* Configuration (including hook/profiler registration) is serialized by the caller
 * while ALL guest execution is quiescent. CPUs attach before their first execution
 * and detach after their last; independent thread lifecycle operations are serialized
 * internally, while the Cpu being attached/detached must not be running. This lets overrides invalidate every live Cpu cache,
 * without adding epoch/TLS checks to LAS's hot dispatch path. */
AOT_API enum AotStatus aot_dispatch_attach_cpu(Cpu *cpu);
AOT_API enum AotStatus aot_dispatch_detach_cpu(Cpu *cpu);
AOT_API void aot_cpu_initialize(Cpu *cpu); /* clears the entire frozen Cpu */
AOT_API enum AotStatus aot_dispatch_override(uint64_t actual_address, GuestFn function);
/* Exact HLE addresses, outside all mapped guest modules; never cached. HLE and
 * overrides precede generated lookup. Concurrent registration is supported after
 * binding for runtime proc-address requests; published addresses remain immutable
 * and function replacements are atomic. No title-specific HLE base in this core. */
AOT_API enum AotStatus aot_dispatch_add_hle(uint64_t actual_address, GuestFn function);
/* Optional OS cancellation/safe-point check before every HLE call. Configure
 * while quiescent, before starting guest threads; the direct-call/cache fast path
 * does not consult it. A guard may unwind its current guest thread. */
AOT_API enum AotStatus aot_dispatch_set_hle_guard(GuestFn guard);
AOT_API void guest_call(Cpu *cpu, uint64_t actual_address);
AOT_API const char *aot_status_string(enum AotStatus status);

#ifdef __cplusplus
}
#endif
#endif
