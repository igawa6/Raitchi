/* Asset-free mixed F32/F16 instruction fixtures through the production emitter. */
#include "shader.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
bool shared_title_las(void) { return false; }
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static ShaderInfo *one_mode(uint64_t word,bool glsl,int stage) {
    _Alignas(16) unsigned char b[0x120] = {0}; uint32_t header=0x12345679;
    size_t offset=stage==STAGE_COMPUTE?0x100:0x80;
    if(stage==STAGE_COMPUTE)memcpy(b,&header,4); uint64_t code[]={0,word,0xe30000000007000full,0x50b0000000070f00ull};
    memcpy(b+offset,code,sizeof code); ShaderKey key={.glsl=glsl};
    return maxwell_translate(b,offset+sizeof code,stage,&key);
}
static ShaderInfo *one(uint64_t word){return one_mode(word,true,STAGE_COMPUTE);}
static void fixture(const char *dir,FILE *index,unsigned id,unsigned kind,unsigned a,unsigned b,unsigned c,unsigned merge,bool sat,bool neg) {
    uint64_t word=(kind==0?0x5d10000000070000ull:kind==1?0x5d08000000070000ull:0x5d00000000070000ull)|
        (1ull<<8)|(2ull<<20)|((uint64_t)a<<47)|((uint64_t)b<<28)|((uint64_t)merge<<49)|((uint64_t)sat<<32);
    if(kind==2)word|=(3ull<<39)|((uint64_t)c<<35);
    if(neg)word|=1ull<<31;
    ShaderInfo *s=one(word); CHECK(s); if(!s->msl)fprintf(stderr,"%016llx %s\n",(unsigned long long)word,s->error); CHECK(s->msl);
    bool all_f32=a==1&&b==1&&(kind!=2||c==1);
    bool mixed=(a==1||b==1||(kind==2&&c==1))&&(!all_f32||merge!=0);
    if(mixed) {
        CHECK(strstr(s->msl,"vec2(uintBitsToFloat(r")!=NULL);
        CHECK(strstr(s->msl,"float16_t(uintBitsToFloat(r")==NULL);
        if(!(all_f32&&merge==1)) CHECK(strstr(s->msl,maxwell_half_native?"hpack(f16vec2(":"hpack(unpackHalf2x16(packHalf2x16(")!=NULL);
    }
    char path[1024]; snprintf(path,sizeof path,"%s/%03u.comp",dir,id); FILE *f=fopen(path,"w");CHECK(f); CHECK(fputs(s->msl,f)>=0); CHECK(!fclose(f));
    fprintf(index,"%u %u %u %u %u %u %u %u %016llx %u\n",id,kind,a,b,c,merge,sat,neg,(unsigned long long)word,mixed); maxwell_free(s);
}
/* Cbuf compare field expectations checked against the yuzu/Eden decoder
 * (Copyright 2021 yuzu Emulator Project, GPL-2.0-or-later). Independent fixtures. */
