#include "atomic_step.h"
#include <limits.h>

typedef __uint128_t AtomicWide;
void ngb_atomic_monitor_clear(NgbAtomicMonitor *monitor) {if(monitor)monitor->valid=0;}
static uint64_t source(const Cpu *cpu,unsigned reg) {return reg==31?0:cpu->x[reg];}
static void destination(Cpu *cpu,unsigned reg,uint64_t value) {if(reg!=31)cpu->x[reg]=value;}
static uint64_t address(const Cpu *cpu,unsigned reg) {return reg==31?cpu->sp:cpu->x[reg];}
static int aligned(uint64_t addr,unsigned width) {return addr && !(addr&(width-1)) && addr<=UINT64_MAX-width;}
static uint64_t mask(unsigned width) {return width==8?UINT64_MAX:(UINT64_C(1)<<(width*8))-1;}

static uint64_t load(uint64_t addr,unsigned width) {
    switch(width) {
    case 1:return __atomic_load_n((uint8_t *)(uintptr_t)addr,__ATOMIC_SEQ_CST);
    case 2:return __atomic_load_n((uint16_t *)(uintptr_t)addr,__ATOMIC_SEQ_CST);
    case 4:return __atomic_load_n((uint32_t *)(uintptr_t)addr,__ATOMIC_SEQ_CST);
    default:return __atomic_load_n((uint64_t *)(uintptr_t)addr,__ATOMIC_SEQ_CST);
    }
}
static void store(uint64_t addr,unsigned width,uint64_t value) {
    switch(width) {
    case 1:__atomic_store_n((uint8_t *)(uintptr_t)addr,(uint8_t)value,__ATOMIC_SEQ_CST);break;
    case 2:__atomic_store_n((uint16_t *)(uintptr_t)addr,(uint16_t)value,__ATOMIC_SEQ_CST);break;
    case 4:__atomic_store_n((uint32_t *)(uintptr_t)addr,(uint32_t)value,__ATOMIC_SEQ_CST);break;
    default:__atomic_store_n((uint64_t *)(uintptr_t)addr,value,__ATOMIC_SEQ_CST);break;
    }
}
static int compare_exchange(uint64_t addr,unsigned width,uint64_t *expected,uint64_t desired) {
    int success;
    switch(width) {
    case 1:{uint8_t value=(uint8_t)*expected;success=__atomic_compare_exchange_n((uint8_t *)(uintptr_t)addr,&value,(uint8_t)desired,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);*expected=value;break;}
    case 2:{uint16_t value=(uint16_t)*expected;success=__atomic_compare_exchange_n((uint16_t *)(uintptr_t)addr,&value,(uint16_t)desired,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);*expected=value;break;}
    case 4:{uint32_t value=(uint32_t)*expected;success=__atomic_compare_exchange_n((uint32_t *)(uintptr_t)addr,&value,(uint32_t)desired,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);*expected=value;break;}
    default:success=__atomic_compare_exchange_n((uint64_t *)(uintptr_t)addr,expected,desired,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);break;
    }
    return success;
}
/* MIN/MAX have no portable C atomic fetch builtin. A strong-CAS retry loop is
 * one linearizable RMW; every failed attempt performs no store. */
static uint64_t rmw(uint64_t addr,unsigned width,unsigned op,uint64_t input) {
    uint64_t value=load(addr,width),m=mask(width),sign=UINT64_C(1)<<(width*8-1);
    input&=m;
    for(;;) {
        uint64_t desired;
        switch(op) {
        case 0:desired=(value+input)&m;break;
        case 1:desired=value&~input;break;
        case 2:desired=value^input;break;
        case 3:desired=value|input;break;
        case 4:desired=(value^sign)>(input^sign)?value:input;break;
        case 5:desired=(value^sign)<(input^sign)?value:input;break;
        case 6:desired=value>input?value:input;break;
        case 7:desired=value<input?value:input;break;
        default:desired=input;break; /* SWP */
        }
        uint64_t expected=value;
        if(compare_exchange(addr,width,&expected,desired))return value;
        value=expected;
    }
}

