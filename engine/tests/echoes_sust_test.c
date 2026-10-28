/* Asset-free fixtures through the actual shared translator; no renderer stand-in claims. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shader.h"
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);abort();}}while(0)
bool shared_title_las(void){return true;}
static uint64_t origin(int dst,int cb,unsigned off){return UINT64_C(0x4c98078000070000)|(uint64_t)dst|((uint64_t)cb<<34)|((uint64_t)(off/4)<<20);}
static ShaderInfo* translate(const uint64_t*code,size_t count,int stage,int cap){
 _Alignas(16) unsigned char blob[0x200]={0};size_t off=stage==STAGE_COMPUTE?0x100:0x80;
 if(stage==STAGE_COMPUTE){uint32_t magic=0x12345679;memcpy(blob,&magic,4);}memcpy(blob+off,code,count*8);
 ShaderKey k={.glsl=1};maxwell_storage_image_write=cap;return maxwell_translate(blob,off+count*8,stage,&k);
}
static void save(const char*dir,const char*name,ShaderInfo*s){char path[1024];snprintf(path,sizeof path,"%s/%s.comp",dir,name);FILE*f=fopen(path,"w");CHECK(f);fputs(s->msl,f);CHECK(!fclose(f));}
static void bad(const uint64_t*code,size_t count,int stage,int cap,const char*why){ShaderInfo*s=translate(code,count,stage,cap);CHECK(s&&!s->msl);if(!strstr(s->error,why))fprintf(stderr,"expected %s got %s\n",why,s->error);CHECK(strstr(s->error,why));maxwell_free(s);}
int main(int argc,char**argv){
 CHECK(argc==2);uint64_t sust=UINT64_C(0xeb20138600f70018),end=UINT64_C(0xe30000000007000f);
 uint64_t code[8]={0,origin(39,2,0x120),sust,end};char name[100];
 for(int i64=0;i64<2;i64++)for(int dim=0;dim<6;dim++)if(dim!=1)for(int cache=0;cache<2;cache++){
  maxwell_shader_int64=i64;code[2]=(sust&~((UINT64_C(7)<<33)|(UINT64_C(3)<<24)))|((uint64_t)dim<<33)|((uint64_t)cache<<24);
  ShaderInfo*s=translate(code,4,STAGE_COMPUTE,1);CHECK(s&&s->msl);CHECK(s->ntex==1);
  ShaderTexture*t=&s->tex[0];CHECK(t->is_image&&t->bindless);CHECK(t->bindless_cbuf==2&&t->bindless_offset==0x120);
  CHECK(strstr(s->msl,"coherent writeonly uniform image"));CHECK(strstr(s->msl,"imageStore(t0"));CHECK(strstr(s->msl,"imageSize(t0)"));
  CHECK(!strstr(s->msl,"uniform sampler"));CHECK(strstr(s->msl,"r39 = cb2[18][0]"));
  snprintf(name,sizeof name,"sust-%d-%d-%d",i64,dim,cache);save(argv[1],name,s);maxwell_free(s);
 }
 code[2]=sust;
 bad(code,4,STAGE_COMPUTE,0,"WriteWithoutFormat");bad(code,4,STAGE_FRAGMENT,1,"compute GLSL");
 code[1]=origin(39,2,0x120)&~(UINT64_C(7)<<16);bad(code,4,STAGE_COMPUTE,1,"no proven");code[1]=origin(39,2,0x120);
 // Predicated/overwritten/unknown sources cannot be mistaken for a previously seen descriptor.
 code[2]=UINT64_C(0x0100000000070027);code[3]=sust;code[5]=end;bad(code,6,STAGE_COMPUTE,1,"no proven");
 code[2]=UINT64_C(0x5c98078002770026);code[3]=(sust&~(UINT64_C(255)<<39))|(UINT64_C(38)<<39);
 ShaderInfo*s=translate(code,6,STAGE_COMPUTE,1);CHECK(s&&s->msl);CHECK(s->tex[0].bindless_offset==0x120);save(argv[1],"sust-copy",s);maxwell_free(s);
 // Native fp16 shadows can overwrite a GPR without emitting rN=; both half/F2F writes must invalidate origins.
 maxwell_half_native=1;code[3]=sust;
 code[2]=UINT64_C(0x5d10000000070027);bad(code,6,STAGE_COMPUTE,1,"no proven");
 code[2]=UINT64_C(0x5ca8000000070927);bad(code,6,STAGE_COMPUTE,1,"no proven");maxwell_half_native=0;
 // Branch target clears provenance rather than reusing a fall-through-only fact.
 code[2]=UINT64_C(0xe240000000070000);code[3]=sust;bad(code,6,STAGE_COMPUTE,1,"no proven");
 memset(code,0,sizeof code);code[1]=origin(39,2,0x120);code[2]=sust;code[3]=end;
 for(int mask=0;mask<15;mask++){code[2]=(sust&~(UINT64_C(15)<<20))|((uint64_t)mask<<20);bad(code,4,STAGE_COMPUTE,1,"partial component");}
 code[2]=sust|(UINT64_C(1)<<52);bad(code,4,STAGE_COMPUTE,1,"sust.d");
 for(int clamp=1;clamp<4;clamp++){code[2]=sust|((uint64_t)clamp<<49);bad(code,4,STAGE_COMPUTE,1,"bounds mode");}
 for(int cache=2;cache<4;cache++){code[2]=sust|((uint64_t)cache<<24);bad(code,4,STAGE_COMPUTE,1,"cache mode");}
 for(int dim=1;dim<8;dim++)if(dim==1||dim>=6){code[2]=(sust&~(UINT64_C(7)<<33))|((uint64_t)dim<<33);bad(code,4,STAGE_COMPUTE,1,"dimension");}
 code[2]=(sust&~UINT64_C(255))|25;bad(code,4,STAGE_COMPUTE,1,"alignment/range");
 code[2]=(sust&~(UINT64_C(255)<<8))|(UINT64_C(254)<<8);bad(code,4,STAGE_COMPUTE,1,"alignment/range");
 // Bound addressing uses its encoded cbuf byteoffset, separate from sampled texture slots.
 code[1]=UINT64_C(0x50b0000000070f00);code[2]=((sust|(UINT64_C(1)<<51))&~(UINT64_C(0x1fff)<<36))|(UINT64_C(0x48)<<36);
 s=translate(code,4,STAGE_COMPUTE,1);CHECK(s&&s->msl&&s->ntex==1);CHECK(s->tex[0].is_image&&!s->tex[0].bindless);CHECK(s->tex[0].handle_offset==0x120);save(argv[1],"sust-bound",s);maxwell_free(s);
 printf("%u actual SUST translator/provenance/capability assertions passed\n",checks);return 0;
}
