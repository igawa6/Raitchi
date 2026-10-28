/* Synthetic instruction/CPU fixtures; no native or original game execution. */
#include "control_step.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned assertions;
#define CHECK(x) do {++assertions;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static Cpu *cpu,*expected,*before;
static const uint64_t ticks=UINT64_C(0x12345678abcdef01);
static void reset(void) {
    memset(cpu,0xa5,sizeof *cpu);
    for(unsigned i=0;i<32;i++)cpu->x[i]=UINT64_C(0x1234000080000000)+i;
    cpu->pc=0xabc;cpu->sp=UINT64_C(0xabcdf000);cpu->nzcv=0;
    cpu->tpidr=UINT64_C(0x13579000);cpu->tpidrro=UINT64_C(0x2468a000);
    cpu->fpcr=0x07c00000;cpu->fpsr=0xf800009f;
    memcpy(expected,cpu,sizeof *cpu);memcpy(before,cpu,sizeof *cpu);
}
static void run(uint32_t word,uint64_t pc,uint64_t next,int rd,uint64_t value) {
    expected->pc=next;if(rd>=0)expected->x[rd]=value;
    CHECK(ngb_control_step(cpu,word,pc,ticks)==1);
    CHECK(!memcmp(cpu,expected,sizeof *cpu));
}
static uint32_t imm26(uint32_t base,int32_t words) {return base|((uint32_t)words&0x03ffffffu);}
static uint32_t imm19(uint32_t base,int32_t words) {return base|(((uint32_t)words&0x7ffffu)<<5);}
static uint32_t imm14(uint32_t base,int32_t words) {return base|(((uint32_t)words&0x3fffu)<<5);}
static uint32_t adr(uint32_t base,int32_t imm) {uint32_t u=(uint32_t)imm&0x1fffffu;return base|((u&3)<<29)|((u>>2)<<5);}
int main(void) {
    cpu=aligned_alloc(16,sizeof *cpu);expected=aligned_alloc(16,sizeof *cpu);before=aligned_alloc(16,sizeof *cpu);
    CHECK(cpu&&expected&&before);
    const uint64_t origins[]={0,0x1000,UINT64_C(0x123456789abc),UINT64_MAX-3};
    const int32_t words26[]={0,1,-1,0x1ffffff,-0x2000000};
    for(unsigned i=0;i<4;i++)for(unsigned j=0;j<5;j++) {
        reset();run(imm26(0x14000000,words26[j]),origins[i],origins[i]+(uint64_t)((int64_t)words26[j]*4),-1,0);
        reset();run(imm26(0x94000000,words26[j]),origins[i],origins[i]+(uint64_t)((int64_t)words26[j]*4),30,origins[i]+4);
    }
    for(unsigned reg=0;reg<32;reg++)for(unsigned op=0;op<3;op++) {
        const uint32_t bases[]={0xd61f0000,0xd63f0000,0xd65f0000};reset();
        if(reg==31) {CHECK(!ngb_control_step(cpu,bases[op]|(reg<<5),0x1000,ticks));CHECK(!memcmp(cpu,before,sizeof *cpu));}
        else {uint64_t destination=cpu->x[reg];run(bases[op]|(reg<<5),0x1000,destination,op==1?30:-1,0x1004);}
    }
    for(unsigned flags=0;flags<16;flags++)for(unsigned cond=0;cond<16;cond++) {
        unsigned n=flags>>3,z=(flags>>2)&1,c=(flags>>1)&1,v=flags&1;
        int truth[]={z,!z,c,!c,n,!n,v,!v,c&&!z,!c||z,n==v,n!=v,!z&&n==v,z||n!=v,1,1};
        for(unsigned backward=0;backward<2;backward++) {
            reset();cpu->nzcv=expected->nzcv=(uint64_t)flags<<28;
            int offset=backward?-0x40000:0x3ffff;
            uint64_t target=truth[cond]?0x1000+(uint64_t)((int64_t)offset*4):0x1004;
            run(imm19(0x54000000|cond,offset),0x1000,target,-1,0);
        }
    }
    for(unsigned sf=0;sf<2;sf++)for(unsigned op=0;op<2;op++)for(unsigned rt=0;rt<32;rt++) {
        reset();cpu->x[rt]=expected->x[rt]=UINT64_C(0x100000000); /* X nonzero, W zero. */
        uint64_t value=rt==31?0:sf?cpu->x[rt]:(uint32_t)cpu->x[rt];
        int take=(value!=0)==op;
        run(imm19(0x34000000|(sf<<31)|(op<<24)|rt,-1),0x1000,take?0xffc:0x1004,-1,0);
    }
    for(unsigned bit=0;bit<64;bit++)for(unsigned op=0;op<2;op++)for(unsigned present=0;present<2;present++) {
        reset();cpu->x[0]=expected->x[0]=present?(UINT64_C(1)<<bit):0;
        uint32_t word=0x36000000|((bit&32)<<26)|((bit&31)<<19)|(op<<24);
        int32_t offset=bit&1?-0x2000:0x1fff;
        run(imm14(word,offset),0x1000,present==op?0x1000+(uint64_t)((int64_t)offset*4):0x1004,-1,0);
        reset();run(imm14(word|31,offset),0x1000,!op?0x1000+(uint64_t)((int64_t)offset*4):0x1004,-1,0);
    }
    const int32_t adr_immediates[]={0,1,-1,0xfffff,-0x100000};
    for(unsigned page=0;page<2;page++)for(unsigned i=0;i<4;i++)for(unsigned j=0;j<5;j++)for(unsigned discard=0;discard<2;discard++) {
        reset();uint64_t pc=origins[i];
        uint64_t result=page?(pc&~UINT64_C(4095))+(uint64_t)((int64_t)adr_immediates[j]*4096):pc+(uint64_t)(int64_t)adr_immediates[j];
        run(adr((page?0x90000000u:0x10000000u)|(discard?31:0),adr_immediates[j]),pc,pc+4,discard?-1:0,result);
    }
    const uint32_t reads[]={0xd53bd040,0xd53bd060,0xd53b4400,0xd53b4420,0xd53b4200,0xd53be020,0xd53be040,0xd53be000};
    for(unsigned i=0;i<8;i++)for(unsigned reg=0;reg<32;reg++) {
        reset();cpu->nzcv=expected->nzcv=UINT64_C(0xfeedfacef0000000);
        uint64_t results[]={cpu->tpidr,cpu->tpidrro,cpu->fpcr,cpu->fpsr,0xf0000000,ticks,ticks,19200000};
        run(reads[i]|reg,0x1000,0x1004,reg==31?-1:(int)reg,results[i]);
    }
    const uint32_t writes[]={0xd51bd040,0xd51b4400,0xd51b4420,0xd51b4200};
    const uint64_t masks[]={UINT64_MAX,0x07c00000,0xf800009f,0xf0000000};
    for(unsigned i=0;i<4;i++)for(unsigned reg=0;reg<32;reg++) {
        reset();cpu->x[reg]=expected->x[reg]=UINT64_MAX;
        uint64_t value=reg==31?0:masks[i];
        if(i==0)expected->tpidr=value;if(i==1)expected->fpcr=value;
        if(i==2)expected->fpsr=value;if(i==3)expected->nzcv=value;
        run(writes[i]|reg,0x1000,0x1004,-1,0);
    }
    const uint32_t rejected[]={0xd503201f,0xd40004a1,0xd51bd060,0x54000010,0xffffffff,0xd69f03e0};
    for(unsigned i=0;i<sizeof rejected/sizeof rejected[0];i++) {
        reset();CHECK(!ngb_control_step(cpu,rejected[i],0x1000,ticks));CHECK(!memcmp(cpu,before,sizeof *cpu));
    }
    reset();CHECK(!ngb_control_step(cpu,0x14000000,1,ticks));CHECK(!memcmp(cpu,before,sizeof *cpu));
    CHECK(!ngb_control_step(NULL,0x14000000,0,ticks));
    free(cpu);free(expected);free(before);printf("control step assertions=%u\n",assertions);return 0;
}
