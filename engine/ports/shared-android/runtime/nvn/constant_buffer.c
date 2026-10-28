#include "constant_buffer.h"
#include <stddef.h>
#include <string.h>

bool nvn_constant_buffer_plan(uint64_t code_address, uint64_t constant_offset,
    uint64_t declared_size, uint64_t shader_extent, uint64_t max_uniform_range,
    NvnConstantBufferPlan *out) {
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (max_uniform_range < 16) return false;
    uint64_t staging = declared_size > shader_extent ? declared_size : shader_extent;
    if (staging < 16) staging = 16;
    if (staging > max_uniform_range) staging = max_uniform_range;
    if (staging > SIZE_MAX) return false;
    uint64_t source = 0, bytes = 0;
    if (declared_size) {
        if (!code_address || code_address > UINTPTR_MAX || constant_offset > UINTPTR_MAX-code_address)
            return false;
        source = code_address + constant_offset;
        if (declared_size > UINTPTR_MAX-source) return false;
        bytes = declared_size < staging ? declared_size : staging;
    }
    *out = (NvnConstantBufferPlan){source, bytes, staging};
    return true;
}

bool nvn_constant_buffer_copy(const NvnConstantBufferPlan *p, void *destination,
    uint64_t destination_size) {
    if (!p || !destination || p->staging_size < 16 || p->staging_size > SIZE_MAX ||
        p->source_size > p->staging_size || p->staging_size > destination_size ||
        p->staging_size > UINTPTR_MAX-(uintptr_t)destination) return false;
    if (p->source_size && (!p->source_address || p->source_address > UINTPTR_MAX ||
        p->source_size > UINTPTR_MAX-p->source_address)) return false;
    if (p->source_size && (uintptr_t)destination < p->source_address+p->source_size &&
        p->source_address < (uintptr_t)destination+p->staging_size) return false;
    // Plan can live inside the output. Retain its validated values before the
    // copy/zero touches that storage; source/destination overlap is rejected.
    uint64_t source = p->source_address, bytes = p->source_size, staging = p->staging_size;
    if (bytes) memcpy(destination, (const void *)(uintptr_t)source, (size_t)bytes);
    memset((uint8_t *)destination+(size_t)bytes, 0, (size_t)(staging-bytes));
    return true;
}
