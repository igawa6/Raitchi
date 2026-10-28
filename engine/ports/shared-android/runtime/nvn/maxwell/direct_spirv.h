/* Direct Maxwell-to-SPIR-V. Unsupported programs retain the legacy compiler. */
#pragma once
#include "shader.h"

typedef enum {
    MAXWELL_SPIRV_OK = 0,
    MAXWELL_SPIRV_UNSUPPORTED,
    MAXWELL_SPIRV_INVALID,
    MAXWELL_SPIRV_OOM
} MaxwellSpirvStatus;

typedef struct {
    uint32_t *words;
    size_t word_count;
    ShaderInfo info; /* metadata only: msl stays NULL on direct success */
    size_t instruction_offset;
    char reason[160];
} MaxwellSpirvOutput;

MaxwellSpirvStatus maxwell_direct_spirv(const uint8_t *blob, size_t code_bytes,
                                       size_t constant_bytes, int stage,
                                       const ShaderKey *key, MaxwellSpirvOutput *out);
void maxwell_direct_spirv_free(MaxwellSpirvOutput *out);
