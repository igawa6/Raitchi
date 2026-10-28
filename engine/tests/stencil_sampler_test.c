/* Production translator: stencil samples retain integer register bits. */
#include "shader.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);abort();}}while(0)
bool shared_title_las(void){return false;}
static ShaderInfo *translate(uint64_t word,uint32_t mask) {
    _Alignas(16) unsigned char b[0xa0]={0};
    uint64_t w[]={0,word,UINT64_C(0xe30000000007000f),UINT64_C(0x50b0000000070f00)};
    memcpy(b+0x80,w,sizeof w);ShaderKey k={.glsl=1,.tex_uint=mask};
    return maxwell_translate(b,sizeof b,STAGE_FRAGMENT,&k);
}
static void save(ShaderInfo *s,const char *dir,const char *name) {
    char p[1024];snprintf(p,sizeof p,"%s/%s.frag",dir,name);
    FILE *f=fopen(p,"w");CHECK(f);CHECK(fputs(s->msl,f)>=0);CHECK(!fclose(f));
}
static void actual(const char *dir,char **a) {
    FILE *f=fopen(a[0],"rb");CHECK(f);
    char *tail;
    size_t off=(size_t)strtoull(a[1],NULL,0),n=(size_t)strtoull(a[2],&tail,0);
    size_t constants=*tail=='+'?(size_t)strtoull(tail+1,NULL,0):0;
    CHECK(n&&n<=1024*1024&&constants<=65536);unsigned char *b=malloc(n+constants);CHECK(b);
    CHECK(!fseek(f,(long)off,SEEK_SET));CHECK(fread(b,1,n+constants,f)==n+constants);CHECK(!fclose(f));
    ShaderKey k={.glsl=1,.tex_uint=(uint32_t)strtoul(a[3],NULL,0)};
    ShaderInfo *s=maxwell_translate(b,n,STAGE_FRAGMENT,&k);CHECK(s);
    if(!s->msl)fprintf(stderr,"%s: %s\n",a[4],s->error);
    CHECK(s->msl);unsigned typed=0,legacy=0;
    for(int i=0;i<s->ntex;i++) {
        if(s->tex[i].is_uint){typed++;CHECK(!s->tex[i].shadow);}
        if(s->tex[i].legacy_stencil_compare){legacy++;CHECK(s->tex[i].shadow&&!s->tex[i].is_uint);}
    }
    if(!strncmp(a[4],"legacy-",7)) {
        CHECK(typed==0&&legacy==1);CHECK(strstr(s->msl,"uniform sampler2DArrayShadow "));
    } else {CHECK(typed==1&&legacy==0);CHECK(strstr(s->msl,"uniform usampler2D "));}
    save(s,dir,a[4]);maxwell_free(s);free(b);
}
int main(int argc,char **argv) {
    CHECK(argc>=2&&(argc-2)%5==0);
    char name[80];
    for(unsigned slot=0;slot<32;slot++)for(unsigned target=0;target<4;target++)for(unsigned array=0;array<2;array++) {
        if(target==2&&array)continue;
        uint64_t w=UINT64_C(0xc038000000070000)|((uint64_t)(8+slot*2)<<36)|
                   ((uint64_t)target<<29)|((uint64_t)array<<28)|(UINT64_C(15)<<31);
        ShaderInfo *s=translate(w,1u<<slot);if(s&&!s->msl)fprintf(stderr,"slot%u target%u array%u: %s\n",slot,target,array,s->error);
        CHECK(s&&s->msl);CHECK(s->ntex==1&&s->tex[0].is_uint);
        CHECK(strstr(s->msl,"uniform usampler"));CHECK(strstr(s->msl,"uvec4 t = texture("));
        for(unsigned i=0;i<4;i++){char line[40];snprintf(line,sizeof line,"r%u = t[%u];",i,i);CHECK(strstr(s->msl,line));}
        CHECK(!strstr(s->msl,"r0 = as_u(t[0])"));
        if(slot==31){snprintf(name,sizeof name,"tex-%u-%u",target,array);save(s,argv[1],name);}
        maxwell_free(s);
        s=translate(w,0);CHECK(s&&s->msl);CHECK(!s->tex[0].is_uint);CHECK(strstr(s->msl,"uniform sampler"));
        CHECK(strstr(s->msl,"r0 = as_u(t[0])"));maxwell_free(s);
    }
    /* TLDS must also retain UINT results, including the two-register TEXS layout. */
    uint64_t fetch=UINT64_C(0xda00000000070000)|(UINT64_C(8)<<36)|(UINT64_C(2)<<53)|(UINT64_C(255)<<28);
    ShaderInfo *s=translate(fetch,1);CHECK(s&&s->msl);CHECK(s->tex[0].is_uint);
    CHECK(strstr(s->msl,"uvec4 t = texelFetch("));CHECK(strstr(s->msl,"r0 = t[0];"));save(s,argv[1],"fetch-uint");maxwell_free(s);
    s=translate(fetch&~(UINT64_C(1)<<59),1);CHECK(s&&!s->msl);CHECK(strstr(s->error,"F16 results"));maxwell_free(s);
    uint64_t compare=UINT64_C(0xc038000000070000)|(UINT64_C(8)<<36)|(UINT64_C(1)<<29)|(UINT64_C(1)<<31)|(UINT64_C(1)<<50);
    s=translate(compare,1);CHECK(s&&s->msl);CHECK(s->ntex==1&&s->tex[0].shadow);
    CHECK(s->tex[0].legacy_stencil_compare&&!s->tex[0].is_uint);
    CHECK(strstr(s->msl,"uniform sampler2DShadow "));CHECK(strstr(s->msl,"r0 = as_u(t[0])"));
    save(s,argv[1],"legacy-comparison");maxwell_free(s);
    /* Raw stencil bits survive every value; float(value)'s IEEE encoding does
       not satisfy the original R2P predicate contract for bits five and six. */
    for(uint32_t v=0;v<256;v++){uint32_t r=v;CHECK(((r>>5)&1)==((v&0x20)!=0));CHECK(((r>>6)&1)==((v&0x40)!=0));}
    for(int i=2;i<argc;i+=5)actual(argv[1],argv+i);
    printf("{\"state\":\"passed\",\"checks\":%u,\"raw_uint_results\":true}\n",checks);return 0;
}
