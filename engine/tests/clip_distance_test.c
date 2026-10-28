/* Asset-free SPH/output-store fixtures plus optional owner-captured vertex shader. */
#include "shader.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);abort();}}while(0)
bool shared_title_las(void){return false;}
static ShaderInfo *synthetic(unsigned declared,unsigned stores,bool predicated) {
    _Alignas(16) unsigned char b[0x200]={0};b[0x30+0x48]=(unsigned char)declared;
    size_t i=1;
    for(unsigned p=0;p<8;p++)if(stores&(1u<<p)){
        if(i%4==0)i++;
        uint64_t w=UINT64_C(0xeff0000000000000)|((uint64_t)(0x2c0+p*4)<<20)|((uint64_t)(predicated?0:7)<<16)|(p+1);
        memcpy(b+0x80+i++*8,&w,8);
    }
    if(i%4==0)i++;
    uint64_t exit_=UINT64_C(0xe30000000007000f),nop=UINT64_C(0x50b0000000070f00);
    memcpy(b+0x80+i++*8,&exit_,8);if(i%4==0)i++;
    memcpy(b+0x80+i++*8,&nop,8);ShaderKey k={.glsl=1};
    return maxwell_translate(b,0x80+i*8,STAGE_VERTEX,&k);
}
static void save(ShaderInfo*s,const char*dir,const char*name){char p[1024];snprintf(p,sizeof p,"%s/%s.vert",dir,name);FILE*f=fopen(p,"w");CHECK(f);CHECK(fputs(s->msl,f)>=0);CHECK(!fclose(f));}
int main(int argc,char**argv){
    CHECK(argc==2||argc==3);maxwell_clip_distance=1;
    for(unsigned mask=0;mask<256;mask++){
        ShaderInfo*s=synthetic(mask,255,false);CHECK(s&&s->msl);CHECK(s->clip_mask==mask);
        unsigned count=0;for(unsigned p=0;p<8;p++)if(mask&(1u<<p))count=p+1;
        char line[128];
        if(count){snprintf(line,sizeof line,"out float gl_ClipDistance[%u];",count);CHECK(strstr(s->msl,line));}
        else CHECK(!strstr(s->msl,"gl_ClipDistance"));
        for(unsigned p=0;p<8;p++){
            snprintf(line,sizeof line,"gl_ClipDistance[%u] = as_f(r%u);",p,p+1);CHECK((strstr(s->msl,line)!=NULL)==((mask&(1u<<p))!=0));
            snprintf(line,sizeof line,"gl_ClipDistance[%u] = 0.0f;",p);CHECK((strstr(s->msl,line)!=NULL)==(p<count));
        }
        if(mask==1||mask==0x80||mask==0x81||mask==255){char name[40];snprintf(name,sizeof name,"mask-%02x",mask);save(s,argv[1],name);}
        maxwell_free(s);
    }
    ShaderInfo*s=synthetic(1,2,false);CHECK(s&&s->msl&&!s->clip_mask);CHECK(!strstr(s->msl,"gl_ClipDistance"));maxwell_free(s);
    s=synthetic(0x81,0x80,true);CHECK(s&&s->msl&&s->clip_mask==0x80);CHECK(strstr(s->msl,"if (p0)"));CHECK(strstr(s->msl,"gl_ClipDistance[7] = as_f(r8);"));save(s,argv[1],"conditional-plane7");maxwell_free(s);
    maxwell_clip_distance=0;s=synthetic(1,1,false);CHECK(s&&!s->msl);CHECK(strstr(s->error,"shaderClipDistance"));maxwell_free(s);
    s=synthetic(0,255,false);CHECK(s&&s->msl&&!s->clip_mask);maxwell_free(s);maxwell_clip_distance=1;
    if(argc==3){
        FILE*f=fopen(argv[2],"rb");CHECK(f);unsigned char b[4372];CHECK(fread(b,1,sizeof b,f)==sizeof b);CHECK(fgetc(f)==EOF);CHECK(!fclose(f));
        CHECK(b[0x30+0x48]==1);ShaderKey k={.glsl=1};
        /* This exact capture ends at the declared20B constants. The legacy
           optimizer scans beyond that span looking for a table terminator.
           Exercise the existing generic BRX path without inventing padding. */
        maxwell_brx_table=false;s=maxwell_translate(b,4352,STAGE_VERTEX,&k);CHECK(s);
        if(!s->msl)fprintf(stderr,"actual producer: %s\n",s->error);CHECK(s->msl&&s->clip_mask==1);
        CHECK(strstr(s->msl,"out float gl_ClipDistance[1];"));CHECK(strstr(s->msl,"gl_ClipDistance[0] = as_f(r17);"));
        save(s,argv[1],"actual-stencil-producer");maxwell_free(s);
    }
    printf("{\"state\":\"passed\",\"checks\":%u,\"all_masks\":256,\"disabled_planes_neutral\":true}\n",checks);return 0;
}
