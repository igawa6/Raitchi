/* Asset-free Maxwell ISA field fixtures through the production translator. */
#include "shader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks,saved;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);abort();}}while(0)
bool shared_title_las(void){return false;}
static uint64_t full(unsigned d,unsigned a,unsigned index,unsigned component,unsigned mask){return UINT64_C(0xc800000000070000)|d|((uint64_t)a<<8)|(UINT64_C(255)<<20)|(UINT64_C(2)<<28)|((uint64_t)mask<<31)|((uint64_t)index<<36)|(UINT64_C(7)<<51)|((uint64_t)component<<56);}
static uint64_t swizzled(unsigned d0,unsigned d1,unsigned a,unsigned b,unsigned index,unsigned component){return UINT64_C(0xdf00000000070000)|d0|((uint64_t)a<<8)|((uint64_t)b<<20)|((uint64_t)d1<<28)|((uint64_t)index<<36)|((uint64_t)component<<52);}
static ShaderInfo*translate(uint64_t instruction,int stage,int glsl,int integer){uint8_t blob[0xa0]={0};uint64_t code[]={0,instruction,UINT64_C(0xe30000000007000f)};memcpy(blob+0x80,code,sizeof code);ShaderKey key={.glsl=glsl,.vs_out_mask=~0u,.tex_uint=integer?(1u<<2):0};return maxwell_translate_sized(blob,0x80+sizeof code,0,stage,&key);}
static void good(ShaderInfo*s){if(!s->msl)fprintf(stderr,"unexpected: %s\n",s->error);CHECK(s->msl);}
static void bad(uint64_t word,int glsl,const char*why){ShaderInfo*s=translate(word,STAGE_FRAGMENT,glsl,0);CHECK(!s->msl);CHECK(strstr(s->error,why));maxwell_free(s);}
static void save(const char*dir,ShaderInfo*s,int stage){char path[1024];snprintf(path,sizeof path,"%s/gather-%u.%s",dir,saved++,stage==STAGE_VERTEX?"vert":"frag");FILE*f=fopen(path,"w");CHECK(f);fputs(s->msl,f);CHECK(!fclose(f));}
int main(int argc,char**argv){CHECK(argc==2);
 for(int stage=0;stage<2;++stage)for(int form=0;form<2;++form)for(unsigned comp=0;comp<4;++comp)for(int integer=0;integer<2;++integer){
   uint64_t word=form?swizzled(16,24,4,5,12,comp):full(32,10,12,comp,13);ShaderInfo*s=translate(word,stage,1,integer);good(s);CHECK(s->ntex==1);ShaderTexture*t=&s->tex[0];CHECK(t->type==TEXTYPE_2D&&!t->shadow&&!t->is_array&&!t->bindless&&!t->is_image&&t->is_uint==integer&&t->handle_offset==48);CHECK(strstr(s->msl,"textureGather(t0"));char selector[16];snprintf(selector,sizeof selector,", %u)",comp);CHECK(strstr(s->msl,selector));CHECK(strstr(s->msl,integer?"uvec4 t = textureGather":"vec4 t = textureGather"));CHECK(strstr(s->msl,"as_f(r4)")||!form);CHECK(strstr(s->msl,"as_f(r5)")||!form);CHECK(strstr(s->msl,"as_f(r10)")||form);CHECK(strstr(s->msl,"as_f(r11)")||form);CHECK(s->uses_rscale==(stage!=STAGE_VERTEX));
   char output[80];unsigned destinations[4]={form?16:32,form?17:33,form?24:34,25};unsigned positions[4]={0,form?1:2,form?2:3,3};unsigned n=form?4:3;
   for(unsigned i=0;i<n;++i){snprintf(output,sizeof output,integer?"r%u = t[%u];":"r%u = as_u(t[%u]);",destinations[i],positions[i]);CHECK(strstr(s->msl,output));}save(argv[1],s,stage);maxwell_free(s);
 }
 // Every full-form mask packs selected quad positions into consecutive GPRs.
 for(unsigned mask=1;mask<16;++mask){ShaderInfo*s=translate(full(40,6,12,2,mask),STAGE_FRAGMENT,1,0);good(s);unsigned d=40;for(unsigned p=0;p<4;++p)if(mask&(1u<<p)){char expected[64];snprintf(expected,sizeof expected,"r%u = as_u(t[%u]);",d++,p);CHECK(strstr(s->msl,expected));}if(mask==15)save(argv[1],s,1);maxwell_free(s);}
 // Destination pairs overlap both coordinate sources: one complete vector
 // sample must be materialized before any GPR assignment.
 ShaderInfo*s=translate(swizzled(12,4,4,5,12,1),STAGE_FRAGMENT,1,0);good(s);const char*sample=strstr(s->msl,"textureGather(t0"),*overwrite=strstr(s->msl,"r4 = as_u(t[2]);");CHECK(sample&&overwrite&&sample<overwrite);CHECK(strstr(s->msl,"r5 = as_u(t[3]);"));save(argv[1],s,1);maxwell_free(s);
 s=translate(swizzled(16,255,255,5,12,0),STAGE_FRAGMENT,1,0);good(s);CHECK(strstr(s->msl,"as_f(0u)"));CHECK(strstr(s->msl,"r16 = as_u(t[0]);")&&strstr(s->msl,"r17 = as_u(t[1]);"));CHECK(!strstr(s->msl,"r255 ="));save(argv[1],s,1);maxwell_free(s);
 s=translate(swizzled(255,24,4,5,12,0),STAGE_FRAGMENT,1,0);good(s);CHECK(strstr(s->msl,"r24 = as_u(t[2]);")&&strstr(s->msl,"r25 = as_u(t[3]);"));save(argv[1],s,1);maxwell_free(s);
 s=translate(full(255,4,12,0,15),STAGE_FRAGMENT,1,0);good(s);CHECK(!s->ntex&&!strstr(s->msl,"textureGather("));maxwell_free(s);s=translate(full(32,4,12,0,0),STAGE_FRAGMENT,1,0);good(s);CHECK(!s->ntex);maxwell_free(s);
 uint64_t word=full(32,4,12,0,15);bad(word,0,"requires GLSL");bad(word|(UINT64_C(1)<<50),1,"unsupported");bad(word|(UINT64_C(1)<<54),1,"unsupported");bad(word|(UINT64_C(2)<<54),1,"unsupported");bad(word|(UINT64_C(1)<<35),1,"unsupported");bad(word&~(UINT64_C(7)<<51),1,"unsupported");bad((word&~(UINT64_C(7)<<28))|(UINT64_C(4)<<28),1,"unsupported");bad(full(254,4,12,0,3),1,"register group");bad(full(32,254,12,0,15),1,"register group");
 word=swizzled(16,24,4,5,12,0);bad(word,0,"requires GLSL");bad(word|(UINT64_C(1)<<55),1,"unsupported");bad(word|(UINT64_C(1)<<51),1,"unsupported");bad(word|(UINT64_C(1)<<50),1,"unsupported");bad(swizzled(254,24,4,5,12,0),1,"register group");bad(swizzled(16,254,4,5,12,0),1,"register group");
 printf("state=passed checks=%u GLSL_variants=%u; bound2D channel/quad masks, split destinations, overlap and explicit unsupported modes\n",checks,saved);
}
