/* Asset-free LDC signed-displacement fixtures through the real translator. */
#include "shader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks,saved;
#define CHECK(v) do{++checks;if(!(v)){fprintf(stderr,"line%d: %s\n",__LINE__,#v);abort();}}while(0)
bool shared_title_las(void){return false;}
static ShaderInfo*translate(unsigned imm,unsigned size,unsigned form,unsigned mode,int glsl,int scalar){
 uint8_t blob[0xa0]={0};unsigned a=form==2?255:8;unsigned dest=form==3?8:4;uint64_t ldc=UINT64_C(0xef90000000070000)|dest|((uint64_t)a<<8)|((uint64_t)imm<<20)|(UINT64_C(8)<<36)|((uint64_t)mode<<44)|((uint64_t)size<<48);
 uint64_t code[]={0,UINT64_C(0x0100000000070008),ldc,UINT64_C(0xe30000000007000f)};
 if(form==0||form==3)code[1]=UINT64_C(0xefd800000807ff08);
 memcpy(blob+0x80,code,sizeof code);ShaderKey k={.glsl=glsl};maxwell_scalar_cbuf=scalar;return maxwell_translate_sized(blob,sizeof blob,0,STAGE_VERTEX,&k);
}
int main(int argc,char**argv){CHECK(argc==3);int robust=atoi(argv[2]);maxwell_robust_cbuf=1;
 unsigned imms[]={0,0x7fff,0x8000,0xffff,0xffe0,0xfff0,0xffe8,0xfff8,32};
 for(int scalar=0;scalar<2;scalar++)for(unsigned i=0;i<sizeof imms/sizeof*imms;i++)for(unsigned size=4;size<=5;size++)for(unsigned form=0;form<3;form++){
  ShaderInfo*s=translate(imms[i],size,form,0,1,scalar);if(!s->msl)fprintf(stderr,"%s\n",s->error);CHECK(s->msl);CHECK(s->cbuf_mask==(1u<<8));CHECK(!strstr(s->msl,"cb8[-"));if(form!=2||imms[i]>=0x8000)CHECK(s->cbuf_dynamic==(1u<<8));
  char p[1024];snprintf(p,sizeof p,"%s/ldc-%d-%d-%u-%u-%u.vert",argv[1],robust,scalar,imms[i],size,form);FILE*f=fopen(p,"w");CHECK(f);fputs(s->msl,f);CHECK(!fclose(f));saved++;maxwell_free(s);
 }
 for(unsigned mode=1;mode<4;mode++)for(int glsl=0;glsl<2;glsl++){ShaderInfo*s=translate(0xffe0,5,0,mode,glsl,0);CHECK(!s->msl);CHECK(strstr(s->error,"LDC address mode"));maxwell_free(s);}
 // A also names the first destination: snapshot before both writes.
 for(int glsl=0;glsl<2;glsl++){ShaderInfo*s=translate(0xffe0,5,3,0,glsl,0);CHECK(s->msl);const char*a=strstr(s->msl,"uint ldc_addr = r8;"),*b=strstr(s->msl,"r8 = cbr(cb8"),*d=strstr(s->msl,"r9 = cbr(cb8");CHECK(a&&b&&d&&a<b&&b<d);CHECK(strstr(b,"(ldc_addr) + -32u"));CHECK(strstr(d,"(ldc_addr) + -28u"));maxwell_free(s);}
 // MSL uses the same modular uint byte addition, without GLSL-only aligned rows.
 for(unsigned i=2;i<8;i++){ShaderInfo*s=translate(imms[i],5,0,0,0,0);CHECK(s->msl);char wanted[64];snprintf(wanted,sizeof wanted,"(r8) + %du",(int)imms[i]-65536);CHECK(strstr(s->msl,wanted));CHECK(strstr(s->msl,"cbr(cb8,"));maxwell_free(s);}
 printf("state=passed checks=%u GLSL_variants=%u robust=%d\n",checks,saved,robust);
}
