/* Actual render translation/module integration. Compiler/driver bodies are
 * controlled here; synthetic direct-emitter GPU equivalence has a separate gate. */
#define _GNU_SOURCE
#include "shader_compiler.h"
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, scenarios;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); abort(); } } while (0)
#define LOG(...) ((void)0)
#define HEAP_BASE UINT64_C(0x100000000)
#define HEAP_MAX UINT64_C(0x10000000)
#define MAX_BIND 96
#define VK_SHADER_STAGE_VERTEX_BIT 1u
#define VK_SHADER_STAGE_FRAGMENT_BIT 16u
#define VK_NULL_HANDLE 0
typedef uint64_t VkShaderModule, VkPipelineLayout;
typedef struct { unsigned set,binding; } VkBindingInfo;
typedef struct {
    ShaderInfo *vi,*fi;
    uint64_t code[2],cmax[2],csize[2];
    const uint8_t *read_code[2];
    ShaderKey vkey,fkey;
    VkShaderModule vs,fs,fs_late;
    VkBindingInfo binds[MAX_BIND];
    int nbinds;
    VkPipelineLayout layout;
    bool vs_uses_vid,headstart;
    _Atomic int state;
    uint64_t chash,prep_vh,prep_fh;
    struct { uint64_t hash; } fp[2];
} Prog;
uint64_t maxwell_gmem_base,maxwell_gmem_size;
uint32_t maxwell_gmem_chunk_shift;
const uint8_t *maxwell_dbg_code;
uint32_t maxwell_dbg_pc;
int maxwell_dbg_reg;
static pthread_mutex_t translate_lock=PTHREAD_MUTEX_INITIALIZER;
static _Atomic int stat_compiles;
typedef struct { bool direct,fail,null_failure,vertex_id,source_vertex_id,early; } StagePlan;
static StagePlan plans[2];
static const uint8_t *seen_code[2];
static size_t seen_bytes[2],seen_constants[2];
static ShaderKey seen_key[2];
static unsigned wrapper_calls[2],shaderc_calls[2],reflection_calls,module_calls,marked,layouts,recorded;
static unsigned legacy_calls[2],reflection_source[2];
static unsigned begins,ends,diag_begins,diag_done,last_diag_state,slow_calls;
static int shaderc_failure_stage,fail_module_call;
static int legacy_failure_stage,legacy_null_stage;
static bool legacy_mask_mismatch;
static unsigned fail_module_mask;
static uint64_t ticks;
static uint8_t original[2][0x90],immutable[2][0x90];
typedef struct { void *pointer; bool active; } Allocation;
static Allocation allocations[256];
static unsigned allocated,freed;
static uint32_t *words(unsigned stage,bool early) {
    size_t n=early?8:5;uint32_t *w=calloc(n,sizeof*w);CHECK(w);
    w[0]=0x07230203;w[1]=0x00010000;w[2]=stage;w[3]=2;
    if(early){w[5]=3u<<16|16u;w[6]=1;w[7]=9;}
    CHECK(allocated<256);allocations[allocated++]=(Allocation){w,true};return w;
}
static void tracked_free(void *pointer) {
    if(pointer)for(unsigned i=allocated;i>0;i--)if(allocations[i-1].pointer==pointer&&allocations[i-1].active){allocations[i-1].active=false;freed++;break;}
    free(pointer);
}
static unsigned outstanding(void) {unsigned n=0;for(unsigned i=0;i<allocated;i++)n+=allocations[i].active;return n;}
static uint64_t SDL_GetTicksNS(void) {return ++ticks;}
static uint32_t gmem_chunk_shift(void) {return 18;}
static int build_begin(const char *what,uint64_t a,uint64_t b) {(void)a;(void)b;CHECK(!strcmp(what,"modules vs/fs"));begins++;return 17;}
static void build_end(int token) {CHECK(token==17);ends++;}
static void build_diag_begin_program(uint64_t id,uint64_t a,uint64_t b) {CHECK(id==1234&&a==11&&b==22);diag_begins++;}
static void build_diag_done(const char *what,uint64_t id,uint64_t start,int state) {(void)start;CHECK(!strcmp(what,"modules vs/fs")&&id==1234);diag_done++;last_diag_state=(unsigned)state;}
static void slow_build(uint64_t start,const char *what,uint64_t a,uint64_t b) {(void)start;(void)what;(void)a;(void)b;slow_calls++;}
static void mark_dynamic(Prog *p) {CHECK(p->nbinds==2);marked++;}
static VkPipelineLayout make_layout(Prog *p,int nsets,unsigned a,unsigned b) {CHECK(p->nbinds==2&&nsets==2&&a==1&&b==16);layouts++;return 99;}
static void hs_record_program(const Prog *p) {CHECK(p->vi&&p->fi&&!atomic_load(&p->state));recorded++;}
static uint32_t *vk_glsl_to_spirv(const char *source,int stage,const char *name,size_t *nwords) {
    CHECK(stage==STAGE_VERTEX||stage==STAGE_FRAGMENT);CHECK(source&&name);
    shaderc_calls[stage]++;
    if(stage==shaderc_failure_stage)return NULL;
    *nwords=plans[stage].early?8:5;uint32_t *out=words((unsigned)stage,plans[stage].early);out[2]|=0x100;return out;
}
static int vk_reflect(const uint32_t *w,size_t n,VkBindingInfo *out,int cap) {
    CHECK(w&&w[0]==0x07230203&&n>=5&&(w[2]&0xff)<2);CHECK(cap==MAX_BIND-(int)reflection_calls);
    unsigned stage=w[2]&0xff;reflection_source[stage]+=(w[2]&0x100)!=0;
    out[0]=(VkBindingInfo){stage,7};reflection_calls++;return 1;
}
static VkShaderModule vk_module(const uint32_t *w,size_t n) {
    CHECK(w&&w[0]==0x07230203&&n>=5);module_calls++;
    return (int)module_calls==fail_module_call||(fail_module_mask&(1u<<(module_calls-1)))?0:100+module_calls;
}
ShaderInfo *maxwell_translate_sized(const uint8_t *code,size_t bytes,size_t constants,int stage,const ShaderKey *key) {
    CHECK(stage==STAGE_VERTEX||stage==STAGE_FRAGMENT);CHECK(code==immutable[stage]&&bytes==sizeof immutable[stage]);
    CHECK(constants==seen_constants[stage]&&key->tex_uint==seen_key[stage].tex_uint);
    if(stage==STAGE_FRAGMENT)CHECK(key->vs_out_mask==0x25);
    CHECK(maxwell_dbg_code==NULL);legacy_calls[stage]++;
    if(stage==legacy_null_stage)return NULL;
    ShaderInfo *info=calloc(1,sizeof*info);CHECK(info);info->stage=stage;info->attr_mask=0x14;
    info->out_mask=stage==STAGE_VERTEX?(legacy_mask_mismatch?0x26:0x25):0x07;
    if(stage==legacy_failure_stage){strcpy(info->error,"legacy retry translation failed");return info;}
    info->msl=strdup(plans[stage].source_vertex_id?"void main(){uint v=gl_VertexIndex;}":"void main(){}");return info;
}
ShaderInfo *maxwell_compile_sized(const uint8_t *code,size_t bytes,size_t constants,int stage,const ShaderKey *key) {
    CHECK(stage==STAGE_VERTEX||stage==STAGE_FRAGMENT);CHECK(code&&key);
    wrapper_calls[stage]++;seen_code[stage]=code;seen_bytes[stage]=bytes;seen_constants[stage]=constants;seen_key[stage]=*key;
    StagePlan *plan=&plans[stage];if(plan->null_failure)return NULL;
    ShaderInfo *info=calloc(1,sizeof*info);CHECK(info);info->stage=stage;
    info->attr_mask=0x14;info->out_mask=stage==STAGE_VERTEX?0x25:0x07;info->uses_vertex_id=plan->vertex_id;
    if(plan->fail){strcpy(info->error,"controlled translation rejection");return info;}
    if(plan->direct){info->spirv=words((unsigned)stage,plan->early);info->spirv_words=plan->early?8:5;}
    else info->msl=strdup(plan->source_vertex_id?"void main(){uint v=gl_VertexIndex;}":"void main(){}");
    return info;
}
#define free tracked_free
#include "direct_spirv_integration_actual.inc"
#undef free

