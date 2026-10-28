/* Actual translator fixtures for the three observed SDK16 shader failures. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shader.h"
static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); abort(); } } while(0)
bool shared_title_las(void) { return true; }
static ShaderInfo *program(uint64_t word,int stage) {
    _Alignas(16) uint8_t blob[0x120]={0}; size_t offset=stage==STAGE_COMPUTE?0x100:0x80;
    if(stage==STAGE_COMPUTE){uint32_t magic=0x12345679;memcpy(blob,&magic,4);}
    uint64_t code[]={0,word,UINT64_C(0xe30000000007000f),UINT64_C(0x50b0000000070f00)};
    memcpy(blob+offset,code,sizeof code);ShaderKey key={.glsl=1};
    return maxwell_translate(blob,offset+sizeof code,stage,&key);
}
static ShaderInfo *emit(const char *dir,const char *name,uint64_t word,int stage) {
    ShaderInfo*s=program(word,stage);CHECK(s);if(!s->msl)fprintf(stderr,"%s: %s\n",name,s->error);CHECK(s->msl);
    char path[1024];snprintf(path,sizeof path,"%s/%s.%s",dir,name,stage==STAGE_COMPUTE?"comp":stage==STAGE_FRAGMENT?"frag":"vert");
    FILE*f=fopen(path,"w");CHECK(f);fputs(s->msl,f);CHECK(!fclose(f));return s;
}
static void rejected(uint64_t word,int stage,const char*why){ShaderInfo*s=program(word,stage);CHECK(s&&!s->msl);CHECK(strstr(s->error,why));maxwell_free(s);}
int main(int argc,char**argv){
 CHECK(argc==2);char name[80];
 maxwell_gmem_base=UINT64_C(0x1240000000);maxwell_gmem_size=0xd0000000;
 maxwell_gmem_mirror=1;maxwell_gmem_zero=0;maxwell_gfind_cache=1;
 maxwell_vote_stages=(1u<<STAGE_COMPUTE)|(1u<<STAGE_FRAGMENT);maxwell_frag_subgroups=1;
 ShaderInfo*s=emit(argv[1],"actual-vote",UINT64_C(0x50d8e38000070002),STAGE_COMPUTE);CHECK(strstr(s->msl,"subgroupBallot"));maxwell_free(s);
 s=emit(argv[1],"actual-hsetp",UINT64_C(0x7e0683bc0f071507),STAGE_VERTEX);CHECK(strstr(s->msl,"hy_"));CHECK(!strstr(s->msl,"subgroup_ballot :"));maxwell_free(s);
 for(int mode=0;mode<3;mode++)for(int stage=0;stage<2;stage++){
  uint64_t w=UINT64_C(0x50d8000000070002)|((uint64_t)mode<<48)|(UINT64_C(1)<<45)|(UINT64_C(7)<<39);
  snprintf(name,sizeof name,"vote-%d-%d",mode,stage);s=emit(argv[1],name,w,stage?STAGE_FRAGMENT:STAGE_COMPUTE);
  CHECK(strstr(s->msl,"gl_SubgroupInvocationID >> 5u"));CHECK(!strstr(s->msl,"subgroupAll("));maxwell_free(s);
 }
 rejected(UINT64_C(0x50dbe38000070002),STAGE_COMPUTE,"vote mode");
 maxwell_vote_stages=0;rejected(UINT64_C(0x50d8e38000070002),STAGE_COMPUTE,"vote requires");
 maxwell_vote_stages=~0u;maxwell_frag_subgroups=0;rejected(UINT64_C(0x50d8e38000070002),STAGE_FRAGMENT,"vote requires");
 maxwell_frag_subgroups=1;rejected(UINT64_C(0x50d8e38000070002),STAGE_VERTEX,"vote requires");
 // HSET and HSETP immediate: comparison and H_AND/BF flag fields must not alias immediate data.
 for(int setp=0;setp<2;setp++)for(int cop=0;cop<16;cop++)for(int flag=0;flag<2;flag++){
  uint64_t w=(setp?UINT64_C(0x7e00000000070007):UINT64_C(0x7c00000000070002))|(UINT64_C(21)<<8)|(UINT64_C(0x3bc0f)<<20)|(UINT64_C(7)<<39)|((uint64_t)cop<<49)|((uint64_t)flag<<53);
  snprintf(name,sizeof name,"hset-%d-%d-%d",setp,cop,flag);s=emit(argv[1],name,w,STAGE_VERTEX);maxwell_free(s);
 }
 // All 32-bit integer reductions, both signed and unsigned, uint32 mirror and int64 paths, CS and FS.
 for(int i64=0;i64<2;i64++)for(int stage=0;stage<2;stage++)for(int sign=0;sign<2;sign++)for(int op=0;op<8;op++){
  maxwell_shader_int64=i64;
  uint64_t w=UINT64_C(0xebf9000000070002)|((uint64_t)sign<<20)|((uint64_t)op<<23);
  snprintf(name,sizeof name,"red-%d-%d-%d-%d",i64,stage,sign,op);s=emit(argv[1],name,w,stage?STAGE_FRAGMENT:STAGE_COMPUTE);
  CHECK(s->uses_gmem&&s->uses_gmem_store&&s->uses_gmem_map);CHECK(strstr(s->msl,"gfind(uint(ga), 4u)"));CHECK(strstr(s->msl,"atomicCompSwap"));
  if(!i64)CHECK(!strstr(s->msl,"uint64_t"));maxwell_free(s);
 }
 maxwell_shader_int64=0;s=emit(argv[1],"actual-red",UINT64_C(0xebf9000003070002),STAGE_FRAGMENT);CHECK(strstr(s->msl,"(o_ | v_)"));maxwell_free(s);
 maxwell_gmem_zero=1;s=emit(argv[1],"actual-red-zero-mirror",UINT64_C(0xebf9000003070002),STAGE_FRAGMENT);
 CHECK(strstr(s->msl,"else { }"));CHECK(!strstr(s->msl,"GMCAS(wi_"));maxwell_free(s);maxwell_gmem_zero=0;
 maxwell_gmem_readonly=1;rejected(UINT64_C(0xebf9000003070002),STAGE_FRAGMENT,"read-only");maxwell_gmem_readonly=0;
 for(int size=2;size<8;size++)rejected(UINT64_C(0xebf9000003070002)|((uint64_t)size<<20),STAGE_COMPUTE,"red size");
 maxwell_gmem_mirror=0;rejected(UINT64_C(0xebf9000003070002),STAGE_COMPUTE,"uint32 shader global memory");
 // SUST requires a supported storage-image capability; its absence must still fail explicitly.
 rejected(UINT64_C(0xeb20138600f70018),STAGE_COMPUTE,"WriteWithoutFormat");
 // Imported heap/chunk helper variants must support real atomics, not a plain load/store emulation.
 maxwell_shader_int64=1;maxwell_gmem_size=0x1000000;maxwell_gmem_chunk_shift=20;
 for(int lazy=0;lazy<2;lazy++){
  maxwell_gmem_lazy=lazy;snprintf(name,sizeof name,"red-chunks-%d",lazy);
  s=emit(argv[1],name,UINT64_C(0xebf9000003070002),STAGE_FRAGMENT);CHECK(strstr(s->msl,"GMCAS"));maxwell_free(s);
 }
 printf("%u actual translator opcode/capability assertions passed\n",checks);return 0;
}
