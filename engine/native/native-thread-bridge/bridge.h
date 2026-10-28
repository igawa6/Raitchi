/* Isolated native entry on genuine production KThreads; optional reviewed SDK thunk. */
#ifndef NATIVE_THREAD_BRIDGE_H
#define NATIVE_THREAD_BRIDGE_H
#include "runtime/aot_cpu.h"
#include "guest_session.h"
#include "module_abi.h"
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#define NT_BLOCKS 8u
#define NT_MAX_SITES (NT_BLOCKS * 4u + 2u)
typedef void (*NtSvc)(Cpu *, uint32_t, uint64_t);
typedef enum SharedGuestStatus (*NtCurrent)(const Cpu *, SharedGuestThreadSnapshot *);
typedef struct __attribute__((aligned(16))) NtFrame {
    uint64_t x[31], sp, nzcv, fpcr, fpsr;
    __uint128_t q[32];
} NtFrame;
typedef struct NtSite { uint64_t pc; uint32_t kind, svc; } NtSite;
enum { NT_SITE_SVC, NT_SITE_TLS_RO, NT_SITE_SDK };
#define NT_SDK_INPUT_VERSION 1u
typedef struct NtSdkInput {
    uint32_t struct_size, version;
    uint32_t words[3], reserved; /* supplied privately: wrapper B; thunk SVC10; RET */
    int64_t original_delta;
    uint64_t aot_address;
} NtSdkInput;
typedef struct NtContext {
    Cpu *cpu, *seed, *before, *opaque;
    NtSvc svc;
    NtCurrent current;
    void *entry;
    void (*aot_fn)(Cpu *);
    void (*call)(struct NtContext *, NtFrame *, const NtSite *);
    uint64_t expected_sp, return_sp, original_guest_sp, host_tls, host_x18;
    uint64_t host_sp, host_call_sp, active_frame, host_stack_lo, host_stack_hi;
    uint64_t guest_stack_lo, guest_stack_hi, guest_entry_sp, stack_cookie;
    uint64_t stack_checks, stack_failures, native_stack_canary_checks, entry_rejections;
    uint64_t expected_id, expected_tlr, marker, marker_seen, core_seen, id_rc, id_seen, tick_seen;
    uint64_t sdk_lr, sdk_saved_lr, sdk_seen, sdk_resume_seen, sdk_returns;
    uint64_t sdk_calls, sdk_lr_checks, sdk_lr_failures, sdk_resume_failures, aot_calls, aot_seen, aot_failures;
    uint32_t expected_handle, expected_core, exit_mode, sdk_enabled;
    uint32_t stack_enabled, entry_depth, gateway_active, reserved;
    _Atomic uint32_t entered, returned, failed, *barrier;
    uint64_t transitions, tls_reads, state_failures, identity_failures, opaque_failures, identity_reasons;
    uint64_t exit_entered, exit_returned, returned_native;
    NtSite sites[NT_MAX_SITES];
    size_t site_count;
} NtContext;

typedef struct NtTotals {
    uint64_t state_failures, identity_failures, opaque_failures, transitions, tls_reads, identity_reasons;
    uint64_t exit_entered, exit_returned;
    uint64_t sdk_calls, sdk_returns, sdk_lr_checks, sdk_lr_failures, sdk_resume_failures, aot_calls, aot_failures;
    uint64_t stack_checks, stack_failures, native_stack_canary_checks, entry_rejections;
    int known;
} NtTotals;
/* Callback counters are non-atomic: only aggregate after both actual joins. */
NtTotals nt_totals(const NtContext *, unsigned joined);

enum NtIdentityReason {
    NT_ID_API=1u, NT_ID_FLAGS=2u, NT_ID_HANDLE=4u, NT_ID_THREAD=8u,
    NT_ID_TLS_BASE=16u, NT_ID_TPIDR_RW=32u, NT_ID_TPIDR_RO=64u,
    NT_ID_CPU=128u, NT_ID_CORE=256u
};
uint32_t nt_identity_reason(const NtContext *, enum SharedGuestStatus, const SharedGuestThreadSnapshot *);

ModuleAbiGuestFn nt_sdk_descriptor_entry(const ModuleAbiDescriptor *, uint64_t);
int nt_sdk_input_valid(const NtSdkInput *);
int nt_stack_transition_good(const NtContext *, const NtFrame *, uintptr_t, int);
int nt_stack_canaries_good(const NtContext *);
int nt_frame_good(const NtContext *, const NtFrame *, const NtSite *);
int nt_sdk_resume_good(const NtContext *);
int nt_patch_branch(uint32_t original, uintptr_t pc, uintptr_t target, uint32_t *out);
int nt_site_allowed(const NtSite *);
void nt_materialize(Cpu *, const NtFrame *);
void nt_restore(NtFrame *, const Cpu *);
int nt_opaque_equal(const Cpu *, const Cpu *);
void nt_call(NtContext *, NtFrame *, const NtSite *);
void nt_observe(NtContext *, NtFrame *);
void nt_enter(NtContext *);
extern const unsigned char nt_resume[], nt_island_begin[], nt_island_end[];
extern const unsigned char nt_island_site[], nt_island_return[];
__attribute__((visibility("default"))) int native_thread_bridge_probe(
    const char *, const char *, const char *, const char *, char *, size_t, const NtSdkInput *, uint32_t);
#endif
