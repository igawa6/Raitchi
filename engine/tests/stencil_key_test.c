/* Production key construction/memo entry points, with only compilation mocked. */
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "shader.h"
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);abort();}}while(0)
typedef struct{uint8_t ds;uint32_t format;}NvnTexture;
typedef struct{uint8_t ds_mode;uint32_t setmask;}NvnTextureView;
typedef struct{NvnTexture*tex;NvnTextureView view;bool has_view;}NvnTexEntry;
static bool vk_sampled_stencil(const NvnTexture*t,const NvnTextureView*v,bool has){return(has?v->ds_mode:t->ds)==1;}
typedef struct{unsigned count;void*entries;}Pool;
static struct{Pool*tpool;uint64_t tex[6][32];}st;
typedef struct{uint64_t code_addr;}Stage;
typedef struct{unsigned stage_mask;Stage stages[2];}NvnProgram;
typedef struct{uint32_t value;}Prog;
static uint32_t key_gen;
static unsigned prof_prog_memo,compiles,fingerprints;
static bool key_memo_on(void){return true;}
static void prog_fp_check(Prog*p,NvnProgram*n){CHECK(p&&n);fingerprints++;}
static Prog variants[2];
static Prog*get_program_(NvnProgram*n,ShaderKey*v,ShaderKey*f){CHECK(n&&v);compiles++;return &variants[f->tex_uint!=0];}
#include "stencil_keys.production.h"
int main(void){
    NvnTexture t[2]={{.ds=0},{.ds=1}};NvnTexEntry e[2]={{.tex=t},{.tex=t+1}};Pool pool={2,e};st.tpool=&pool;
    for(unsigned s=0;s<32;s++){
        memset(st.tex,0,sizeof st.tex);st.tex[1][s]=(1u<<20)|1u;
        CHECK(sampled_uint_mask(1)==(1u<<s));CHECK(sampled_uint_mask(0)==0);
        e[1].view.ds_mode=0;e[1].has_view=true;CHECK(sampled_uint_mask(1)==0);
        e[1].view.ds_mode=1;CHECK(sampled_uint_mask(1)==(1u<<s));e[1].has_view=false;
        st.tex[1][s]=(1u<<20)|2u;CHECK(sampled_uint_mask(1)==0); /* invalid pool entry */
    }
    st.tpool=NULL;CHECK(!sampled_uint_mask(1));
    NvnTextureView depth={.ds_mode=77,.setmask=32},before=e[1].view;
    t[1].format=0x35;e[1].has_view=true;e[1].view.setmask=63;before=e[1].view;
    CHECK(legacy_stencil_depth_view(e+1,&depth));CHECK(depth.ds_mode==0&&depth.setmask==63);
    CHECK(!memcmp(&before,&e[1].view,sizeof before));
    t[1].format=0x36;CHECK(legacy_stencil_depth_view(e+1,&depth));
    t[1].format=0x31;CHECK(!legacy_stencil_depth_view(e+1,&depth)); /* intrinsic S8 has no depth */
    t[1].format=0x35;e[1].view.ds_mode=0;CHECK(!legacy_stencil_depth_view(e+1,&depth));
    CHECK(!legacy_stencil_depth_view(NULL,&depth));
    e[1].has_view=false;CHECK(legacy_stencil_depth_view(e+1,&depth));CHECK(depth.ds_mode==0&&depth.setmask==16);
    ShaderKey v={.glsl=1},f={.glsl=1};NvnProgram np={.stage_mask=3,.stages={{0x1000},{0x2000}}};
    uint64_t float_hash=production_program_hash(&np,&v,&f);
    Prog*p=get_program(&np,&v,&f);CHECK(p==variants&&compiles==1);
    CHECK(get_program(&np,&v,&f)==p&&compiles==1&&prof_prog_memo==1);
    f.tex_uint=1;CHECK(production_program_hash(&np,&v,&f)!=float_hash);
    CHECK(get_program(&np,&v,&f)==variants+1&&compiles==2);
    CHECK(get_program(&np,&v,&f)==variants+1&&compiles==2&&prof_prog_memo==2);
    f.tex_uint=0;CHECK(get_program(&np,&v,&f)==variants&&compiles==3);
    for(unsigned i=0;i<32;i++){f.tex_uint=1u<<i;CHECK(production_program_hash(&np,&v,&f)!=float_hash);}
    CHECK(fingerprints>=2);
    printf("{\"state\":\"passed\",\"checks\":%u,\"typed_memo_and_hash\":true}\n",checks);return 0;
}
