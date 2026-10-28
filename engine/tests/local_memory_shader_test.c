/* Asset-free synthetic local-memory programs through the production translator. */
#include "shader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
bool shared_title_las(void){return false;}
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);abort();}}while(0)
typedef struct{uint64_t w[160];unsigned n;} Code;
static unsigned next(Code*c){return c->n%4?c->n:c->n+1;}
static unsigned push(Code*c,uint64_t w){if(c->n%4==0)c->w[c->n++]=0;unsigned at=c->n;c->w[c->n++]=w;return at;}
static void local(Code*c,bool store,unsigned size,unsigned base,int off,unsigned data,unsigned predicate){
    push(c,(store?UINT64_C(0xef50000000000000):UINT64_C(0xef40000000000000))|((uint64_t)size<<48)|
        ((uint64_t)((uint32_t)off&0xffffff)<<20)|((uint64_t)predicate<<16)|((uint64_t)base<<8)|data);
}
static void branch(Code*c,unsigned at,unsigned target){c->w[at]=UINT64_C(0xe24000000000000f)|((uint64_t)((target*8-(at*8+8))&0xffffff)<<20);}
int main(int argc,char**argv){
    CHECK(argc==2);char path[1024];snprintf(path,sizeof path,"%s/index.txt",argv[1]);FILE*index=fopen(path,"w");CHECK(index);
    for(unsigned kind=0;kind<15;kind++){
        Code c={0};
        if(kind==0||kind==3||kind==6){local(&c,true,6,255,kind==3?-4:0,0,7);local(&c,false,5,kind==6?4:31,0,4,7);local(&c,false,4,kind==6?6:31,8,6,7);}
        else if(kind==1){local(&c,true,4,255,0,0,7);local(&c,true,0,255,1,1,7);local(&c,true,2,255,2,2,7);for(unsigned s=0;s<5;s++)local(&c,false,s,31,s<2?1:s<4?2:0,8+s,7);}
        else if(kind==2){local(&c,true,2,255,3,0,7);local(&c,true,0,255,1023,1,7);for(unsigned s=0;s<5;s++)local(&c,false,s,31,3,8+s,7);}
        else if(kind==4){for(unsigned s=0;s<8;s++)local(&c,false,s,31,0,8+s*4,7);}
        else if(kind==5){local(&c,true,7,255,0,255,7);local(&c,false,5,31,0,255,7);local(&c,false,6,31,0,4,7);}
        else if(kind==7){local(&c,true,6,255,0,0,0);local(&c,false,6,31,0,4,7);}
        else if(kind==8||kind==9){local(&c,true,6,255,0,0,7);local(&c,true,6,31,0,8,kind==8?1:7);local(&c,false,6,31,0,4,7);}
        else if(kind==10||kind==11){for(unsigned i=0;i<(kind==10?17:16);i++)local(&c,true,4,255,(int)i*(kind==10?4:64),0,7);local(&c,false,6,31,0,4,7);}
        else if(kind==12){local(&c,true,4,255,0,0,7);local(&c,false,4,255,0,1,7);local(&c,true,4,255,4,1,7);local(&c,false,5,31,0,2,7);}
        else if(kind==13){unsigned at=push(&c,0);local(&c,true,6,255,0,0,7);branch(&c,at,next(&c));local(&c,false,6,31,0,4,7);}
        else if(kind==14){
            push(&c,UINT64_C(0x0100000000070014)|(UINT64_C(3)<<20));unsigned loop=next(&c);
            local(&c,true,4,255,0,20,7);
            push(&c,UINT64_C(0x1c00000000070014)|(UINT64_C(20)<<8)|(UINT64_C(0xffffffff)<<20));
            push(&c,UINT64_C(0x3660000000070007)|(UINT64_C(20)<<8)|(UINT64_C(7)<<39)|(UINT64_C(5)<<49));
            unsigned at=push(&c,0);branch(&c,at,loop);local(&c,false,4,31,0,4,7);
        }
        push(&c,UINT64_C(0xe30000000007000f));
        _Alignas(16) uint8_t blob[0x800]={0};memcpy(blob+0x80,c.w,c.n*8);ShaderKey key={.glsl=1};
        ShaderInfo*s=maxwell_translate_sized(blob,0x80+c.n*8,0,STAGE_FRAGMENT,&key);CHECK(s);if(!s->msl)fprintf(stderr,"%u:%s\n",kind,s->error);CHECK(s->msl);
        snprintf(path,sizeof path,"%s/%02u.frag",argv[1],kind);FILE*f=fopen(path,"w");CHECK(f);CHECK(fputs(s->msl,f)>=0);CHECK(!fclose(f));
        fprintf(index,"%u",kind);for(unsigned i=0;i<c.n;i++)fprintf(index," %016llx",(unsigned long long)c.w[i]);fputc('\n',index);maxwell_free(s);
    }
    CHECK(!fclose(index));printf("%u local-memory translation assertions passed\n",checks);return 0;
}
