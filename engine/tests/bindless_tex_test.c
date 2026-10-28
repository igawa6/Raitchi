/* Asset-free fixtures through the production translator; no guest assets. */
#include "shader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks, saved;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); abort(); } } while (0)
bool shared_title_las(void) { return false; }
static uint64_t origin(int d,int cb,unsigned off) { return UINT64_C(0x4c98078000070000)|d|((uint64_t)cb<<34)|((uint64_t)(off/4)<<20); }
static uint64_t tex(int target,int array,int lod,int dc,int b) { return UINT64_C(0xdeb8000000070000)|(uint64_t)32|((uint64_t)4<<8)|((uint64_t)b<<20)|((uint64_t)array<<28)|((uint64_t)target<<29)|(UINT64_C(15)<<31)|((uint64_t)lod<<37)|((uint64_t)dc<<50); }
static ShaderInfo *tr(const uint64_t *code,size_t n,int stage,int glsl) {
 unsigned char blob[4096]={0}; CHECK(n*8+0x80<=sizeof blob); memcpy(blob+0x80,code,n*8);
 ShaderKey k={.glsl=glsl,.vs_out_mask=~0u}; return maxwell_translate_sized(blob,0x80+n*8,0,stage,&k);
}
static void good(ShaderInfo*s) { if (!s->msl) fprintf(stderr,"unexpected: %s\n",s->error); CHECK(s->msl); }
static void bad(const uint64_t*c,size_t n,int stage,int glsl,const char*why) {
 ShaderInfo*s=tr(c,n,stage,glsl); CHECK(!s->msl); if(!strstr(s->error,why))fprintf(stderr,"expected %s got %s\n",why,s->error);CHECK(strstr(s->error,why));maxwell_free(s);
}
static void save(const char *dir,ShaderInfo*s,int stage) { char p[1024];snprintf(p,sizeof p,"%s/synthetic-%u.%s",dir,saved++,stage==0?"vert":"frag");FILE*f=fopen(p,"w");CHECK(f);fputs(s->msl,f);CHECK(!fclose(f)); }
int main(int argc,char**argv) {
 CHECK(argc==2); uint64_t end=UINT64_C(0xe30000000007000f),c[128]={0};
 // All admitted sampler types/LOD/compare combinations must produce valid SPIR-V.
 for(int stage=0;stage<2;stage++)for(int target=1;target<=2;target++)for(int array=0;array<2;array++)for(int dc=0;dc<2;dc++)for(int lod=0;lod<4;lod++) {
  if(target==2&&(array||dc))continue;
  memset(c,0,sizeof c);c[1]=origin(20,7,0x80);c[2]=tex(target,array,lod,dc,20);c[3]=end;
  ShaderInfo*s=tr(c,4,stage,1);good(s);CHECK(s->ntex==1);ShaderTexture*t=&s->tex[0];
  CHECK(t->bindless&&!t->is_image&&!t->is_uint);CHECK(t->bindless_cbuf==7&&t->bindless_offset==0x80);
  CHECK(t->type==(target==2?TEXTYPE_3D:array?TEXTYPE_2D_ARRAY:TEXTYPE_2D));CHECK(t->shadow==dc);
  if(lod>=2)CHECK(strstr(s->msl,"as_f(r21)")); // B is HANDLE, B+1 is LOD
  if(dc)CHECK(strstr(s->msl,lod>=2?"as_f(r22)":"as_f(r21)"));
  if(array)CHECK(strstr(s->msl,"r4 & 0xffffu"));
  save(argv[1],s,stage);maxwell_free(s);
 }
 memset(c,0,sizeof c);c[1]=origin(20,6,0xb8);c[2]=tex(1,0,0,0,20);c[3]=end;
 bad(c,4,0,0,"requires GLSL");
 c[1]=0x50b0000000070f00ull;bad(c,4,1,1,"no proven");
 c[1]=origin(20,7,0x80)&~(UINT64_C(7)<<16);bad(c,4,1,1,"no proven");
 c[1]=origin(20,7,0x80);c[2]=UINT64_C(0x0100000000070014);c[3]=tex(1,0,0,0,20);c[5]=end;bad(c,6,1,1,"no proven");
 c[2]=UINT64_C(0xe240000000070000);bad(c,6,1,1,"no proven");
 c[2]=UINT64_C(0x5c98078001470015);c[3]=tex(1,0,0,0,21);
 ShaderInfo*s=tr(c,6,1,1);good(s);CHECK(s->tex[0].bindless_offset==0x80);save(argv[1],s,1);maxwell_free(s);
 // LDC.32 is a proven origin; LDC.64/dynamic offsets are deliberately not.
 memset(c,0,sizeof c);c[1]=UINT64_C(0xef90000000070000)|(UINT64_C(4)<<48)|(UINT64_C(7)<<36)|(UINT64_C(0x80)<<20)|(UINT64_C(255)<<8)|20;c[2]=tex(1,0,0,0,20);c[3]=end;
 s=tr(c,4,1,1);good(s);CHECK(s->tex[0].bindless_cbuf==7);maxwell_free(s);
 c[1]^=UINT64_C(1)<<48;bad(c,4,1,1,"no proven");
 c[1]=origin(20,7,0x80);
 for(int target=0;target<4;target++)for(int array=0;array<2;array++)for(int dc=0;dc<2;dc++)if(target==0||target==3||(target==2&&(array||dc))) {c[2]=tex(target,array,0,dc,20);bad(c,4,1,1,"unsupported");}
 for(int lod=4;lod<8;lod++){c[2]=tex(1,0,lod,0,20);bad(c,4,1,1,"unsupported");}
 c[2]=tex(1,0,0,0,20)|(UINT64_C(1)<<36);bad(c,4,1,1,"unsupported");
 c[2]=tex(1,0,0,0,20)|(UINT64_C(1)<<40);bad(c,4,1,1,"unsupported");
 c[2]=tex(1,0,0,0,20)|(UINT64_C(1)<<35);bad(c,4,1,1,"unsupported");
 // Existing TEX.B opcode dispatch admits PT only; sparse predicate encodings
 // must stay rejected before the emitter rather than silently losing a result.
 for(int p=0;p<7;p++){c[2]=(tex(1,0,0,0,20)&~(UINT64_C(7)<<51))|((uint64_t)p<<51);bad(c,4,1,1,"instruction");}
 c[2]=(tex(2,0,0,0,20)&~(UINT64_C(255)<<8))|(UINT64_C(254)<<8);bad(c,4,1,1,"range");
 c[2]=(tex(1,0,0,0,20)&~UINT64_C(255))|254;bad(c,4,1,1,"range");
 // Descriptor dedup, type conflicts, and resource capacity share the normal ntex limit.
 memset(c,0,sizeof c);c[1]=origin(20,7,0x80);c[2]=c[3]=tex(1,0,0,0,20);c[5]=end;
 s=tr(c,6,1,1);good(s);CHECK(s->ntex==1);maxwell_free(s);
 c[3]=tex(2,0,0,0,20);bad(c,6,1,1,"two types");
 // Same offset in a different CB is a distinct sampler, not a false alias.
 c[3]=origin(20,6,0x80);c[5]=tex(1,0,0,0,20);c[6]=end;
 s=tr(c,7,1,1);good(s);CHECK(s->ntex==2);CHECK(s->tex[0].bindless_cbuf==7&&s->tex[1].bindless_cbuf==6);maxwell_free(s);
 size_t pos=0;memset(c,0,sizeof c);
 for(int i=0;i<32;i++){if(pos%4==0)pos++;c[pos++]=origin(20,7,4*i);if(pos%4==0)pos++;c[pos++]=tex(1,0,0,0,20);}if(pos%4==0)pos++;c[pos++]=end;bad(c,pos,1,1,"too many");
 printf("%u production TEX.B assertions; %u GLSL variants\n",checks,saved);return 0;
}
