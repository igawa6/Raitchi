/* Synthetic VMNMX and returning ATOM.ADD fixtures through the actual translator. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shader.h"
bool shared_title_las(void){return false;}
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
static ShaderInfo*one(uint64_t word,bool glsl){_Alignas(16) uint8_t b[0x120]={0};uint32_t h=0x12345679;memcpy(b,&h,4);uint64_t code[]={0,word,0xe30000000007000full,0x50b0000000070f00ull};memcpy(b+0x100,code,sizeof code);ShaderKey key={.glsl=glsl};return maxwell_translate(b,sizeof b,STAGE_COMPUTE,&key);}
static void output(const char*dir,const char*name,uint64_t word){ShaderInfo*s=one(word,true);CHECK(s);if(!s->msl)fprintf(stderr,"%s: %s\n",name,s->error);CHECK(s->msl);char p[1024];snprintf(p,sizeof p,"%s/%s.comp",dir,name);FILE*f=fopen(p,"w");CHECK(f);CHECK(fputs(s->msl,f)>=0&&!fclose(f));maxwell_free(s);}
static void reject(uint64_t word,bool glsl,const char*why){ShaderInfo*s=one(word,glsl);CHECK(s&&!s->msl&&strstr(s->error,why));maxwell_free(s);}
int main(int argc,char**argv){
 CHECK(argc>=2);maxwell_gmem_base=0x1240000000ull;maxwell_gmem_size=0xd0000000;maxwell_gmem_mirror=1;maxwell_gfind_cache=1;maxwell_vote_stages=~0u;maxwell_frag_subgroups=1;
 FILE*cases;char path[1024];snprintf(path,sizeof path,"%s/words.txt",argv[1]);cases=fopen(path,"w");CHECK(cases);unsigned number=0;
 uint64_t actual=0x3b7707e061172a0full;output(argv[1],"actual-vmnmx",actual);ShaderInfo*s=one(actual,false);CHECK(s&&s->msl);CHECK(strstr(s->msl,"max(int(uint(max(int(r42), int(r17)))), int(r15))"));maxwell_free(s);
 for(unsigned i=0;i<256;i++){
  uint64_t word=0x3a0000000007000full|(42ull<<8)|(17ull<<20)|(15ull<<39)|(1ull<<50)|((uint64_t)(i&7)<<36)|((uint64_t)((i>>3)&7)<<28)|((uint64_t)((i>>6)&1)<<48)|((uint64_t)((i>>7)&1)<<49)|((uint64_t)((i>>2)&1)<<56)|((uint64_t)(5+((i>>5)&1))<<51)|((uint64_t)((i>>1)&1)<<54);
  char name[64];snprintf(name,sizeof name,"case-%03u",number);output(argv[1],name,word);fprintf(cases,"%u %016llx\n",number++,(unsigned long long)word);
 }
 for(unsigned i=0;i<32;i++){
  unsigned immediate[]={0,0x7fff,0x8000,0xffff};uint64_t word=0x3a0000000007000full|(42ull<<8)|((uint64_t)immediate[i&3]<<20)|(15ull<<39)|((uint64_t)((i>>2)&1)<<49)|(3ull<<37)|((uint64_t)((i>>3)&1)<<48)|((uint64_t)((i>>2)&1)<<56)|((uint64_t)(5+((i>>3)&1))<<51)|((uint64_t)((i>>4)&1)<<54);
  char name[64];snprintf(name,sizeof name,"case-%03u",number);output(argv[1],name,word);fprintf(cases,"%u %016llx\n",number++,(unsigned long long)word);
 }
 CHECK(!fclose(cases));for(unsigned mode=0;mode<8;mode++)if(mode!=5&&mode!=6)reject((actual&~(7ull<<51))|((uint64_t)mode<<51),true,"VMNMX");reject(actual|(1ull<<47),true,"VMNMX");reject(actual|(1ull<<55),true,"VMNMX");
 // A returning atomic must capture source before writing an aliasing destination.
 maxwell_shader_int64=0;uint64_t atom=0xed03000000500005ull;output(argv[1],"actual-atom",atom);s=one(atom,true);CHECK(s&&s->uses_gmem_store&&s->uses_gmem_map&&s->msl);CHECK(strstr(s->msl,"atom_value_ = r5")&&strstr(s->msl,"r5 = atom_old_")&&strstr(s->msl,"ga <= 0xcffffffcu")&&strstr(s->msl,"gfind(uint(ga), 4u)"));maxwell_free(s);
 output(argv[1],"atom-dest-zr",(atom&~255ull)|255);output(argv[1],"atom-source-zr",(atom&~(255ull<<20))|(255ull<<20));
 for(unsigned operation=1;operation<16;operation++)reject(atom|((uint64_t)operation<<52),true,"ATOM");for(unsigned type=2;type<8;type++)reject((atom&~(7ull<<49))|((uint64_t)type<<49),true,"ATOM");reject(atom,false,"ATOM");maxwell_gmem_readonly=1;reject(atom,true,"ATOM");maxwell_gmem_readonly=0;
 maxwell_gmem_zero=1;output(argv[1],"atom-zero-miss",atom);maxwell_gmem_zero=0;maxwell_shader_int64=1;output(argv[1],"atom-int64-address",atom);maxwell_shader_int64=0;
 if(argc==4){FILE*f=fopen(argv[2],"rb");CHECK(f);CHECK(!fseek(f,0,SEEK_END));long n=ftell(f);CHECK(n>0&&!fseek(f,0,SEEK_SET));uint8_t*b=malloc((size_t)n);CHECK(b&&fread(b,1,(size_t)n,f)==(size_t)n&&!fclose(f));ShaderKey key={.glsl=1};
  for(unsigned address64=0;address64<2;address64++)for(unsigned zero=0;zero<2;zero++){maxwell_shader_int64=address64;maxwell_gmem_zero=zero;s=maxwell_translate(b,(size_t)n,STAGE_COMPUTE,&key);CHECK(s);if(!s->msl)fprintf(stderr,"actual shader: %s\n",s->error);CHECK(s->msl);snprintf(path,sizeof path,"%s/%s-i64%u-zero%u.comp",argv[1],argv[3],address64,zero);f=fopen(path,"w");CHECK(f&&fputs(s->msl,f)>=0&&!fclose(f));maxwell_free(s);}free(b);
 }
 printf("{\"state\":\"passed\",\"checks\":%u,\"vmnmx_variants\":%u}\n",checks,number);return 0;
}
