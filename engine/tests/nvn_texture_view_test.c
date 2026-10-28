/* Actual intercepted APIs, command-view copies, and the Wonder stack-overwrite regression. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include <pthread.h>
#include <stddef.h>
#include "nvn/nvn.h"
#undef FATAL
#undef LOG
static void quiet_log(const char *format,...) { (void)format; }
#define LOG(...) quiet_log(__VA_ARGS__)
static unsigned checks, failures, notifications, format_notes;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"line%d: %s\n",__LINE__,#x);abort(); } } while (0)
static jmp_buf fatal_env;
static bool expecting_fatal;
static void fatal(const char *format,...) __attribute__((noreturn));
static void fatal(const char *format,...) {
    ++failures;
    if (expecting_fatal) longjmp(fatal_env,1);
    va_list ap;va_start(ap,format);vfprintf(stderr,format,ap);va_end(ap);abort();
}
#define FATAL(...) fatal(__VA_ARGS__)
static pthread_mutex_t g_pool_lock=PTHREAD_MUTEX_INITIALIZER;
void backend_pool_written(uint64_t address,uint64_t size) { CHECK(address && size);++notifications; }
void backend_texture_view_format(NvnTexture *texture,uint32_t format) { CHECK(texture && format);++format_notes; }
#include "texture_view_objects.inc"
#include "texture_view_commands.inc"
static _Alignas(16) uint8_t command[1024];
static CmdRec *record(Cpu *cpu,uint32_t op,uint32_t extra,Snap *snapshot) {
    (void)cpu;CHECK(sizeof(CmdRec)+extra<=sizeof command);memset(command,0,sizeof command);
    CmdRec *r=(CmdRec*)command;r->op=op;r->size=sizeof(CmdRec)+extra;
    memcpy(r->x,snapshot->x+1,sizeof r->x);memcpy(r->f,snapshot->f,sizeof r->f);return r;
}
static void call(HostFn fn,NvnTextureView *view,uint64_t a,uint64_t b,uint64_t d,uint64_t e) {
    Cpu c={0};c.x[0]=(uintptr_t)view;c.x[1]=a;c.x[2]=b;c.x[3]=d;c.x[4]=e;fn(&c);
}
static void canaries(const uint8_t *p,size_t n,uint8_t byte) { for(size_t i=0;i<n;i++)CHECK(p[i]==byte); }
static void unchanged_on_error(HostFn fn,NvnTextureView *v,uint64_t a,uint64_t b,uint64_t d,uint64_t e) {
    NvnTextureView before=*v;unsigned count=failures;expecting_fatal=true;
    if(!setjmp(fatal_env)) { call(fn,v,a,b,d,e);CHECK(!"invalid value accepted"); }
    expecting_fatal=false;CHECK(failures==count+1);CHECK(!memcmp(v,&before,sizeof before));
}
static void getters(const NvnTextureView *v) {
    struct {uint32_t before,words[4],after;} out={0xaabbccdd,{0,0,0,0},0x12345678};
    const HostFn functions[]={hle_nvnTextureViewGetLevels,hle_nvnTextureViewGetLayers,hle_nvnTextureViewGetFormat,hle_nvnTextureViewGetSwizzle,hle_nvnTextureViewGetDepthStencilMode,hle_nvnTextureViewGetTarget};
    const uint32_t expected[][4]={{v->base_level,v->num_levels},{v->min_layer,v->num_layers},{v->format},{v->swizzle[0],v->swizzle[1],v->swizzle[2],v->swizzle[3]},{v->ds_mode},{v->target}};
    const unsigned counts[]={2,2,1,4,1,1};
    for(unsigned f=0;f<6;f++) {
        Cpu c={0};c.x[0]=(uintptr_t)v;for(unsigned i=0;i<4;i++){out.words[i]=0xcafebabe;c.x[1+i]=(uintptr_t)&out.words[i];}
        functions[f](&c);CHECK(c.x[0]==!!(v->setmask&(1u<<f)));
        for(unsigned i=0;i<4;i++)CHECK(out.words[i]==(i<counts[f]?expected[f][i]:0xcafebabe));
        CHECK(out.before==0xaabbccdd && out.after==0x12345678);
    }
}
static void api_tests(void) {
    CHECK(sizeof(NvnTextureView)==16);CHECK(offsetof(NvnTextureView,swizzle)+4==16);
    struct {uint8_t before[16];NvnTextureView view;uint8_t after[32];} guarded;
    memset(&guarded,0x97,sizeof guarded);NvnTextureView *v=&guarded.view;
    call(hle_nvnTextureViewSetDefaults,v,0,0,0,0);NvnTextureView zero={0};CHECK(!memcmp(v,&zero,16));getters(v);
    call(hle_nvnTextureViewSetLevels,v,2,0,0,0);call(hle_nvnTextureViewSetLayers,v,3,0,0,0);getters(v);
    call(hle_nvnTextureViewSetLevels,v,65535,65534,0,0);
    call(hle_nvnTextureViewSetLayers,v,60000,50000,0,0);
    call(hle_nvnTextureViewSetFormat,v,0xa0,0,0,0);
    call(hle_nvnTextureViewSetTarget,v,10,0,0,0);
    call(hle_nvnTextureViewSetDepthStencilMode,v,1,0,0,0);
    call(hle_nvnTextureViewSetSwizzle,v,5,4,1,0);
    CHECK(v->setmask==63 && v->base_level==65535 && v->num_levels==65534 && v->min_layer==60000 && v->num_layers==50000);
    CHECK(v->format==0xa0 && v->target==10 && v->ds_mode==1);CHECK(v->swizzle[0]==5 && v->swizzle[1]==4 && v->swizzle[2]==1 && v->swizzle[3]==0);getters(v);
    for(unsigned i=0;i<=255;i++){call(hle_nvnTextureViewSetFormat,v,i,0,0,0);CHECK(v->format==i);}
    for(unsigned i=0;i<=10;i++){call(hle_nvnTextureViewSetTarget,v,i,0,0,0);CHECK(v->target==i);}
    for(unsigned i=0;i<=5;i++){call(hle_nvnTextureViewSetSwizzle,v,i,i,i,i);for(unsigned j=0;j<4;j++)CHECK(v->swizzle[j]==i);}
    unchanged_on_error(hle_nvnTextureViewSetLevels,v,65536,1,0,0);
    unchanged_on_error(hle_nvnTextureViewSetLevels,v,1,UINT64_MAX,0,0);
    unchanged_on_error(hle_nvnTextureViewSetLayers,v,65536,1,0,0);
    unchanged_on_error(hle_nvnTextureViewSetLayers,v,1,UINT32_MAX,0,0);
    unchanged_on_error(hle_nvnTextureViewSetFormat,v,256,0,0,0);
    unchanged_on_error(hle_nvnTextureViewSetFormat,v,UINT64_MAX,0,0,0);
    unchanged_on_error(hle_nvnTextureViewSetTarget,v,11,0,0,0);
    unchanged_on_error(hle_nvnTextureViewSetDepthStencilMode,v,2,0,0,0);
    for(unsigned i=0;i<4;i++){uint64_t sw[4]={1,2,3,4};sw[i]=6;unchanged_on_error(hle_nvnTextureViewSetSwizzle,v,sw[0],sw[1],sw[2],sw[3]);}
    canaries(guarded.before,sizeof guarded.before,0x97);canaries(guarded.after,sizeof guarded.after,0x97);
    // Exact-size allocations give ASan redzones after byte16: Compare must not
    // inspect the old28-byte extent; opaque copies need no hidden host lifetime.
    NvnTextureView *copy=malloc(16),*source=malloc(16);CHECK(copy && source);memcpy(source,v,16);memcpy(copy,source,16);
    for(unsigned i=0;i<16;i++) {
        Cpu c={0};c.x[0]=(uintptr_t)source;c.x[1]=(uintptr_t)copy;hle_nvnTextureViewCompare(&c);CHECK(c.x[0]==1);
        ((uint8_t*)copy)[i]^=1;c.x[0]=(uintptr_t)source;hle_nvnTextureViewCompare(&c);CHECK(c.x[0]==0);((uint8_t*)copy)[i]^=1;
    }
    free(source);getters(copy);free(copy);
    call(hle_nvnTextureViewSetDefaults,v,0,0,0,0);CHECK(!memcmp(v,&zero,16));canaries(guarded.after,sizeof guarded.after,0x97);
}
static void copied_consumers(void) {
    NvnTexture t={.format=0x25};uint64_t guest=(uintptr_t)&t;
    NvnTextureView *view=malloc(16);CHECK(view);call(hle_nvnTextureViewSetDefaults,view,0,0,0,0);
    call(hle_nvnTextureViewSetLevels,view,3,2,0,0);call(hle_nvnTextureViewSetLayers,view,4,5,0,0);
    call(hle_nvnTextureViewSetFormat,view,0x26,0,0,0);call(hle_nvnTextureViewSetSwizzle,view,2,3,4,5);
    call(hle_nvnTextureViewSetDepthStencilMode,view,1,0,0,0);call(hle_nvnTextureViewSetTarget,view,4,0,0,0);
    NvnTextureView expected=*view;NvnCopyRegion region={1,2,3,4,5,6};
    TexRef ref={0};tex_ref(&ref,(uintptr_t)&guest,(uintptr_t)view,(uintptr_t)&region);
    CHECK(ref.tex==(uintptr_t)&t && ref.has_view && !memcmp(&ref.view,&expected,16));CHECK(!memcmp(ref.region,&region,24));
    NvnTexEntry entry={0};NvnDescPool pool={.count=1,.entries=&entry};uint64_t guest_pool=(uintptr_t)&pool;Cpu cpu={0};
    cpu.x[0]=(uintptr_t)&guest_pool;cpu.x[2]=(uintptr_t)&guest;cpu.x[3]=(uintptr_t)view;register_texture(&cpu);
    CHECK(entry.tex==&t && entry.has_view && t.pool_refs==1);CHECK(!memcmp(&entry.view,&expected,16));
    uint64_t colors[]={(uintptr_t)&guest},views[]={(uintptr_t)view};cpu=(Cpu){0};cpu.x[1]=1;cpu.x[2]=(uintptr_t)colors;cpu.x[3]=(uintptr_t)views;cpu.x[4]=(uintptr_t)&guest;cpu.x[5]=(uintptr_t)view;
    hle_nvnCommandBufferSetRenderTargets(&cpu);RtRec rt;CHECK(((CmdRec*)command)->op==CMD_SET_RENDER_TARGETS);memcpy(&rt,((CmdRec*)command)->extra,sizeof rt);
    CHECK(rt.tex[0]==(uintptr_t)&t && rt.depth==(uintptr_t)&t && rt.has_view[0] && rt.has_dview);CHECK(!memcmp(&rt.views[0],&expected,16) && !memcmp(&rt.dview,&expected,16));
    float color[]={1,2,3,4};cpu=(Cpu){0};cpu.x[1]=(uintptr_t)&guest;cpu.x[2]=(uintptr_t)view;cpu.x[3]=(uintptr_t)&region;cpu.x[4]=(uintptr_t)color;clear_texture(&cpu,0);
    ClearTexRec clear;CHECK(((CmdRec*)command)->op==CMD_CLEAR_TEXTURE);memcpy(&clear,((CmdRec*)command)->extra,sizeof clear);
    CHECK(clear.tex==(uintptr_t)&t && clear.has_view && !memcmp(&clear.view,&expected,16));CHECK(!memcmp(clear.region,&region,24) && !memcmp(clear.color,color,16));
    memset(view,0,16);free(view);
    CHECK(!memcmp(&ref.view,&expected,16) && !memcmp(&entry.view,&expected,16) && !memcmp(&rt.dview,&expected,16) && !memcmp(&clear.view,&expected,16));CHECK(format_notes==5);
}
static void client_stack_write(void) {
    // Exact observed lifetime/order: region initialized, then defaults+levels
    // on the immediately preceding16B view, then tightly packed WriteTexels.
    struct {uint64_t before;NvnTextureView view;NvnCopyRegion region;uint64_t after;} stack;
    memset(&stack,0xa7,sizeof stack);stack.region=(NvnCopyRegion){0,0,0,4,1,1};NvnCopyRegion original=stack.region;
    CHECK((uint8_t*)&stack.region-(uint8_t*)&stack.view==16);
    call(hle_nvnTextureViewSetDefaults,&stack.view,0,0,0,0);call(hle_nvnTextureViewSetLevels,&stack.view,0,1,0,0);
    CHECK(!memcmp(&stack.region,&original,24));CHECK(stack.before==UINT64_C(0xa7a7a7a7a7a7a7a7) && stack.after==stack.before);
    TexLayout layout={.width=4,.height=1,.depth=1,.layers=1,.levels=1,.fmt=nvn_format_info(0x25)};tegra_layout_init(&layout);
    size_t n=tegra_total_size(&layout);uint8_t *allocation=malloc(n+64),*expected=malloc(n);CHECK(allocation && expected);
    memset(allocation,0x67,n+64);uint8_t *dst=allocation+32;memcpy(expected,dst,n);
    const uint8_t input[16]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    LevelLayout l=tegra_level(&layout,0);tegra_swizzle(input,expected,l.wb,l.hb,1,4,l.block_height_log2,l.block_depth_log2);
    NvnPool pool={.storage=(uintptr_t)dst,.size=n};NvnTexture texture={.layout=layout,.target=0,.format=0x25,.width=4,.height=1,.depth=1,.levels=1,.pool=&pool,.addr=(uintptr_t)dst};uint64_t guest=(uintptr_t)&texture;
    Cpu cpu={0};cpu.x[0]=(uintptr_t)&guest;cpu.x[1]=(uintptr_t)&stack.view;cpu.x[2]=(uintptr_t)&stack.region;cpu.x[3]=(uintptr_t)input;
    unsigned before=notifications;hle_nvnTextureWriteTexels(&cpu);
    CHECK(notifications==before+1 && texture.cpu_write_generation==1);CHECK(!memcmp(dst,expected,n));CHECK(!memcmp(&stack.region,&original,24));
    canaries(allocation,32,0x67);canaries(allocation+n+32,32,0x67);free(expected);free(allocation);
}
int main(void) { api_tests();copied_consumers();client_stack_write();printf("state=passed checks=%u rejected=%u; actual16B TextureView APIs/copies and Wonder4x1 WriteTexels canary\n",checks,failures);return 0; }
