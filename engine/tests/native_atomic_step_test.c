/* Synthetic instruction encodings and private host memory; no game execution. */
#include "atomic_step.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned assertions;
#define CHECK(x) do {++assertions;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static Cpu *cpu,*expected;
static NgbAtomicMonitor monitor;
static _Alignas(16) uint64_t cell[2];
static unsigned bytes(unsigned size) {return 1u<<size;}
static uint64_t width_mask(unsigned size) {return size==3?UINT64_MAX:(UINT64_C(1)<<(bytes(size)*8))-1;}
static uint32_t excl(unsigned size,unsigned loading,unsigned pair,unsigned order,unsigned status,unsigned rt2,unsigned rn,unsigned rt) {
    return 0x08000000u|(size<<30)|(loading<<22)|(pair<<21)|(status<<16)|(order<<15)|(rt2<<10)|(rn<<5)|rt;
}
static uint64_t peek(unsigned size) {
    switch(size){case 0:return __atomic_load_n((uint8_t*)cell,__ATOMIC_SEQ_CST);case 1:return __atomic_load_n((uint16_t*)cell,__ATOMIC_SEQ_CST);case 2:return __atomic_load_n((uint32_t*)cell,__ATOMIC_SEQ_CST);default:return __atomic_load_n(cell,__ATOMIC_SEQ_CST);}
}
static void set(unsigned size,uint64_t value) {
    switch(size){case 0:__atomic_store_n((uint8_t*)cell,(uint8_t)value,__ATOMIC_SEQ_CST);break;case 1:__atomic_store_n((uint16_t*)cell,(uint16_t)value,__ATOMIC_SEQ_CST);break;case 2:__atomic_store_n((uint32_t*)cell,(uint32_t)value,__ATOMIC_SEQ_CST);break;default:__atomic_store_n(cell,value,__ATOMIC_SEQ_CST);}
}
static void reset(void) {
    memset(cpu,0xa5,sizeof *cpu);for(unsigned i=0;i<32;i++)cpu->x[i]=UINT64_C(0x1234567800000000)+i;
    cpu->pc=0x123;cpu->x[1]=(uintptr_t)cell;cpu->sp=(uintptr_t)cell;
    monitor=(NgbAtomicMonitor){.address=7,.value=8,.high=9,.width=4,.valid=0};
    cell[0]=UINT64_C(0x1122334455667788);cell[1]=UINT64_C(0x99aabbccddeeff00);
}
static void step(uint32_t word,int reg0,uint64_t value0,int reg1,uint64_t value1) {
    memcpy(expected,cpu,sizeof *cpu);expected->pc=0x1004;
    if(reg0>=0)expected->x[reg0]=value0;if(reg1>=0)expected->x[reg1]=value1;
    CHECK(ngb_atomic_step(cpu,word,0x1000,&monitor)==1);CHECK(!memcmp(cpu,expected,sizeof *cpu));
}
static void reject(uint32_t word,int result) {
    memcpy(expected,cpu,sizeof *cpu);NgbAtomicMonitor before=monitor;uint64_t memory[2]={cell[0],cell[1]};
    CHECK(ngb_atomic_step(cpu,word,0x1000,&monitor)==result);CHECK(!memcmp(cpu,expected,sizeof *cpu));
    CHECK(!memcmp(&monitor,&before,sizeof monitor));CHECK(!memcmp(cell,memory,sizeof cell));
}
typedef struct ThreadCase {uint64_t *memory;unsigned pair,iterations;atomic_uint *errors;uint64_t attempts;} ThreadCase;
static void *increment(void *opaque) {
    ThreadCase *test=opaque;Cpu *state=aligned_alloc(16,sizeof *state);
    if(!state){atomic_fetch_add(test->errors,1);return NULL;}
    memset(state,0,sizeof *state);state->x[1]=(uintptr_t)test->memory;
    NgbAtomicMonitor local={0};unsigned complete=0;
    uint32_t ll=excl(test->pair?3:2,1,test->pair,1,31,test->pair?3:31,1,0);
    uint32_t sc=excl(test->pair?3:2,0,test->pair,1,2,test->pair?3:31,1,0);
    while(complete<test->iterations&&++test->attempts<UINT64_C(2000000)) {
        if(ngb_atomic_step(state,ll,0x1000,&local)!=1){atomic_fetch_add(test->errors,1);break;}
        ++state->x[0];if(test->pair)++state->x[3];
        if(ngb_atomic_step(state,sc,0x1004,&local)!=1){atomic_fetch_add(test->errors,1);break;}
        if(state->x[2]==0)++complete;else if(state->x[2]!=1){atomic_fetch_add(test->errors,1);break;}
    }
    if(complete!=test->iterations)atomic_fetch_add(test->errors,1);
    free(state);return NULL;
}
typedef struct OwnershipCase {
    uint32_t *word;
    atomic_uint *active,*entries,*errors;
    uint32_t tag;unsigned kernel,iterations;
} OwnershipCase;
static void *ownership(void *opaque) {
    OwnershipCase *test=opaque;Cpu *state=aligned_alloc(16,sizeof *state);
    if(!state){atomic_fetch_add(test->errors,1);return NULL;}
    memset(state,0,sizeof *state);state->x[1]=(uintptr_t)test->word;NgbAtomicMonitor local={0};
    unsigned acquired=0;uint64_t attempts=0;
    while(acquired<test->iterations&&++attempts<UINT64_C(4000000)) {
        int success;
        if(test->kernel) {
            uint32_t expected_word=0;
            success=__atomic_compare_exchange_n(test->word,&expected_word,test->tag,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
        } else {
            if(ngb_atomic_step(state,excl(2,1,0,1,31,31,1,0),0x1000,&local)!=1){atomic_fetch_add(test->errors,1);break;}
            if(state->x[0]) {ngb_atomic_monitor_clear(&local);continue;}
            state->x[0]=test->tag;
            if(ngb_atomic_step(state,excl(2,0,0,1,2,31,1,0),0x1004,&local)!=1){atomic_fetch_add(test->errors,1);break;}
            success=state->x[2]==0;
        }
        if(!success)continue;
        if(atomic_fetch_add(test->active,1)!=0)atomic_fetch_add(test->errors,1);
        if(__atomic_load_n(test->word,__ATOMIC_SEQ_CST)!=test->tag)atomic_fetch_add(test->errors,1);
        atomic_fetch_add(test->entries,1);
        if(atomic_fetch_sub(test->active,1)!=1)atomic_fetch_add(test->errors,1);
        if(test->kernel)__atomic_store_n(test->word,0,__ATOMIC_SEQ_CST);
        else if(ngb_atomic_step(state,0x889ffc3fu,0x1008,&local)!=1){atomic_fetch_add(test->errors,1);break;} /* STLR WZR,[X1] */
        ++acquired;
    }
    if(acquired!=test->iterations)atomic_fetch_add(test->errors,1);
    free(state);return NULL;
}
int main(void) {
    cpu=aligned_alloc(16,sizeof *cpu);expected=aligned_alloc(16,sizeof *cpu);CHECK(cpu&&expected);
    for(unsigned size=0;size<4;size++)for(unsigned order=0;order<2;order++) {
        reset();uint64_t original=peek(size),wanted=UINT64_C(0xfedcba9876543210)&width_mask(size);
        step(excl(size,1,0,order,31,31,1,0),0,original,-1,0);
        CHECK(monitor.valid&&monitor.width==bytes(size)&&monitor.address==(uintptr_t)cell&&monitor.value==original&&!monitor.high);
        cpu->x[0]=wanted;step(excl(size,0,0,order,2,31,1,0),2,0,-1,0);CHECK(peek(size)==wanted&&!monitor.valid);
        /* A failed strong-CAS must not write back the observed old value. */
        reset();step(excl(size,1,0,order,31,31,1,0),0,peek(size),-1,0);
        set(size,5);cpu->x[0]=9;step(excl(size,0,0,order,2,31,1,0),2,1,-1,0);CHECK(peek(size)==5&&!monitor.valid);
        reset();step(excl(size,1,0,order,31,31,1,0),0,peek(size),-1,0);ngb_atomic_monitor_clear(&monitor);
        cpu->x[0]=9;uint64_t unchanged=peek(size);step(excl(size,0,0,order,2,31,1,0),2,1,-1,0);CHECK(peek(size)==unchanged);
        /* Address31 is SP, destination/status31 is discarded, data31 is zero. */
        reset();original=peek(size);step(excl(size,1,0,order,31,31,31,31),-1,0,-1,0);
        CHECK(monitor.valid&&monitor.value==original);cpu->x[0]=0;step(excl(size,0,0,order,31,31,31,0),-1,0,-1,0);CHECK(peek(size)==0&&!monitor.valid);
        set(size,9);step(excl(size,1,0,order,31,31,31,0),0,9,-1,0);
        step(excl(size,0,0,order,2,31,31,31),2,0,-1,0);CHECK(peek(size)==0);
        reset();monitor=(NgbAtomicMonitor){.address=(uintptr_t)cell,.value=peek(size),.width=bytes(size)==8?4:8,.valid=1};
        unchanged=peek(size);step(excl(size,0,0,order,2,31,1,0),2,1,-1,0);CHECK(peek(size)==unchanged);
    }
    for(unsigned size=2;size<4;size++)for(unsigned order=0;order<2;order++) {
        reset();uint64_t a=size==2?(uint32_t)cell[0]:cell[0],b=size==2?(uint32_t)(cell[0]>>32):cell[1];
        step(excl(size,1,1,order,31,3,1,0),0,a,3,b);
        CHECK(monitor.valid&&monitor.width==bytes(size)*2&&monitor.value==cell[0]);
        CHECK(monitor.high==(size==3?cell[1]:0));
        cpu->x[0]=0x123;cpu->x[3]=0x456;step(excl(size,0,1,order,2,3,1,0),2,0,-1,0);
        CHECK(cell[0]==(size==2?UINT64_C(0x45600000123):UINT64_C(0x123)));if(size==3)CHECK(cell[1]==0x456);
        reset();step(excl(size,1,1,order,31,3,1,0),0,size==2?(uint32_t)cell[0]:cell[0],3,size==2?cell[0]>>32:cell[1]);
        if(size==3)__atomic_store_n(&cell[1],17,__ATOMIC_SEQ_CST);else __atomic_store_n(cell,17,__ATOMIC_SEQ_CST);
        uint64_t old0=cell[0],old1=cell[1];cpu->x[0]=1;cpu->x[3]=2;
        step(excl(size,0,1,order,2,3,1,0),2,1,-1,0);CHECK(cell[0]==old0&&cell[1]==old1);
        reset();step(excl(size,1,1,order,31,3,31,31),3,size==2?cell[0]>>32:cell[1],-1,0);
        cpu->x[3]=0;step(excl(size,0,1,order,2,3,31,31),2,0,-1,0);CHECK(cell[0]==0);if(size==3)CHECK(cell[1]==0);
    }
    for(unsigned size=0;size<4;size++) {
        reset();uint64_t original=peek(size);step(0x08dffc20u|(size<<30),0,original,-1,0);CHECK(!monitor.valid);
        cpu->x[0]=0xff;step(0x089ffc20u|(size<<30),-1,0,-1,0);CHECK(peek(size)==0xff);
        reset();step(0x08dfffffu|(size<<30),-1,0,-1,0);step(0x089fffffu|(size<<30),-1,0,-1,0);CHECK(peek(size)==0);
        for(unsigned acquire=0;acquire<2;acquire++)for(unsigned release=0;release<2;release++) {
            uint32_t cas=0x08a07c20u|(size<<30)|(acquire<<22)|(release<<15)|(2u<<16); /* CAS R2,R0,[R1] */
            reset();original=peek(size);cpu->x[2]=original;cpu->x[0]=9;step(cas,2,original,-1,0);CHECK(peek(size)==9);
            reset();cpu->x[2]=0;original=peek(size);cpu->x[0]=9;step(cas,2,original,-1,0);CHECK(peek(size)==original);
        }
        for(unsigned op=0;op<9;op++)for(unsigned reverse=0;reverse<2;reverse++) {
            reset();uint64_t sign=UINT64_C(1)<<(bytes(size)*8-1),a=reverse?2:sign+3,b=reverse?sign+3:2,desired=0;
            set(size,a);cpu->x[2]=b;
            switch(op){case 0:desired=(a+b)&width_mask(size);break;case 1:desired=a&~b;break;case 2:desired=a^b;break;case 3:desired=a|b;break;case 4:desired=2;break;case 5:desired=sign+3;break;case 6:desired=sign+3;break;case 7:desired=2;break;case 8:desired=b;break;}
            step(0x38200020u|(size<<30)|(2u<<16)|(op<<12)|(1u<<23)|(1u<<22),0,a,-1,0);CHECK(peek(size)==desired);
        }
    }
    reset();monitor.valid=1;step(0xd5033f5f,-1,0,-1,0);CHECK(!monitor.valid);
    for(unsigned option=0;option<16;option++)for(unsigned op=0;op<3;op++) {
        const uint32_t barriers[]={0xd503309f,0xd50330bf,0xd50330df};reset();step(barriers[op]|(option<<8),-1,0,-1,0);
    }
    reset();reject(0xd503201f,0);reject(0x08207c20,-1); /* CASP deliberately unsupported. */
    reject(0x38209020,-1); /* Unreviewed LSE opcode. */
    cpu->x[1]=(uintptr_t)cell+1;reject(excl(3,1,0,1,31,31,1,0),-1);
    reset();reject(excl(3,1,1,0,31,0,1,0),-1); /* Duplicate nonzero pair destinations. */
    reject(excl(3,0,0,0,31,31,1,31),-1); /* Status/data ZR overlap is still unpredictable. */
    reject(excl(3,0,0,0,0,31,1,0),-1);reject(excl(3,0,0,0,1,31,1,0),-1);
    for(unsigned pair=0;pair<2;pair++) {
        _Alignas(16) uint64_t shared[2]={0,0};atomic_uint errors=0;pthread_t threads[4];ThreadCase tests[4];
        for(unsigned i=0;i<4;i++){tests[i]=(ThreadCase){.memory=shared,.pair=pair,.iterations=10000,.errors=&errors};CHECK(!pthread_create(&threads[i],NULL,increment,&tests[i]));}
        uint64_t attempts=0;for(unsigned i=0;i<4;i++){CHECK(!pthread_join(threads[i],NULL));attempts+=tests[i].attempts;}
        CHECK(!atomic_load(&errors));CHECK(shared[0]==40000);if(pair)CHECK(shared[1]==40000);CHECK(attempts>=40000);
    }
    uint32_t shared_word=0;atomic_uint active=0,entries=0,errors=0;pthread_t owners[5];OwnershipCase cases[5];
    for(unsigned i=0;i<5;i++) {
        cases[i]=(OwnershipCase){.word=&shared_word,.active=&active,.entries=&entries,.errors=&errors,
            .tag=0x101+i,.kernel=i==4,.iterations=10000};
        CHECK(!pthread_create(&owners[i],NULL,ownership,&cases[i]));
    }
    for(unsigned i=0;i<5;i++)CHECK(!pthread_join(owners[i],NULL));
    CHECK(!atomic_load(&errors)&&!atomic_load(&active)&&!shared_word&&atomic_load(&entries)==50000);
    free(cpu);free(expected);printf("atomic step assertions=%u; 4-thread increments=40000 scalar + 40000 pair128; guest/kernel ownership=50000\n",assertions);return 0;
}
