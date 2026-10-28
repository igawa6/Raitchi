/* Synthetic resource tables only; no original game code or binary offsets. */
#define _GNU_SOURCE
#include "native_sdk_fastpath.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static unsigned assertions;
#define CHECK(x) do {++assertions;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void put16(void *p,uint16_t v){memcpy(p,&v,2);}static void put32(void *p,uint32_t v){memcpy(p,&v,4);}static void put64(void *p,uint64_t v){memcpy(p,&v,8);}
static uint64_t get64(const void *p){uint64_t v;memcpy(&v,p,8);return v;}
static void section(unsigned char *table,unsigned index,uint64_t base,uint32_t position,int32_t first,int32_t count) {
    unsigned char *at=table+16+index*24;put64(at,base);put32(at+8,position);put32(at+12,0xdeadbeef);put32(at+16,(uint32_t)first);put32(at+20,(uint32_t)count);
}
static void entry(unsigned char *table,unsigned sections,unsigned index,uint32_t position,uint16_t groups,uint8_t pointers,uint8_t gap) {
    unsigned char *at=table+16+sections*24+index*8;put32(at,position);put16(at+4,groups);at[6]=pointers;at[7]=gap;
}
static void header(unsigned char *file,unsigned offset,int32_t sections) {
    put16(file+0x14,0x5a04);unsigned char *table=file+offset;memcpy(table,"_RLT",4);put32(table+4,offset);put32(table+8,(uint32_t)sections);put32(table+12,0x12345678);
}
static void expect_rejection(unsigned char *file,size_t size,uint64_t table,enum NativeSdkRelocationStatus expected) {
    unsigned char *copy=malloc(size);CHECK(copy);memcpy(copy,file,size);NativeSdkRelocationResult result;memset(&result,0xa5,sizeof result);NativeSdkRelocationResult old=result;
    CHECK(native_sdk_relocate_table(table,&result)==expected);CHECK(!memcmp(file,copy,size));CHECK(!memcmp(&result,&old,sizeof old));free(copy);
}
int main(void) {
    const size_t size=32768;unsigned char *allocation=malloc(size+16),*expected=malloc(size);CHECK(allocation&&expected);
    for(unsigned pointers=1;pointers<=255;pointers++)for(unsigned unaligned=0;unaligned<2;unaligned++) {
        unsigned char *file=allocation+(unaligned?3:0);memset(file,0x6d,size);unsigned char *table=file+16384;
        header(file,16384,1);section(table,0,0,0,0,1);entry(table,1,0,65,3,(uint8_t)pointers,5);
        memcpy(expected,file,size);uint64_t fixed=0,nulls=0;
        for(unsigned group=0;group<3;group++)for(unsigned p=0;p<pointers;p++) {
            size_t at=65+(group*(pointers+5)+p)*8;uint64_t value=(p+group)%5?0x20+p*17:0;
            put64(file+at,value);put64(expected+at,value?value+(uintptr_t)file:0);fixed+=value!=0;nulls+=value==0;
        }
        put16(expected+0x14,0x5a05);NativeSdkRelocationResult result={0};
        CHECK(native_sdk_relocate_table((uintptr_t)table,&result)==NATIVE_SDK_RELOCATION_OK);
        CHECK(!memcmp(file,expected,size));CHECK(result.sections==1&&result.entries==1&&result.groups==3&&result.pointers==3*pointers&&result.fixed_pointers==fixed&&result.null_pointers==nulls);
        /* The original method adds the base again even with flag bit0 set. */
        for(unsigned group=0;group<3;group++)for(unsigned p=0;p<pointers;p++) {
            size_t at=65+(group*(pointers+5)+p)*8;uint64_t value=get64(expected+at);if(value)put64(expected+at,value+(uintptr_t)file);
        }
        CHECK(native_sdk_relocate_table((uintptr_t)table,&result)==NATIVE_SDK_RELOCATION_OK);CHECK(!memcmp(file,expected,size));
    }
    unsigned char *file=allocation;memset(file,0,size);unsigned char *table=file+4096;
    header(file,4096,2);section(table,0,0,0,0,1);section(table,1,UINT64_C(0x12345000),512,1,1);
    entry(table,2,0,64,1,2,0);entry(table,2,1,128,2,2,3);
    put64(file+64,1);put64(file+72,0);put64(file+128,UINT64_MAX);put64(file+136,8);put64(file+168,3);put64(file+176,0);
    memcpy(expected,file,size);put16(expected+0x14,0x5a05);put64(expected+64,(uintptr_t)file+1);
    put64(expected+128,UINT64_MAX+UINT64_C(0x12345000)-512);put64(expected+136,UINT64_C(0x12345000)-512+8);put64(expected+168,UINT64_C(0x12345000)-512+3);
    NativeSdkRelocationResult result={0};CHECK(native_sdk_relocate_table((uintptr_t)table,&result)==NATIVE_SDK_RELOCATION_OK);
    CHECK(!memcmp(file,expected,size));CHECK(result.sections==2&&result.entries==2&&result.groups==3&&result.pointers==6&&result.fixed_pointers==4&&result.null_pointers==2);
    /* Zero-length structures/pointer arrays never touch their invalid positions. */
    for(unsigned mode=0;mode<2;mode++) {
        memset(file,0,size);header(file,4096,1);section(table,0,0,0,0,1);entry(table,1,0,UINT32_MAX,mode?5:0,mode?0:5,255);
        memcpy(expected,file,size);put16(expected+0x14,0x5a05);CHECK(native_sdk_relocate_table((uintptr_t)table,&result)==NATIVE_SDK_RELOCATION_OK);
        CHECK(!memcmp(file,expected,size)&&result.entries==1&&!result.pointers);
    }
    for(int count=-1;count<=0;count++) {
        memset(file,0,size);header(file,4096,count);memcpy(expected,file,size);put16(expected+0x14,0x5a05);
        CHECK(native_sdk_relocate_table((uintptr_t)table,&result)==NATIVE_SDK_RELOCATION_OK);CHECK(!memcmp(file,expected,size)&&!result.sections);
    }
    memset(file,0,size);header(file,4096,1);section(table,0,0,0,0,-1);memcpy(expected,file,size);put16(expected+0x14,0x5a05);
    CHECK(native_sdk_relocate_table((uintptr_t)table,&result)==NATIVE_SDK_RELOCATION_OK);CHECK(!memcmp(file,expected,size)&&!result.entries);
    /* Duplicate entries preserve original traversal order; wrap-to-zero becomes
     * a null on the next traversal, including within this same call. */
    memset(file,0,size);header(file,4096,1);section(table,0,1,0,0,2);
    entry(table,1,0,64,1,1,0);entry(table,1,1,64,1,1,0);put64(file+64,UINT64_MAX);
    CHECK(native_sdk_relocate_table((uintptr_t)table,&result)==NATIVE_SDK_RELOCATION_OK);
    CHECK(!get64(file+64)&&result.fixed_pointers==1&&result.null_pointers==1);
    CHECK(native_sdk_relocate_table((uintptr_t)table,&result)==NATIVE_SDK_RELOCATION_OK);
    CHECK(!get64(file+64)&&result.fixed_pointers==0&&result.null_pointers==2);
    /* Invalid metadata must reject before the first otherwise valid fixup. */
    memset(file,0,size);header(file,4096,1);section(table,0,0,0,0,2);entry(table,1,0,64,1,1,0);entry(table,1,1,4096,1,1,0);put64(file+64,9);
    expect_rejection(file,size,(uintptr_t)table,NATIVE_SDK_RELOCATION_METADATA);
    entry(table,1,1,20,1,1,0);expect_rejection(file,size,(uintptr_t)table,NATIVE_SDK_RELOCATION_METADATA);
    section(table,0,0,0,-1,2);expect_rejection(file,size,(uintptr_t)table,NATIVE_SDK_RELOCATION_METADATA);
    section(table,0,0,0,INT32_MAX,2);expect_rejection(file,size,(uintptr_t)table,NATIVE_SDK_RELOCATION_OVERFLOW);
    CHECK(native_sdk_relocate_table(0,NULL)==NATIVE_SDK_RELOCATION_ARGUMENT);
    CHECK(native_sdk_relocate_table(UINT64_MAX-7,NULL)==NATIVE_SDK_RELOCATION_RANGE);
    size_t page=(size_t)sysconf(_SC_PAGESIZE);unsigned char *mapping=mmap(NULL,page*4,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(mapping!=MAP_FAILED);
    unsigned char *t=mapping+2*page;header(mapping,(unsigned)(2*page),1);section(t,0,0,0,0,2);entry(t,1,0,64,1,1,0);entry(t,1,1,(uint32_t)(page+8),1,1,0);put64(mapping+64,9);
    CHECK(!mprotect(mapping+page,page,PROT_NONE));unsigned char snapshot[128];memcpy(snapshot,mapping,sizeof snapshot);
    CHECK(native_sdk_relocate_table((uintptr_t)t,&result)==NATIVE_SDK_RELOCATION_RANGE);CHECK(!memcmp(mapping,snapshot,sizeof snapshot));
    entry(t,1,1,80,1,1,0);put64(mapping+80,11);CHECK(!mprotect(t,page,PROT_READ));
    CHECK(native_sdk_relocate_table((uintptr_t)t,&result)==NATIVE_SDK_RELOCATION_OK);CHECK(get64(mapping+64)==(uintptr_t)mapping+9&&get64(mapping+80)==(uintptr_t)mapping+11);
    CHECK(!munmap(mapping,page*4));free(allocation);free(expected);
#ifndef NATIVE_SDK_RELOCATION_STANDALONE
    CHECK(!unsetenv("LARECOMP_NATIVE_SDK_RELOCATE_FASTPATH"));
    CHECK(!native_sdk_fastpath(NULL)&&!native_sdk_fastpath("_ZN2nn4util15RelocationTable8RelocateEv"));
    const char *invalid[]={"", "0", "true", "01", "1x"};
    for(unsigned i=0;i<sizeof invalid/sizeof invalid[0];i++) {
        CHECK(!setenv("LARECOMP_NATIVE_SDK_RELOCATE_FASTPATH",invalid[i],1));
        CHECK(!native_sdk_fastpath("_ZN2nn4util15RelocationTable8RelocateEv"));
    }
    CHECK(!setenv("LARECOMP_NATIVE_SDK_RELOCATE_FASTPATH","1",1));
    CHECK(!native_sdk_fastpath("other")&&!native_sdk_fastpath("_ZN2nn4util15RelocationTable8RelocateEvX"));
    GuestFn method=native_sdk_fastpath("_ZN2nn4util15RelocationTable8RelocateEv");CHECK(method);
    file=calloc(1,8192);Cpu *cpu=malloc(sizeof *cpu),*before=malloc(sizeof *before);CHECK(file&&cpu&&before);
    header(file,4096,1);section(file+4096,0,0,0,0,1);entry(file+4096,1,0,64,1,1,0);put64(file+64,13);
    memset(cpu,0xc3,sizeof *cpu);cpu->x[0]=(uintptr_t)(file+4096);memcpy(before,cpu,sizeof *cpu);
    method(cpu);CHECK(!memcmp(cpu,before,sizeof *cpu));CHECK(get64(file+64)==(uintptr_t)file+13);
    CHECK(!unsetenv("LARECOMP_NATIVE_SDK_RELOCATE_FASTPATH"));free(before);free(cpu);free(file);
#endif
    printf("SDK relocation assertions=%u; pointer counts1..255, unaligned buffers, repeated calls, mapped bounds, failure-before-effect\n",assertions);return 0;
}
