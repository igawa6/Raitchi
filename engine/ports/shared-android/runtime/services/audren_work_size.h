/* SPDX-License-Identifier: GPL-3.0-or-later
 * Portable audio-renderer work-buffer arithmetic; see LICENSE-audren-work-size.txt. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define AUDREN_WORK_PARAMETER_SIZE 52u
enum AudrenWorkSizeResult {
    AUDREN_WORK_SIZE_OK,
    AUDREN_WORK_SIZE_ARGUMENT,
    AUDREN_WORK_SIZE_REVISION,
    AUDREN_WORK_SIZE_PARAMETER,
    AUDREN_WORK_SIZE_OVERFLOW
};
/* Exact 52-byte little-endian AudioRendererParameterInternal prefix. Supports
 * REV1..REV11; no guest pointers or native C++ sizeof dependencies. Signed count
 * fields must be nonnegative and all SDK arithmetic intermediates representable.
 * This computes a contract size, not full renderer configuration validation or
 * an audio-support claim. Fields not affecting sizing (rate, flags, external
 * context) are preserved as irrelevant to this calculation, not validated here.
 * On failure *out_size is zero. Input/output storage must not overlap. */
enum AudrenWorkSizeResult audren_work_size(const void *parameters, size_t length,
                                          uint64_t *out_size);
#ifdef __cplusplus
}
#endif
