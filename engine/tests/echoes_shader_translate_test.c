/* Synthetic Maxwell programs through the actual shared translator. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shader.h"
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);abort();}}while(0)
bool shared_title_las(void){return true;}
static uint64_t instruction(uint64_t base){return base|(UINT64_C(7)<<16)|(UINT64_C(8)<<8)|(UINT64_C(9)<<20);}
static ShaderInfo *program(uint64_t word,int modern,int glsl){
    _Alignas(16) uint8_t blob[0x120]={0};size_t offset=modern?0x100:0;
    if(modern){uint32_t magic=0x12345679;memcpy(blob,&magic,4);memset(blob+4,0xff,0xfc);}
    uint64_t code[4]={0,word,UINT64_C(0xe30000000007000f),UINT64_C(0x50b0000000070f00)};
    memcpy(blob+offset,code,sizeof code);ShaderKey key={0};key.glsl=glsl;
    return maxwell_translate(blob,offset+sizeof code,STAGE_COMPUTE,&key);
}
static void emit(const char *dir,const char *name,uint64_t word){
    ShaderInfo *legacy=program(word,0,1),*modern=program(word,1,1),*msl=program(word,1,0);
    CHECK(legacy&&modern&&msl);CHECK(legacy->msl&&modern->msl&&msl->msl);CHECK(!strcmp(legacy->msl,modern->msl));
    CHECK(strstr(msl->msl,"static inline uint brev32")!=NULL);CHECK(strstr(msl->msl,"static inline uint flo_sh")!=NULL);
    char path[1024];snprintf(path,sizeof path,"%s/%s.glsl",dir,name);FILE *file=fopen(path,"w");CHECK(file);fputs(modern->msl,file);CHECK(!fclose(file));
    maxwell_free(legacy);maxwell_free(modern);maxwell_free(msl);
}
int main(int argc,char **argv){
    CHECK(argc==2);char name[64];
    for(unsigned sign=0;sign<2;sign++)for(unsigned brev=0;brev<2;brev++){
        snprintf(name,sizeof name,"bfe-%u-%u",sign,brev);emit(argv[1],name,instruction(UINT64_C(0x5c00000000000000))|((uint64_t)sign<<48)|((uint64_t)brev<<40));
    }
    for(unsigned sign=0;sign<2;sign++)for(unsigned inv=0;inv<2;inv++)for(unsigned sh=0;sh<2;sh++){
        snprintf(name,sizeof name,"flo-%u-%u-%u",sign,inv,sh);emit(argv[1],name,instruction(UINT64_C(0x5c30000000000000))|((uint64_t)sign<<48)|((uint64_t)inv<<40)|((uint64_t)sh<<41));
    }
    for(unsigned shift=0;shift<32;shift++){
        snprintf(name,sizeof name,"lea-%u",shift);emit(argv[1],name,instruction(UINT64_C(0x5bd8000000000000))|(UINT64_C(10)<<39)|((uint64_t)shift<<28));
    }
    emit(argv[1],"lea-zero-high",instruction(UINT64_C(0x5bd8000000000000))|(UINT64_C(255)<<39)|(UINT64_C(31)<<28));
    for(unsigned bit=0;bit<3;bit++){
        unsigned field=bit==0?37:bit==1?38:47;ShaderInfo *s=program(instruction(UINT64_C(0x5bd8000000000000))|(UINT64_C(1)<<field),1,1);
        CHECK(s&&s->msl==NULL);CHECK(strstr(s->error,"lea.hi neg/x/cc")!=NULL);maxwell_free(s);
    }
    // Exact-sized allocations let ASan catch reads beyond a truncated SDK header.
    ShaderKey key={.glsl=1};
    for(size_t size=0;size<0x100;size++){
        uint8_t *blob=size?calloc(1,size):NULL;if(size>=4){uint32_t magic=0x12345679;memcpy(blob,&magic,4);}
        ShaderInfo *s=maxwell_translate(blob,size,STAGE_COMPUTE,&key);CHECK(s&&s->msl==NULL);CHECK(strstr(s->error,"truncated")!=NULL);maxwell_free(s);free(blob);
    }
    for(size_t size=0;size<0x80;size++){
        uint8_t *blob=size?calloc(1,size):NULL;ShaderInfo *s=maxwell_translate(blob,size,STAGE_FRAGMENT,&key);
        CHECK(s&&s->msl==NULL);maxwell_free(s);free(blob);
    }
    printf("%u actual translator/header assertions passed\n",checks);return 0;
}