int ngb_atomic_step(Cpu *cpu,uint32_t word,uint64_t pc,NgbAtomicMonitor *monitor) {
    uint32_t barrier=word&UINT32_C(0xfffff0ff);
    int clear=barrier==UINT32_C(0xd503305f);
    int fence=barrier==UINT32_C(0xd503309f) || barrier==UINT32_C(0xd50330bf) || barrier==UINT32_C(0xd50330df);
    int exclusive_group=(word&UINT32_C(0x3f000000))==UINT32_C(0x08000000);
    int lse=(word&UINT32_C(0x3f200c00))==UINT32_C(0x38200000);
    if(!clear&&!fence&&!exclusive_group&&!lse)return 0;
    if(!cpu||!monitor||(pc&3))return -1;
    if(clear) {ngb_atomic_monitor_clear(monitor);cpu->pc=pc+4;return 1;}
    if(fence) {__atomic_thread_fence(__ATOMIC_SEQ_CST);cpu->pc=pc+4;return 1;}
    unsigned size=word>>30,width=1u<<size,rt=word&31,rn=(word>>5)&31,rs=(word>>16)&31;
    uint64_t addr=address(cpu,rn);
    if(lse) {
        unsigned op=(word>>12)&15;
        if(op>8||!aligned(addr,width))return -1;
        uint64_t old=rmw(addr,width,op,source(cpu,rs));
        destination(cpu,rt,old);cpu->pc=pc+4;return 1;
    }
    unsigned o2=(word>>23)&1,acquire_or_load=(word>>22)&1,pair=(word>>21)&1,rt2=(word>>10)&31;
    if(o2&&pair) { /* CAS/CASA/CASL/CASAL, all scalar widths. */
        if(rt2!=31||!aligned(addr,width))return -1;
        uint64_t expected=source(cpu,rs)&mask(width),desired=source(cpu,rt);
        (void)compare_exchange(addr,width,&expected,desired);
        destination(cpu,rs,expected);cpu->pc=pc+4;return 1;
    }
    if(o2) { /* LDAR/STLR; weaker/extension encodings fail closed. */
        if(pair||rs!=31||rt2!=31||!(word&(1u<<15))||!aligned(addr,width))return -1;
        if(acquire_or_load)destination(cpu,rt,load(addr,width));else store(addr,width,source(cpu,rt));
        cpu->pc=pc+4;return 1;
    }
    if(pair&&size<2)return -1; /* CASP and reserved small-pair encodings. */
    if(!pair&&rt2!=31)return -1;
    unsigned total=pair?width*2:width;
    if(!aligned(addr,total))return -1;
    if(acquire_or_load) {
        if(rs!=31||(pair&&rt==rt2&&rt!=31))return -1;
        uint64_t value,high=0;
        if(total==16) {
            AtomicWide all=__atomic_load_n((AtomicWide *)(uintptr_t)addr,__ATOMIC_SEQ_CST);
            value=(uint64_t)all;high=(uint64_t)(all>>64);
        } else value=load(addr,total);
        monitor->address=addr;monitor->width=total;monitor->value=value;monitor->high=high;monitor->valid=1;
        if(pair&&width==4) {destination(cpu,rt,(uint32_t)value);destination(cpu,rt2,(uint32_t)(value>>32));}
        else {destination(cpu,rt,value);if(pair)destination(cpu,rt2,high);}
        cpu->pc=pc+4;return 1;
    }
    /* STXR's status register may be WZR, but must not overlap either data
     * register (including ZR) or a non-SP address register. */
    if(rs==rt||(pair&&rs==rt2)||(rn!=31&&rs==rn))return -1;
    uint64_t result=1;
    if(monitor->valid&&monitor->address==addr&&monitor->width==total) {
        if(total==16) {
            AtomicWide expected=((AtomicWide)monitor->high<<64)|monitor->value;
            AtomicWide desired=((AtomicWide)source(cpu,rt2)<<64)|source(cpu,rt);
            result=!__atomic_compare_exchange_n((AtomicWide *)(uintptr_t)addr,&expected,desired,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
        } else {
            uint64_t expected=monitor->value,desired=source(cpu,rt);
            if(pair)desired=(uint32_t)desired|((uint64_t)(uint32_t)source(cpu,rt2)<<32);
            result=!compare_exchange(addr,total,&expected,desired);
        }
    }
    ngb_atomic_monitor_clear(monitor);destination(cpu,rs,result);cpu->pc=pc+4;return 1;
}
