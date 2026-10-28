/* CPU staging for declared immutable compiler constants (NVN c1). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct NvnConstantBufferPlan {
    uint64_t source_address, source_size, staging_size;
} NvnConstantBufferPlan;
/* Descriptor extent and readable source extent are independent. The caller
 * guarantees the declared source is readable; this checks arithmetic only.
 * An empty declaration never reads code memory, even with a nonzero shader
 * extent. Staging is at least 16 bytes and capped by the device range. */
bool nvn_constant_buffer_plan(uint64_t code_address, uint64_t constant_offset,
    uint64_t declared_size, uint64_t shader_extent, uint64_t max_uniform_range,
    NvnConstantBufferPlan *out);
/* Copy only the plan's source bytes, zero the remaining staging extent. Reject
 * short/overflowing/overlapping destinations before changing any byte. */
bool nvn_constant_buffer_copy(const NvnConstantBufferPlan *plan, void *destination,
    uint64_t destination_size);
