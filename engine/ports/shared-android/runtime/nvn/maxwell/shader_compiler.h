/* Renderer-independent direct compiler selection and exact legacy fallback. */
#pragma once
#include "shader.h"

typedef struct {
    uint64_t requests, direct_attempts, direct, fallback;
    uint64_t unsupported, invalid, oom, ineligible, disabled, failures;
} ShaderCompilerStageStats;
typedef struct {
    ShaderCompilerStageStats stage[6];
    uint64_t invalid_arguments;
} ShaderCompilerStats;

/* Inputs retain maxwell_translate_sized's borrowed-buffer lifetime contract.
 * Direct compilation additionally requires finite, bounded code/constants spans.
 * The returned object owns either source or SPIR-V; release with maxwell_free. */
ShaderInfo *maxwell_compile_sized(const uint8_t *code, size_t code_bytes,
                                 size_t constant_bytes, int stage, const ShaderKey *key);
bool shader_info_succeeded(const ShaderInfo *info);
/* Versioned mode identity. Include this alongside full compiler-source and
 * capability/environment fingerprints when caching compiler output. */
uint32_t shader_compiler_policy(void);
/* Relaxed diagnostic snapshot; counters can advance while it is collected. */
void shader_compiler_stats(ShaderCompilerStats *out);
