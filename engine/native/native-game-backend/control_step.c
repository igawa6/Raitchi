#include "control_step.h"

static uint64_t displacement(uint64_t immediate,unsigned bits,unsigned scale) {
    uint64_t sign=UINT64_C(1)<<(bits-1);
    return ((immediate^sign)-sign)<<scale;
}
static uint64_t read_zero(const Cpu *cpu,unsigned reg) {return reg==31?0:cpu->x[reg];}
static int condition(unsigned code,uint64_t nzcv) {
    unsigned n=(unsigned)(nzcv>>31)&1,z=(unsigned)(nzcv>>30)&1;
    unsigned c=(unsigned)(nzcv>>29)&1,v=(unsigned)(nzcv>>28)&1;
    int result;
    switch(code>>1) {
    case 0:result=z;break;case 1:result=c;break;case 2:result=n;break;case 3:result=v;break;
    case 4:result=c&&!z;break;case 5:result=n==v;break;case 6:result=!z&&(n==v);break;
    default:result=1;break;
    }
    return (code&1)&&code!=15?!result:result;
}
int ngb_control_step(Cpu *cpu,uint32_t word,uint64_t pc,uint64_t ticks) {
    if(!cpu || (pc&3)) return 0;
    uint64_t next=pc+4,value=0;unsigned reg=word&31;
    if((word&0x7c000000u)==0x14000000u) { /* B/BL immediate */
        if(word>>31)cpu->x[30]=next;
        cpu->pc=pc+displacement(word&0x03ffffffu,26,2);return 1;
    }
    uint32_t branch=word&0xfffffc1fu;
    if(branch==0xd61f0000u || branch==0xd63f0000u || branch==0xd65f0000u) {
        unsigned rn=(word>>5)&31;if(rn==31)return 0;
        uint64_t target=cpu->x[rn]; /* BLR X30 must read before writing LR. */
        if(branch==0xd63f0000u)cpu->x[30]=next;
        cpu->pc=target;return 1;
    }
    if((word&0xff000010u)==0x54000000u) { /* B.cond, excluding FEAT_HBC BC.cond */
        cpu->pc=condition(word&15,cpu->nzcv)?pc+displacement((word>>5)&0x7ffffu,19,2):next;return 1;
    }
    if((word&0x7e000000u)==0x34000000u) { /* CBZ/CBNZ, W or X. */
        value=read_zero(cpu,reg);if(!(word>>31))value=(uint32_t)value;
        int take=(value!=0)==!!(word&(1u<<24));
        cpu->pc=take?pc+displacement((word>>5)&0x7ffffu,19,2):next;return 1;
    }
    if((word&0x7e000000u)==0x36000000u) { /* TBZ/TBNZ; Rt31 is zero. */
        unsigned bit=((word>>19)&31)|((word>>26)&32);
        int take=!!(read_zero(cpu,reg)&(UINT64_C(1)<<bit))==!!(word&(1u<<24));
        cpu->pc=take?pc+displacement((word>>5)&0x3fffu,14,2):next;return 1;
    }
    if((word&0x1f000000u)==0x10000000u) { /* ADR/ADRP */
        uint64_t immediate=((word>>29)&3)|(((word>>5)&0x7ffffu)<<2);
        value=word>>31?(pc&~UINT64_C(4095))+displacement(immediate,21,12):pc+displacement(immediate,21,0);
        if(reg!=31)cpu->x[reg]=value;
        cpu->pc=next;return 1;
    }
    uint32_t system=word&0xffffffe0u;
    switch(system) {
    case 0xd53bd040u:value=cpu->tpidr;break;
    case 0xd53bd060u:value=cpu->tpidrro;break;
    case 0xd53b4400u:value=cpu->fpcr;break;
    case 0xd53b4420u:value=cpu->fpsr;break;
    case 0xd53b4200u:value=cpu->nzcv&UINT64_C(0xf0000000);break;
    case 0xd53be020u:case 0xd53be040u:value=ticks;break;
    case 0xd53be000u:value=19200000;break;
    case 0xd51bd040u:cpu->tpidr=read_zero(cpu,reg);cpu->pc=next;return 1;
    case 0xd51b4400u:cpu->fpcr=read_zero(cpu,reg)&UINT64_C(0x07c00000);cpu->pc=next;return 1;
    case 0xd51b4420u:cpu->fpsr=read_zero(cpu,reg)&UINT64_C(0xf800009f);cpu->pc=next;return 1;
    case 0xd51b4200u:cpu->nzcv=read_zero(cpu,reg)&UINT64_C(0xf0000000);cpu->pc=next;return 1;
    default:return 0;
    }
    if(reg!=31)cpu->x[reg]=value;
    cpu->pc=next;return 1;
}
