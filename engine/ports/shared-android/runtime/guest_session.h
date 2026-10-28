/* Controlled headless embedding seam. Normal game startup does not call it. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "runtime/aot_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SHARED_GUEST_API_VERSION 1u
#define SHARED_GUEST_API __attribute__((visibility("default")))

enum SharedGuestStatus {
    SHARED_GUEST_OK = 0,
    SHARED_GUEST_ARGUMENT,
    SHARED_GUEST_NO_SESSION,
    SHARED_GUEST_ALREADY_USED,
    SHARED_GUEST_NOT_READY,
    SHARED_GUEST_INPUT,
    SHARED_GUEST_INVALID_HANDLE,
    SHARED_GUEST_IN_PROGRESS,
    SHARED_GUEST_HOST_ERROR,
    SHARED_GUEST_CURRENT_MISMATCH
};
enum SharedGuestThreadFlags {
    SHARED_GUEST_THREAD_STARTED = 1u << 0,
    SHARED_GUEST_THREAD_FINISHED = 1u << 1,
    SHARED_GUEST_THREAD_SIGNALED = 1u << 2,
    SHARED_GUEST_THREAD_CURRENT = 1u << 3,
    SHARED_GUEST_THREAD_JOINED = 1u << 4,
    SHARED_GUEST_THREAD_CPU_STATE_VALID = 1u << 5
};
typedef struct SharedGuestThreadSnapshot {
    uint32_t struct_size, version, flags, handle;
    int32_t core, priority;
    uint64_t thread_id, tls_base, guest_sp, guest_tpidr, cpu_address, entry;
} SharedGuestThreadSnapshot;

/* Requires prior successful shared_session_prepare(root,lib,sidecar,...).
 * Verifies/loads original NSOs and initializes the real kernel exactly once.
 * No UI, GPU, save setup, original constructors, SDK startup or game entry.
 * All sessions and callbacks require a dedicated fresh process. */
SHARED_GUEST_API enum SharedGuestStatus shared_guest_prepare(
    const char *exefs_directory, char *why, size_t capacity);

/* Caller initializes struct_size/version. Live foreign mutable fields are
 * omitted: priority/guest_sp/guest_tpidr require CPU_STATE_VALID. */
SHARED_GUEST_API enum SharedGuestStatus shared_guest_query(
    uint32_t handle, SharedGuestThreadSnapshot *snapshot);
/* Verifies the actual thread-local KThread/Cpu binding, not Cpu.thread alone. */
SHARED_GUEST_API enum SharedGuestStatus shared_guest_current(
    const Cpu *expected_cpu, SharedGuestThreadSnapshot *snapshot);

/* Never waits for a live guest. After FINISHED, joins the real host thread and
 * prevents another join during shutdown. Keep callback code/context alive
 * until this succeeds. No thread-handle reuse after joining is supported. */
SHARED_GUEST_API enum SharedGuestStatus shared_guest_join_finished(
    uint32_t handle, SharedGuestThreadSnapshot *snapshot);
/* Rejects while any actual kernel thread is live; stops/joins once, no reset
 * or address-space reuse. A helper still needs an outer process deadline. */
SHARED_GUEST_API enum SharedGuestStatus shared_guest_shutdown(void);
SHARED_GUEST_API const char *shared_guest_status_string(enum SharedGuestStatus);

#ifdef __cplusplus
}
#endif
