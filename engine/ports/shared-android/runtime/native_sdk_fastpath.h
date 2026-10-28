/* Optional native-session SDK acceleration; no title addresses or game code. */
#pragma once
#include "runtime/aot_cpu.h"
enum NativeSdkRelocationStatus {
    NATIVE_SDK_RELOCATION_OK, NATIVE_SDK_RELOCATION_ARGUMENT,
    NATIVE_SDK_RELOCATION_RANGE, NATIVE_SDK_RELOCATION_OVERFLOW,
    NATIVE_SDK_RELOCATION_METADATA, NATIVE_SDK_RELOCATION_NO_MEMORY
};
typedef struct NativeSdkRelocationResult {
    uint64_t sections, entries, groups, pointers, fixed_pointers, null_pointers;
} NativeSdkRelocationResult;
typedef struct NativeSdkRelocationBounds {
    /* Optional production APP_HEAP range whose read-only pages are protected by
     * the normal dirty-write signal handler. No other read-only writes allowed. */
    uint64_t tracked_write_lo, tracked_write_hi;
} NativeSdkRelocationBounds;
/* Complete preflight precedes writes. Metadata and pointer fields may be
 * unaligned. Destination fields must not overlap relocation metadata or flags.
 * Inputs are trusted, caller-owned and quiescent; /proc bounds establish mapped
 * memory, not malloc object sizes or a sandbox. Original pointer additions wrap
 * modulo64, preserve nulls, and repeat on a second call (not idempotent).
 * Failure leaves all resource bytes unchanged; counts describe only success. */
enum NativeSdkRelocationStatus native_sdk_relocate_table(uint64_t table, NativeSdkRelocationResult *result);
enum NativeSdkRelocationStatus native_sdk_relocate_table_ex(uint64_t table,
    const NativeSdkRelocationBounds *bounds, NativeSdkRelocationResult *result);
/* Explicit env opt-in + exact SDK symbol only; no global HLE registration.
 * NATIVE_SDK_RELOCATION_STANDALONE excludes this production-only wrapper when
 * compiling the same algorithm into an independent diagnostic helper. */
GuestFn native_sdk_fastpath(const char *symbol);