static Prog setup(bool direct_vs,bool direct_fs) {
    CHECK(!outstanding());allocated=freed=0;memset(allocations,0,sizeof allocations);
    memset(plans,0,sizeof plans);plans[0].direct=direct_vs;plans[1].direct=direct_fs;
    memset(wrapper_calls,0,sizeof wrapper_calls);memset(shaderc_calls,0,sizeof shaderc_calls);
    memset(legacy_calls,0,sizeof legacy_calls);memset(reflection_source,0,sizeof reflection_source);
    memset(seen_code,0,sizeof seen_code);memset(seen_constants,0,sizeof seen_constants);
    reflection_calls=module_calls=marked=layouts=recorded=begins=ends=diag_begins=diag_done=last_diag_state=slow_calls=0;
    atomic_store(&stat_compiles,0);shaderc_failure_stage=-1;fail_module_call=-1;
    legacy_failure_stage=legacy_null_stage=-1;legacy_mask_mismatch=false;fail_module_mask=0;
    Prog p={.vi=NULL,.fi=NULL,.chash=1234,.prep_vh=11,.prep_fh=22,.vkey={.glsl=1,.tex_uint=5},.fkey={.glsl=1,.tex_uint=6,.vs_out_mask=0xdeadbeef}};
    for(unsigned i=0;i<2;i++){p.code[i]=(uintptr_t)original[i];p.read_code[i]=immutable[i];p.cmax[i]=sizeof immutable[i];}
    return p;
}
static void cleanup(Prog *p) {
    maxwell_free(p->vi);maxwell_free(p->fi);p->vi=p->fi=NULL;
    CHECK(!outstanding()&&allocated==freed);scenarios++;
}
static void translated(const Prog *p) {
    CHECK(wrapper_calls[0]==1&&wrapper_calls[1]==1);
    CHECK(seen_code[0]==immutable[0]&&seen_code[1]==immutable[1]);
    CHECK(seen_bytes[0]==sizeof immutable[0]&&seen_bytes[1]==sizeof immutable[1]);
    CHECK(!seen_constants[0]&&!seen_constants[1]);
    CHECK(seen_key[1].vs_out_mask==0x25&&p->fkey.vs_out_mask==0xdeadbeef);
    CHECK(seen_key[0].tex_uint==5&&seen_key[1].tex_uint==6);
    CHECK(recorded==1&&maxwell_gmem_base==HEAP_BASE&&maxwell_gmem_size==HEAP_MAX&&maxwell_gmem_chunk_shift==18);
}
static void compiled(Prog *p,unsigned expected_vs_source,unsigned expected_fs_source,unsigned modules) {
    compile_program(p);CHECK(atomic_load(&p->state)==1);
    CHECK(shaderc_calls[0]==expected_vs_source&&shaderc_calls[1]==expected_fs_source);
    CHECK(module_calls==modules&&reflection_calls==2&&marked==1&&layouts==1&&p->layout==99&&p->nbinds==2);
    CHECK(begins==1&&ends==1&&diag_begins==1&&diag_done==1&&last_diag_state==1&&slow_calls==1&&stat_compiles==1);
    CHECK(!p->vi->spirv&&!p->vi->spirv_words&&!p->fi->spirv&&!p->fi->spirv_words);
    CHECK(p->vi->out_mask==0x25&&p->vi->attr_mask==0x14);
}
int main(void) {
    CHECK(!unsetenv("LARECOMP_DBG_FS"));
    for(unsigned mask=0;mask<4;mask++) {
        Prog p=setup(mask&1,mask&2);CHECK(translate_program_now(&p));translated(&p);
        compiled(&p,!(mask&1),!(mask&2),2);cleanup(&p);
    }
    Prog p=setup(true,true);plans[1].early=true;CHECK(translate_program_now(&p));compiled(&p,0,0,3);CHECK(p.fs_late!=0);cleanup(&p);
    p=setup(true,true);plans[0].vertex_id=true;CHECK(translate_program_now(&p)&&p.vs_uses_vid);cleanup(&p);
    p=setup(false,true);plans[0].source_vertex_id=true;CHECK(translate_program_now(&p)&&p.vs_uses_vid);cleanup(&p);
    p=setup(true,true);CHECK(translate_program_now(&p)&&!p.vs_uses_vid);cleanup(&p);
    p=setup(true,true);p.read_code[0]=p.read_code[1]=NULL;CHECK(translate_program_now(&p));
    CHECK(seen_code[0]==original[0]&&seen_code[1]==original[1]&&seen_constants[0]==SIZE_MAX&&seen_constants[1]==SIZE_MAX);cleanup(&p);
    p=setup(true,true);p.read_code[0]=p.read_code[1]=NULL;p.headstart=true;CHECK(translate_program_now(&p));CHECK(!seen_constants[0]&&!seen_constants[1]);cleanup(&p);
    p=setup(true,true);p.csize[0]=7;p.csize[1]=13;CHECK(translate_program_now(&p));CHECK(seen_constants[0]==7&&seen_constants[1]==13);cleanup(&p);
    p=setup(true,true);p.code[1]=0;p.read_code[1]=NULL;CHECK(translate_program_now(&p));
    CHECK(wrapper_calls[0]==1&&!wrapper_calls[1]&&p.fi->msl&&p.fi->stage==STAGE_FRAGMENT&&recorded==1);
    compiled(&p,0,1,2);cleanup(&p);
    p=setup(true,true);plans[0].fail=true;CHECK(!translate_program_now(&p)&&atomic_load(&p.state)==2);
    CHECK(wrapper_calls[0]==1&&!wrapper_calls[1]&&!recorded&&!allocated);cleanup(&p);
    p=setup(true,true);plans[1].fail=true;CHECK(!translate_program_now(&p)&&atomic_load(&p.state)==2);
    CHECK(wrapper_calls[0]==1&&wrapper_calls[1]==1&&!recorded&&outstanding()==1);cleanup(&p);
    p=setup(true,true);plans[0].null_failure=true;CHECK(!translate_program_now(&p)&&atomic_load(&p.state)==2);CHECK(!p.vi&&!wrapper_calls[1]);cleanup(&p);
    p=setup(true,true);plans[1].null_failure=true;CHECK(!translate_program_now(&p)&&atomic_load(&p.state)==2);CHECK(!p.fi&&outstanding()==1);cleanup(&p);
    p=setup(false,true);CHECK(translate_program_now(&p));shaderc_failure_stage=STAGE_VERTEX;compile_program(&p);
    CHECK(atomic_load(&p.state)==2&&shaderc_calls[0]==1&&!shaderc_calls[1]&&!module_calls&&outstanding()==1);
    CHECK(p.fi->spirv&&begins==1&&ends==1&&last_diag_state==2&&!stat_compiles);cleanup(&p);
    p=setup(true,false);CHECK(translate_program_now(&p));shaderc_failure_stage=STAGE_FRAGMENT;compile_program(&p);
    CHECK(atomic_load(&p.state)==2&&!shaderc_calls[0]&&shaderc_calls[1]==1&&!module_calls&&!outstanding());cleanup(&p);
    p=setup(true,true);CHECK(translate_program_now(&p));fail_module_call=1;compiled(&p,1,0,3);
    CHECK(legacy_calls[0]==1&&!legacy_calls[1]&&p.vi->msl&&!p.fi->msl&&reflection_source[0]==1&&!reflection_source[1]);cleanup(&p);
    p=setup(true,true);CHECK(translate_program_now(&p));fail_module_call=2;compiled(&p,0,1,3);
    CHECK(!legacy_calls[0]&&legacy_calls[1]==1&&!p.vi->msl&&p.fi->msl&&!reflection_source[0]&&reflection_source[1]==1);cleanup(&p);
    p=setup(true,true);CHECK(translate_program_now(&p));fail_module_mask=1u|4u;compiled(&p,1,1,4);
    CHECK(legacy_calls[0]==1&&legacy_calls[1]==1&&reflection_source[0]==1&&reflection_source[1]==1);cleanup(&p);
    p=setup(true,true);plans[0].source_vertex_id=true;CHECK(translate_program_now(&p)&&!p.vs_uses_vid);
    fail_module_call=1;compiled(&p,1,0,3);CHECK(p.vs_uses_vid);cleanup(&p);
    p=setup(true,true);CHECK(translate_program_now(&p));fail_module_mask=3u;compile_program(&p);
    CHECK(atomic_load(&p.state)==2&&module_calls==3&&!p.vs&&p.fs&&legacy_calls[0]==1&&!outstanding()&&last_diag_state==2);cleanup(&p);
    p=setup(true,true);CHECK(translate_program_now(&p));fail_module_mask=6u;compile_program(&p);
    CHECK(atomic_load(&p.state)==2&&module_calls==3&&p.vs&&!p.fs&&legacy_calls[1]==1&&!outstanding()&&last_diag_state==2);cleanup(&p);
    p=setup(false,false);CHECK(translate_program_now(&p));fail_module_call=1;compile_program(&p);
    CHECK(atomic_load(&p.state)==2&&module_calls==2&&!legacy_calls[0]&&!legacy_calls[1]&&!outstanding());cleanup(&p);
    p=setup(true,true);CHECK(translate_program_now(&p));p.read_code[0]=NULL;fail_module_call=1;compile_program(&p);
    CHECK(atomic_load(&p.state)==2&&!legacy_calls[0]&&module_calls==2&&!outstanding());cleanup(&p);
    p=setup(true,true);CHECK(translate_program_now(&p));legacy_mask_mismatch=true;fail_module_call=1;compile_program(&p);
    CHECK(atomic_load(&p.state)==2&&p.vi->out_mask==0x25&&!p.vi->msl&&legacy_calls[0]==1&&!shaderc_calls[0]&&!outstanding());cleanup(&p);
    p=setup(true,true);CHECK(translate_program_now(&p));legacy_failure_stage=STAGE_FRAGMENT;fail_module_call=2;compile_program(&p);
    CHECK(atomic_load(&p.state)==2&&legacy_calls[1]==1&&!shaderc_calls[1]&&!p.fi->msl&&!outstanding());cleanup(&p);
    p=setup(true,true);CHECK(translate_program_now(&p));legacy_null_stage=STAGE_VERTEX;fail_module_call=1;compile_program(&p);
    CHECK(atomic_load(&p.state)==2&&legacy_calls[0]==1&&!shaderc_calls[0]&&!p.vi->msl&&!outstanding());cleanup(&p);
    p=setup(true,true);CHECK(translate_program_now(&p));shaderc_failure_stage=STAGE_VERTEX;fail_module_call=1;compile_program(&p);
    CHECK(atomic_load(&p.state)==2&&legacy_calls[0]==1&&shaderc_calls[0]==1&&!p.vi->msl&&!outstanding());cleanup(&p);
    p=setup(true,true);CHECK(translate_program_now(&p));shaderc_failure_stage=STAGE_FRAGMENT;fail_module_call=2;compile_program(&p);
    CHECK(atomic_load(&p.state)==2&&legacy_calls[1]==1&&shaderc_calls[1]==1&&!p.fi->msl&&!outstanding());cleanup(&p);
    size_t n=77;CHECK(!program_spirv(NULL,STAGE_VERTEX,"vs",&n)&&n==0);
    ShaderInfo *empty=calloc(1,sizeof*empty);CHECK(empty);n=77;CHECK(!program_spirv(empty,STAGE_VERTEX,"vs",&n)&&!n);maxwell_free(empty);scenarios++;
    printf("{\"state\":\"passed\",\"scenarios\":%u,\"assertions\":%u,\"actual_render_functions\":true,\"compiler_and_gpu_controlled\":true}\n",scenarios,checks);
}
