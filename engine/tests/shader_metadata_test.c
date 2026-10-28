/* Synthetic shader/control-flow metadata and production captured-draw join. */
#include "shader.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
bool shared_title_las(void) { return false; }
static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) {fprintf(stderr,"line %d: %s\n",__LINE__,#x);abort();}} while(0)
static unsigned declarations(const char *text,const char *kind) {
    unsigned n=0;char line[96];
    for(unsigned r=0;r<256;r++) {
        if(!strcmp(kind,"r"))snprintf(line,sizeof line,"  uint r%u = 0u;\n",r);
        else snprintf(line,sizeof line,"  f16vec2 %s%u = f16vec2(0.0hf);\n",kind,r);
        if(strstr(text,line))n++;
    }
    return n;
}
#ifndef METADATA_BASELINE
static char log_text[32768];static size_t log_at;
static void test_log(const char *format,...) {
    va_list args;va_start(args,format);int n=vsnprintf(log_text+log_at,sizeof log_text-log_at,format,args);va_end(args);
    CHECK(n>=0&&(size_t)n<sizeof log_text-log_at);log_at+=(size_t)n;
}
#define LOG(...) test_log(__VA_ARGS__)
static uint64_t pg_frame=1400,pg_verts,pg_vc[1024];static int pg_n,pg_state;
static bool draws_enabled;static unsigned key_calls;
static bool pg_draws_on(void) {return draws_enabled;}
typedef struct {uint64_t hash;uint8_t state;} MetaFp;
typedef struct {ShaderInfo *vi,*fi;MetaFp fp[2];uint64_t prep_vh,prep_fh,chash;} Prog;
typedef struct {uint64_t key;} PipeKey;typedef struct {PipeKey k;} Pipe;
static uint64_t pipe_key_hash(const PipeKey *key) {key_calls++;return key->key;}
static struct {struct {uint64_t code_addr;} stages[2];} guest;
static struct {void *unused; __typeof__(guest) *program; float viewport[1][4];int scissor[1][4];} st;
static unsigned pass_w=1440,pass_h=810,pass_samples=1,count=4,icount=9,inst=1;
static bool use_index;
#include "production_draw_metadata.inc"
static void captured_draw(Prog *pr,Pipe *pp) {
#include "production_draw_join.inc"
}
static void test_join(ShaderInfo *info) {
    Prog p={.vi=info,.fi=info,.fp={{0x111,1},{0x222,1}},.chash=0x555};Pipe pipe={.k={0x777}};
    st.program=&guest;guest.stages[0].code_addr=0x1000;guest.stages[1].code_addr=0x2000;
    st.viewport[0][2]=1440;st.viewport[0][3]=810;st.scissor[0][2]=1440;st.scissor[0][3]=810;
    pg_state=0;pg_n=267;draws_enabled=true;captured_draw(&p,&pipe);CHECK(!log_at&&!key_calls&&!pg_verts);
    pg_state=1;draws_enabled=false;captured_draw(&p,&pipe);CHECK(!log_at&&!key_calls&&!pg_verts);
    pg_state=2;draws_enabled=true;captured_draw(&p,&pipe);CHECK(!log_at&&!key_calls&&!pg_verts);
    pg_state=1;pg_n=0;captured_draw(&p,&pipe);CHECK(!log_at&&!key_calls&&!pg_verts);
    pg_n=267;captured_draw(&p,&pipe);CHECK(pg_verts==4&&pg_vc[266]==4&&key_calls==1);
    CHECK(strstr(log_text,"[drawmeta] frame 1400 mark 266 program 0000000000000555 pipeline 0000000000000777 target 1440x810"));
    CHECK(strstr(log_text,"stage vs guest 1000 hash 0000000000000111 fingerprint_state 1"));
    CHECK(strstr(log_text,"stage fs guest 2000 hash 0000000000000222 fingerprint_state 1"));
    log_at=0;log_text[0]=0;p.fp[0]=(MetaFp){0,0};p.fp[1]=(MetaFp){0,0};p.prep_vh=0x333;p.prep_fh=0x444;
    use_index=true;inst=2;captured_draw(&p,&pipe);CHECK(pg_vc[266]==18);
    CHECK(strstr(log_text,"stage vs guest 1000 hash 0000000000000333 fingerprint_state 0"));
    CHECK(strstr(log_text,"stage fs guest 2000 hash 0000000000000444 fingerprint_state 0"));
    log_at=0;log_text[0]=0;p.fp[1]=(MetaFp){0x666,2};captured_draw(&p,&pipe);
    CHECK(strstr(log_text,"stage fs guest 2000 hash 0000000000000666 fingerprint_state 2"));
    ShaderInfo boundary=*info;boundary.flow_kind=255;boundary.ntex=64;boundary.tex[63]=(ShaderTexture){.type=31,.shadow=true,.is_image=true,.bindless=true,.is_uint=true};
    log_at=0;log_text[0]=0;pg_log_shader_metadata("fs",0x2000,0,2,&boundary);
    CHECK(strstr(log_text,"flow unknown"));CHECK(strstr(log_text,"types 80000001 shadow 8000000000000000 image 8000000000000000 bindless 8000000000000000 uint 8000000000000000"));
    size_t before=log_at;unsigned repeated=pg_log_shader_metadata("fs",0x2000,0,2,&boundary);CHECK(log_at==before&&repeated);
    pg_log_shader_metadata("fs",0,0,0,NULL);CHECK(log_at==before);
    log_at=0;log_text[0]=0;captured_draw(&p,&pipe);
    CHECK(strstr(log_text,"[drawmeta]"));CHECK(!strstr(log_text,"[shadermeta]"));
    CHECK(strstr(log_text,"vs 1000 hash 0000000000000333 fp 0 metadata"));
    CHECK(strstr(log_text,"fs 2000 hash 0000000000000666 fp 2 metadata"));
}
#endif
static ShaderInfo *fixture(unsigned kind) {
    _Alignas(16) uint8_t blob[0x180]={0};uint64_t words[24]={0};size_t n=4;
    words[1]=UINT64_C(0x0100000000070000)|(UINT64_C(0x3f800000)<<20);
    words[2]=UINT64_C(0xe30000000007000f);words[3]=UINT64_C(0x50b0000000070f00);
    if(kind==1) {
        words[2]=UINT64_C(0xe24000000000000f)|(UINT64_C(24)<<20); // p0: jump from word2 to word6
        words[3]=UINT64_C(0x0100000000070001)|(UINT64_C(0x40000000)<<20);
        words[5]=UINT64_C(0x0100000000070002)|(UINT64_C(0x40400000)<<20);
        words[6]=UINT64_C(0xe30000000007000f);words[7]=UINT64_C(0x50b0000000070f00);n=8;
    } else if(kind==2)words[1]=UINT64_C(0xef4400000007ff00); // LDL.32 r0,[RZ]
    else if(kind==3 || kind==4) {
        words[1]=UINT64_C(0x5d10000000070000)|(UINT64_C(1)<<8)|(UINT64_C(2)<<20);
        if(kind==4)words[1]|=(UINT64_C(1)<<47)|(UINT64_C(1)<<28);
    }
    uint32_t output=15;memcpy(blob+0x78,&output,4);memcpy(blob+0x80,words,n*8);
    ShaderKey key={.glsl=1};return maxwell_translate_sized(blob,0x80+n*8,0,STAGE_FRAGMENT,&key);
}
int main(int argc,char **argv) {
    CHECK(argc==4);maxwell_half_native=atoi(argv[2]);unsigned expected=(unsigned)atoi(argv[3]);
    for(unsigned kind=0;kind<5;kind++) {
        ShaderInfo *info=fixture(kind);CHECK(info);if(!info->msl)fprintf(stderr,"%s\n",info->error);CHECK(info->msl);
#ifndef METADATA_BASELINE
        CHECK(info->flow_kind==(kind==1?expected:SHADER_FLOW_STRAIGHT));
        CHECK(info->structured==(kind==1&&(expected==SHADER_FLOW_NESTED||expected==SHADER_FLOW_FLAGS)));
        CHECK(info->register_count==declarations(info->msl,"r"));
        CHECK(info->half_register_count==declarations(info->msl,"h"));
        CHECK(info->half_conversion_count==declarations(info->msl,"hc"));
        CHECK(info->local_words==(kind==2?256:0));CHECK(strstr(info->msl,"uint lmem[256]")==NULL); // the no-store fixture reads only proven zeros
        CHECK(info->brx_count==0&&info->brx_tables==0);
        if(kind==1) {
            CHECK(info->branch_labels>=2);CHECK(info->num_insts==6);
            bool dynamic=getenv("LARECOMP_DYN_STACKS")!=NULL;CHECK(info->stacks_resolved==!dynamic&&info->static_stacks==!dynamic);
            CHECK((strstr(info->msl,"switch (pc)")!=NULL)==(expected==SHADER_FLOW_PC));
            CHECK((strstr(info->msl,"bool f0")!=NULL)==(expected==SHADER_FLOW_FLAGS));
            CHECK((strstr(info->msl,"uint ssy[16]")!=NULL)==dynamic);
        }
        if(kind==0)test_join(info);
#endif
        char path[1024];snprintf(path,sizeof path,"%s/%u.frag",argv[1],kind);FILE *f=fopen(path,"w");CHECK(f);CHECK(fputs(info->msl,f)>=0);CHECK(!fclose(f));maxwell_free(info);
    }
#ifndef METADATA_BASELINE
    ShaderInfo *many=calloc(257,sizeof *many);CHECK(many);unsigned truncations=0;
    for(unsigned i=0;i<257;i++) {
        log_at=0;log_text[0]=0;pg_log_shader_metadata("fs",i,i,1,&many[i]);
        if(strstr(log_text,"limit 256 reached"))truncations++;
    }
    CHECK(truncations==1);log_at=0;log_text[0]=0;
    CHECK(!pg_log_shader_metadata("fs",999,999,1,&many[256]));CHECK(!log_at);free(many);
#endif
    printf("{\"assertions\":%u,\"fixtures\":5}\n",checks);return 0;
}
