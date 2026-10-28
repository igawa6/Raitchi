/* Actual compiler selector + actual legacy translator; synthetic emitter outputs
 * isolate failure/ownership cases. Real direct shader execution has its own gate. */
#define _GNU_SOURCE
#include "shader_compiler.h"
#include "direct_spirv.h"
#include <stdatomic.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Atomic unsigned checks, direct_calls, releases;
static _Atomic bool fail_next_allocation;
#define CHECK(x) do { atomic_fetch_add(&checks, 1); if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x);abort(); } } while (0)
enum FakeMode { DIRECT_OK, DIRECT_UNSUPPORTED, DIRECT_INVALID, DIRECT_OOM, DIRECT_BAD_HEADER, DIRECT_BAD_STAGE };
static enum FakeMode fake_mode;
static uint8_t blob[0x90], compute_blob[16];
static ShaderKey key = {.glsl=1};
bool shared_title_las(void) { return false; }
void *compiler_test_malloc(size_t size) {
    if (atomic_exchange(&fail_next_allocation, false)) return NULL;
    return malloc(size);
}
MaxwellSpirvStatus maxwell_direct_spirv(const uint8_t *code, size_t bytes,
    size_t constants, int stage, const ShaderKey *input_key, MaxwellSpirvOutput *out) {
    CHECK(code && bytes && constants != SIZE_MAX && input_key && input_key->glsl);
    atomic_fetch_add(&direct_calls, 1);
    out->words = calloc(5, sizeof *out->words); CHECK(out->words);
    out->word_count=5; out->words[0]=0x07230203; out->words[1]=0x00010000; out->words[3]=1;
    out->info.stage=stage; out->info.attr_mask=0x14; out->info.out_mask=0x12;
    if (fake_mode==DIRECT_BAD_HEADER) out->words[0]=0;
    if (fake_mode==DIRECT_BAD_STAGE) out->info.stage=stage==STAGE_VERTEX?STAGE_FRAGMENT:STAGE_VERTEX;
    if (fake_mode==DIRECT_UNSUPPORTED) return MAXWELL_SPIRV_UNSUPPORTED;
    if (fake_mode==DIRECT_INVALID) return MAXWELL_SPIRV_INVALID;
    if (fake_mode==DIRECT_OOM) return MAXWELL_SPIRV_OOM;
    return MAXWELL_SPIRV_OK;
}
void maxwell_direct_spirv_free(MaxwellSpirvOutput *out) {
    if (out->words) atomic_fetch_add(&releases, 1);
    free(out->words); memset(out,0,sizeof *out);
}
static ShaderInfo *compile(int stage) {
    return maxwell_compile_sized(stage==STAGE_COMPUTE?compute_blob:blob,
        stage==STAGE_COMPUTE?sizeof compute_blob:sizeof blob,0,stage,&key);
}
static void compare_legacy(size_t constants, int stage, const ShaderKey *input_key) {
    const uint8_t *code=stage==STAGE_COMPUTE?compute_blob:blob;
    size_t bytes=stage==STAGE_COMPUTE?sizeof compute_blob:sizeof blob;
    ShaderInfo *baseline=maxwell_translate_sized(code,bytes,constants,stage,input_key);
    ShaderInfo *selected=maxwell_compile_sized(code,bytes,constants,stage,input_key);
    CHECK(baseline && selected && baseline->msl && selected->msl);
    CHECK(!selected->spirv && !selected->spirv_words && !strcmp(baseline->msl,selected->msl));
    CHECK(baseline->cbuf_mask==selected->cbuf_mask && baseline->out_mask==selected->out_mask);
    maxwell_free(baseline); maxwell_free(selected);
}
static void *parallel_compile(void *unused) {
    (void)unused;
    for (unsigned i=0;i<100;i++) {
        ShaderInfo *info=compile(STAGE_VERTEX);
        CHECK(info && info->spirv && !info->msl && shader_info_succeeded(info));
        maxwell_free(info);
    }
    return NULL;
}
int main(void) {
    uint64_t exit_word=UINT64_C(0xe30000000007000f);
    memcpy(blob+0x88,&exit_word,8); memcpy(compute_blob+8,&exit_word,8);
    const char *options[]={"LARECOMP_VK_DIRECT_SPIRV","LARECOMP_VK_OPT","LARECOMP_VK_OPT_FS","LARECOMP_VK_NO_CONTRACT"};
    for (unsigned i=0;i<sizeof options/sizeof *options;i++) CHECK(!unsetenv(options[i]));
    CHECK(!shader_info_succeeded(NULL));
    CHECK(shader_compiler_policy()==UINT32_C(0x53500100));
    compare_legacy(0,STAGE_VERTEX,&key); CHECK(!direct_calls);
    const char *disabled[]={"0","", "true", "01", "garbage"};
    for (unsigned i=0;i<sizeof disabled/sizeof *disabled;i++) {
        CHECK(!setenv(options[0],disabled[i],1)); compare_legacy(0,STAGE_FRAGMENT,&key);
        CHECK(shader_compiler_policy()==UINT32_C(0x53500100)); CHECK(!direct_calls);
    }
    CHECK(!setenv(options[0],"1",1)); CHECK(shader_compiler_policy()==UINT32_C(0x53500101));
    fake_mode=DIRECT_OK;
    ShaderInfo *info=compile(STAGE_VERTEX);
    CHECK(info && info->spirv && !info->msl && info->spirv_words==5 && shader_info_succeeded(info));
    CHECK(info->attr_mask==0x14 && info->out_mask==0x12); maxwell_free(info);
    unsigned before=direct_calls, released=releases;
    for (unsigned mode=DIRECT_UNSUPPORTED;mode<=DIRECT_BAD_STAGE;mode++) {
        fake_mode=(enum FakeMode)mode; compare_legacy(0,STAGE_FRAGMENT,&key);
    }
    CHECK(direct_calls==before+5 && releases==released+5);
    fake_mode=DIRECT_OK; atomic_store(&fail_next_allocation,true); compare_legacy(0,STAGE_VERTEX,&key);
    CHECK(!fail_next_allocation && releases==released+6);
    before=direct_calls;
    compare_legacy(SIZE_MAX,STAGE_VERTEX,&key); CHECK(direct_calls==before);
    ShaderKey metal={0}; compare_legacy(0,STAGE_VERTEX,&metal); compare_legacy(0,STAGE_VERTEX,NULL);
    CHECK(direct_calls==before);
    maxwell_dbg_code=blob; compare_legacy(0,STAGE_FRAGMENT,&key); maxwell_dbg_code=NULL;
    maxwell_dbg_vs=blob; compare_legacy(0,STAGE_VERTEX,&key); maxwell_dbg_vs=NULL;
    CHECK(direct_calls==before);
    const char *opt_values[]={"1","0",""};
    for (unsigned i=0;i<3;i++) { CHECK(!setenv(options[1],opt_values[i],1)); compare_legacy(0,STAGE_VERTEX,&key); }
    CHECK(!unsetenv(options[1])); CHECK(direct_calls==before);
    CHECK(!setenv(options[2],"1",1)); compare_legacy(0,STAGE_FRAGMENT,&key); CHECK(direct_calls==before);
    info=compile(STAGE_VERTEX); CHECK(info&&info->spirv); maxwell_free(info); before=direct_calls;
    CHECK(!unsetenv(options[2]));
    CHECK(!setenv(options[3],"0",1)); compare_legacy(0,STAGE_VERTEX,&key); CHECK(direct_calls==before);
    info=compile(STAGE_FRAGMENT); CHECK(info&&info->spirv); maxwell_free(info); CHECK(!unsetenv(options[3]));
    fake_mode=DIRECT_UNSUPPORTED; compare_legacy(0,STAGE_COMPUTE,&key);
    info=maxwell_compile_sized(NULL,0,0,STAGE_VERTEX,&key); CHECK(info&&!shader_info_succeeded(info)&&info->error[0]); maxwell_free(info);
    info=maxwell_compile_sized(blob,sizeof blob,0,99,&key); CHECK(info&&!shader_info_succeeded(info)); maxwell_free(info);
    info=maxwell_compile_sized(blob,SIZE_MAX,1,STAGE_VERTEX,&key); CHECK(info&&!shader_info_succeeded(info)); maxwell_free(info);
    info=maxwell_compile_sized(blob,1,0,STAGE_VERTEX,&key); CHECK(info&&!shader_info_succeeded(info)&&info->error[0]); maxwell_free(info);
    ShaderCompilerStats prior, after; shader_compiler_stats(&prior); CHECK(prior.invalid_arguments==3);
    CHECK(prior.stage[STAGE_FRAGMENT].unsupported==1 && prior.stage[STAGE_FRAGMENT].invalid==3 && prior.stage[STAGE_FRAGMENT].oom==1);
    CHECK(prior.stage[STAGE_VERTEX].oom==1 && prior.stage[STAGE_VERTEX].failures==1);
    fake_mode=DIRECT_OK; pthread_t threads[4];
    for (unsigned i=0;i<4;i++) CHECK(!pthread_create(&threads[i],NULL,parallel_compile,NULL));
    for (unsigned i=0;i<4;i++) CHECK(!pthread_join(threads[i],NULL));
    shader_compiler_stats(&after); shader_compiler_stats(NULL);
    CHECK(after.stage[STAGE_VERTEX].requests==prior.stage[STAGE_VERTEX].requests+400);
    CHECK(after.stage[STAGE_VERTEX].direct==prior.stage[STAGE_VERTEX].direct+400);
    CHECK(after.stage[STAGE_VERTEX].fallback==prior.stage[STAGE_VERTEX].fallback);
    CHECK(after.stage[STAGE_COMPUTE].fallback==1 && after.stage[STAGE_COMPUTE].unsupported==1);
    printf("{\"state\":\"passed\",\"checks\":%u,\"direct_calls\":%u,\"failed_output_releases\":%u,\"actual_legacy_parity\":true,\"concurrent_direct_calls\":400}\n",
        atomic_load(&checks),atomic_load(&direct_calls),atomic_load(&releases));
}