static void compare_fixture(const char *dir,FILE *index,unsigned id,bool setp,unsigned cop,
        unsigned bop,unsigned swizzle,unsigned flag,unsigned modifiers,unsigned pred,unsigned pa,unsigned pb) {
    bool neg_a=modifiers&1,abs_a=modifiers&2,neg_b=modifiers&4,abs_b=setp&&(modifiers&8);
    bool ftz=modifiers&16,neg_pred=modifiers&32;
    unsigned slot=(cop+swizzle)%18,offset=17+cop*4+swizzle;
    if(id==0){slot=17;offset=0x3fff;}
    uint64_t word=(setp?UINT64_C(0x7e80000000070000):UINT64_C(0x7c80000000070000))|
        (UINT64_C(1)<<8)|((uint64_t)slot<<34)|((uint64_t)offset<<20)|((uint64_t)pred<<39)|
        ((uint64_t)neg_pred<<42)|((uint64_t)neg_a<<43)|((uint64_t)abs_a<<44)|((uint64_t)bop<<45)|
        ((uint64_t)swizzle<<47)|((uint64_t)cop<<49)|((uint64_t)flag<<53)|((uint64_t)neg_b<<56);
    if(setp)word|=((uint64_t)pa<<3)|pb|((uint64_t)abs_b<<54)|((uint64_t)ftz<<6);
    else word|=((uint64_t)ftz<<54)|pa;
    ShaderInfo *s=one(word);CHECK(s);if(!s->msl)fprintf(stderr,"compare %016llx: %s\n",(unsigned long long)word,s->error);CHECK(s->msl);
    CHECK(s->cbuf_mask==(1u<<slot));char address[80];snprintf(address,sizeof address,"cb%u[%u][%u]",slot,offset/4,offset%4);
    CHECK(strstr(s->msl,address));CHECK(strstr(s->msl,"vec2(uintBitsToFloat(cb"));
    CHECK(!strstr(s->msl,"float16_t(uintBitsToFloat(cb"));
    char path[1024];snprintf(path,sizeof path,"%s/cmp%04u.comp",dir,id);FILE *f=fopen(path,"w");CHECK(f);CHECK(fputs(s->msl,f)>=0);CHECK(!fclose(f));
    fprintf(index,"%u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %016llx\n",id,setp,cop,bop,swizzle,flag,
        neg_a,abs_a,neg_b,abs_b,ftz,neg_pred,pred,pa,pb,(unsigned long long)word);maxwell_free(s);
}
static void compare_rejected(uint64_t word) {
    ShaderInfo *s=one(word);CHECK(s&&!s->msl);CHECK(strstr(s->error,"half cbuf binding/boolean operation"));maxwell_free(s);
}
int main(int argc,char **argv) {
    CHECK(argc==3 || (argc==4 && !strcmp(argv[3],"compare-only"))); maxwell_half_native=atoi(argv[2]); maxwell_shader_int64=0;
    char path[1024];snprintf(path,sizeof path,"%s/index.txt",argv[1]);FILE *f=fopen(path,"w");CHECK(f);unsigned id=0;
    ShaderInfo *s;
    if(argc==3) {
    // Every operand swizzle, merge, and operation. Include same-precision controls.
    for(unsigned kind=0;kind<3;kind++)for(unsigned merge=0;merge<4;merge++)for(unsigned a=0;a<4;a++) {
        unsigned b=(a+1)%4,c=(a+2)%4;
        fixture(argv[1],f,id++,kind,a,b,c,merge,false,false);
        fixture(argv[1],f,id++,kind,1,kind==2?1:2,kind==2?2:0,merge,true,true);
        fixture(argv[1],f,id++,kind,1,1,1,merge,false,false);
        fixture(argv[1],f,id++,kind,0,2,3,merge,false,false);
    }
    // Guarded all-F32 FMA must bypass native half hpack even with shadows off.
    uint64_t guarded=0x5d00000000000000ull|(1ull<<8)|(2ull<<20)|(3ull<<39)|
        (1ull<<47)|(1ull<<28)|(1ull<<35)|(1ull<<49);
    s=one(guarded);CHECK(s&&s->msl);
    CHECK(strstr(s->msl,"if (p0)")!=NULL);
    CHECK(strstr(s->msl,"r0 = floatBitsToUint((fma(vec2(uintBitsToFloat(r1))")!=NULL);
    snprintf(path,sizeof path,"%s/guarded.comp",argv[1]);FILE *g=fopen(path,"w");CHECK(g);
    CHECK(fputs(s->msl,g)>=0);CHECK(!fclose(g));maxwell_free(s);
    }
    CHECK(!fclose(f));snprintf(path,sizeof path,"%s/compare-index.txt",argv[1]);f=fopen(path,"w");CHECK(f);unsigned comparisons=0;
    for(unsigned setp=0;setp<2;setp++)for(unsigned cop=0;cop<16;cop++)for(unsigned bop=0;bop<3;bop++)
        for(unsigned swizzle=0;swizzle<4;swizzle++)for(unsigned flag=0;flag<2;flag++)
            compare_fixture(argv[1],f,comparisons++,setp,cop,bop,swizzle,flag,0,2,0,1);
    for(unsigned setp=0;setp<2;setp++)for(unsigned cop=0;cop<16;cop++)for(unsigned variant=0;variant<4;variant++)
        compare_fixture(argv[1],f,comparisons++,setp,cop,variant%3,variant,variant&1,
            variant==0?63:variant==1?5:variant==2?26:48,variant&1?0:7,0,1);
    // Destination aliases, PT/RZ writes, and a predicate that is also a destination.
    for(unsigned flag=0;flag<2;flag++)for(unsigned dest=0;dest<3;dest++) {
        compare_fixture(argv[1],f,comparisons++,true,4,2,0,flag,0,0,dest==2?7:0,dest==0?0:1);
        compare_fixture(argv[1],f,comparisons++,false,5,1,1,flag,0,7,255,1);
    }
    CHECK(!fclose(f));
    compare_rejected(UINT64_C(0x7e80000000070001)|(UINT64_C(18)<<34));
    compare_rejected(UINT64_C(0x7c80000000070000)|(UINT64_C(3)<<45));
    for(unsigned setp=0;setp<2;setp++) {
        uint64_t word=(setp?UINT64_C(0x7e80000000070001):UINT64_C(0x7c80000000070000))|(UINT64_C(1)<<8)|(UINT64_C(4)<<49);
        s=one_mode(word,false,STAGE_FRAGMENT);CHECK(s&&s->msl);CHECK(strstr(s->msl,"hy_ = float2(as_type<float>(cb"));maxwell_free(s);
    }
    printf("{\"checks\":%u,\"fixtures\":%u,\"compare_fixtures\":%u,\"native_half\":%d}\n",checks,id,comparisons,maxwell_half_native); return 0;
}
