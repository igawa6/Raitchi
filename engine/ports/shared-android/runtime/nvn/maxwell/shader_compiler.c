#include "shader_compiler.h"
#include "direct_spirv.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COMPILER_DIRECT_MAX_BYTES (16u * 1024u * 1024u)
#define COMPILER_DIRECT_MAX_WORDS (16u * 1024u * 1024u)
typedef struct {
    _Atomic uint64_t requests, direct_attempts, direct, fallback;
    _Atomic uint64_t unsupported, invalid, oom, ineligible, disabled, failures;
} CompilerCounters;
static CompilerCounters counters[6];
static _Atomic uint64_t invalid_arguments;
#define COUNT(c, field) atomic_fetch_add_explicit(&(c)->field, 1, memory_order_relaxed)

static bool direct_enabled(void) {
    const char *value = getenv("LARECOMP_VK_DIRECT_SPIRV");
    return value && !strcmp(value, "1");
}
uint32_t shader_compiler_policy(void) {
    return UINT32_C(0x53500100) | (direct_enabled() ? 1u : 0u);
}
bool shader_info_succeeded(const ShaderInfo *info) {
    return info && (info->msl || (info->spirv && info->spirv_words >= 5 &&
                                 info->spirv[0] == UINT32_C(0x07230203)));
}
static bool direct_eligible(const uint8_t *code, size_t bytes, size_t constants,
                            int stage, const ShaderKey *key) {
    if (!key || !key->glsl || bytes == SIZE_MAX || constants == SIZE_MAX ||
        bytes > COMPILER_DIRECT_MAX_BYTES || constants > COMPILER_DIRECT_MAX_BYTES - bytes)
        return false;
    if ((stage == STAGE_FRAGMENT && code == maxwell_dbg_code) ||
        (stage == STAGE_VERTEX && code == maxwell_dbg_vs)) return false;
    // These options alter shaderc/post-processing semantics. Honor them through
    // the existing implementation until direct equivalents are validated.
    if (getenv("LARECOMP_VK_OPT")) return false;
    const char *option = getenv("LARECOMP_VK_OPT_FS");
    if (stage != STAGE_VERTEX && option && atoi(option)) return false;
    option = getenv("LARECOMP_VK_NO_CONTRACT");
    if (stage == STAGE_VERTEX && option && !atoi(option)) return false;
    return true;
}
static bool direct_output_valid(const MaxwellSpirvOutput *out, int stage) {
    return out->words && out->word_count >= 5 && out->word_count <= COMPILER_DIRECT_MAX_WORDS &&
        out->words[0] == UINT32_C(0x07230203) && out->words[3] && !out->words[4] &&
        out->info.stage == stage && !out->info.msl && !out->info.spirv && !out->info.error[0];
}
ShaderInfo *maxwell_compile_sized(const uint8_t *code, size_t bytes, size_t constants,
                                 int stage, const ShaderKey *key) {
    if (!code || stage < STAGE_VERTEX || stage > STAGE_COMPUTE ||
        (constants != SIZE_MAX && bytes > SIZE_MAX - constants)) {
        atomic_fetch_add_explicit(&invalid_arguments, 1, memory_order_relaxed);
        ShaderInfo *failed = calloc(1, sizeof *failed);
        if (failed) {
            failed->stage = stage;
            snprintf(failed->error, sizeof failed->error, "invalid shader compiler input");
        }
        return failed;
    }
    CompilerCounters *count = &counters[stage];
    COUNT(count, requests);
    if (!direct_enabled()) COUNT(count, disabled);
    else if (!direct_eligible(code, bytes, constants, stage, key)) COUNT(count, ineligible);
    else {
        MaxwellSpirvOutput output = {0};
        COUNT(count, direct_attempts);
        MaxwellSpirvStatus result = maxwell_direct_spirv(code, bytes, constants, stage, key, &output);
        if (result == MAXWELL_SPIRV_OK && direct_output_valid(&output, stage)) {
            ShaderInfo *info = malloc(sizeof *info);
            if (info) {
                *info = output.info;
                info->spirv = output.words;
                info->spirv_words = output.word_count;
                output.words = NULL;
                maxwell_direct_spirv_free(&output);
                COUNT(count, direct);
                return info;
            }
            result = MAXWELL_SPIRV_OOM;
        } else if (result == MAXWELL_SPIRV_OK) result = MAXWELL_SPIRV_INVALID;
        if (result == MAXWELL_SPIRV_UNSUPPORTED) COUNT(count, unsupported);
        else if (result == MAXWELL_SPIRV_OOM) COUNT(count, oom);
        else COUNT(count, invalid);
        maxwell_direct_spirv_free(&output);
    }
    COUNT(count, fallback);
    ShaderInfo *info = maxwell_translate_sized(code, bytes, constants, stage, key);
    if (!shader_info_succeeded(info)) COUNT(count, failures);
    return info;
}
void shader_compiler_stats(ShaderCompilerStats *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
#define SNAPSHOT(field) out->stage[i].field = atomic_load_explicit(&counters[i].field, memory_order_relaxed)
    for (unsigned i = 0; i < 6; i++) {
        SNAPSHOT(requests); SNAPSHOT(direct_attempts); SNAPSHOT(direct); SNAPSHOT(fallback);
        SNAPSHOT(unsupported); SNAPSHOT(invalid); SNAPSHOT(oom); SNAPSHOT(ineligible);
        SNAPSHOT(disabled); SNAPSHOT(failures);
    }
#undef SNAPSHOT
    out->invalid_arguments = atomic_load_explicit(&invalid_arguments, memory_order_relaxed);
}
