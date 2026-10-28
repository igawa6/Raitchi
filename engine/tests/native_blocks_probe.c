/* Asset-free execution checks for the actual optional native-block adapter.
 * Synthetic Cpu/anonymous stacks only: no KThread, title, SDK or game claim. */
#define _GNU_SOURCE
#include "native_blocks.h"
#include "native_blocks_words.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>
/* Link-time wrappers exist only in this diagnostic, never production. */
void *__real_mmap(void*,size_t,int,int,int,off_t);
int __real_mprotect(void*,size_t,int);
static _Thread_local int deny_mapping,deny_protection;
static _Thread_local uintptr_t active_code;
static _Atomic unsigned protection_violations;
void *__wrap_mmap(void *address,size_t size,int protection,int flags,int fd,off_t offset){
    if((protection&(PROT_WRITE|PROT_EXEC))==(PROT_WRITE|PROT_EXEC))atomic_fetch_add(&protection_violations,1);
    if(deny_mapping){errno=ENOMEM;return MAP_FAILED;}
    return __real_mmap(address,size,protection,flags,fd,offset);
}
int __wrap_mprotect(void *address,size_t size,int protection){
    if((protection&(PROT_WRITE|PROT_EXEC))==(PROT_WRITE|PROT_EXEC) ||
       ((protection&PROT_WRITE)&&active_code>=(uintptr_t)address&&active_code-(uintptr_t)address<size))atomic_fetch_add(&protection_violations,1);
    if(deny_protection){errno=ENOMEM;return -1;}
    return __real_mprotect(address,size,protection);
}
static unsigned resource_decline_cases(void){
    Cpu *cpu=aligned_alloc(16,sizeof *cpu),*before=aligned_alloc(16,sizeof *before);if(!cpu||!before)abort();
    memset(cpu,0x5a,sizeof *cpu);cpu->sp=0x10000;memcpy(before,cpu,sizeof *cpu);unsigned passed=0;
    uint32_t word=0x91000000u|(2047u<<10);deny_mapping=1;
    passed+=native_blocks_run(cpu,&word,1,1u)==1&&!memcmp(cpu,before,sizeof *cpu);deny_mapping=0;
    word=0x91000000u|(2046u<<10);deny_protection=1;
    passed+=native_blocks_run(cpu,&word,1,1u)==1&&!memcmp(cpu,before,sizeof *cpu);deny_protection=0;
    if(native_blocks_thread_cleanup())abort();free(cpu);free(before);return passed;
}
#ifdef NATIVE_BLOCKS_HOST_TEST
#include "block_plan.h"
/* Host tests exercise real C selection/policy; no native entry is executed. */
static _Thread_local int test_nested;
static _Atomic unsigned nested_rejections;
__asm__(".pushsection .rodata\n.global native_blocks_exit_begin\nnative_blocks_exit_begin:\n.byte 0,0,0,0\n.global native_blocks_exit_end\nnative_blocks_exit_end:\n.popsection\n");
void native_blocks_enter(void *opaque){
    struct View{Cpu *cpu;const void *entry;} *frame=opaque;
    FILE *maps=fopen("/proc/self/maps","r");if(!maps)abort();char line[512],permissions[5];unsigned long long lo,hi;int rx=0;
    while(fgets(line,sizeof line,maps))if(sscanf(line,"%llx-%llx %4s",&lo,&hi,permissions)==3&&lo<=(uintptr_t)frame->entry&&hi>(uintptr_t)frame->entry){rx=!memcmp(permissions,"r-x",3);break;}
    fclose(maps);if(!rx)atomic_fetch_add(&protection_violations,1);
    if(test_nested){test_nested=0;active_code=(uintptr_t)frame->entry;uint32_t nested=0x91000000u|(2045u<<10);
        if(native_blocks_run(frame->cpu,&nested,1,1u)==1)atomic_fetch_add(&nested_rejections,1);active_code=0;}
}
typedef struct HostCacheWorker{unsigned failed;} HostCacheWorker;
static void *host_cache_worker(void *opaque){
    HostCacheWorker *worker=opaque;Cpu *cpu[2]={aligned_alloc(16,sizeof(Cpu)),aligned_alloc(16,sizeof(Cpu))};Cpu *before=aligned_alloc(16,sizeof(Cpu));if(!cpu[0]||!cpu[1]||!before)abort();
    for(unsigned i=0;i<96;i++){Cpu *current=cpu[i&1];memset(current,0x5a^i,sizeof *current);current->sp=0x10000;memcpy(before,current,sizeof *current);
        uint32_t word=0x91000000u|((i+1)<<10)|(19u<<5)|19u;
        worker->failed+=native_blocks_run(current,&word,1,UINT32_C(1)<<19)!=0||memcmp(current,before,sizeof *current)!=0;}
    free(cpu[0]);free(cpu[1]);free(before);return NULL; /* pthread destructor must free both identities */
}
static unsigned assertions;
#define CHECK(x) do {assertions++;if(!(x)){fprintf(stderr,"line%d:%s\n",__LINE__,#x);return 1;}}while(0)
int main(void) {
    NgbDecoder decoder;CHECK(ngb_decoder_init(&decoder)==NGB_OK);
    CHECK(native_blocks_context_register(0)==19);CHECK(native_blocks_context_register(NGB_CONTEXT_GPR_MASK)==19);
    CHECK(native_blocks_context_register(UINT32_C(1)<<18)<0);CHECK(native_blocks_context_register(UINT32_C(1)<<31)<0);
    CHECK(native_blocks_context_register(NGB_CONTEXT_GPR_MASK|(UINT32_C(1)<<19))<0);
    for(unsigned reg=0;reg<31;reg++)if(NGB_CONTEXT_GPR_MASK&(UINT32_C(1)<<reg))
    CHECK(native_blocks_context_register((NGB_CONTEXT_GPR_MASK&~(UINT32_C(1)<<reg))|(UINT32_C(1)<<19))==(int)reg);
    for(unsigned p=0;p<PROBE_PROGRAM_COUNT;p++) {
        NgbBlockPlan plan={.struct_size=sizeof plan,.version=NGB_PLAN_VERSION};
        int context=(probe_masks[p]&(UINT32_C(1)<<19))!=0;
        CHECK((context?ngb_plan_block_context:ngb_plan_block)(&decoder,probe_programs[p],probe_counts[p]*4,0x10000,64,&plan)==NGB_OK);
        CHECK(plan.native_count==probe_counts[p]);CHECK(plan.gpr_usage==probe_masks[p]);
        CHECK(native_blocks_context_register(plan.gpr_usage)>=0);
        for(size_t i=0;i<probe_counts[p];i++) {
                NgbInstruction instruction;CHECK(ngb_classify(&decoder,&probe_programs[p][i],0x10000+i*4,&instruction)==NGB_OK);
            CHECK(!((instruction.read_gpr|instruction.write_gpr)&(UINT32_C(1)<<18)));
        }
    }
    NgbInstruction instruction;CHECK(ngb_classify(&decoder,&probe_exclusive_words[0],0x10000,&instruction)==NGB_OK);CHECK(instruction.flags&NGB_EXCLUSIVE_LOAD);
    CHECK(ngb_classify(&decoder,&probe_exclusive_words[2],0x10008,&instruction)==NGB_OK);CHECK(instruction.flags&NGB_EXCLUSIVE_STORE);
    CHECK(ngb_classify(&decoder,&probe_exclusive_words[3],0x1000c,&instruction)==NGB_OK);CHECK(instruction.flags&NGB_CLEAR_EXCLUSIVE);
    CHECK(resource_decline_cases()==2);
    HostCacheWorker cache_workers[4]={0};pthread_t cache_threads[4];
    for(unsigned i=0;i<4;i++)CHECK(!pthread_create(&cache_threads[i],NULL,host_cache_worker,&cache_workers[i]));
    for(unsigned i=0;i<4;i++){CHECK(!pthread_join(cache_threads[i],NULL));CHECK(!cache_workers[i].failed);}
    NativeBlocksCacheStats statistics;native_blocks_cache_stats(&statistics);
    CHECK(statistics.global_pages<=statistics.global_page_limit);CHECK(!statistics.overflow_live_pages);
    if(statistics.global_page_limit<=2){
        CHECK(statistics.global_pages==statistics.global_page_limit);CHECK(statistics.overflow_rewrites>0);CHECK(statistics.overflow_peak_pages<=4*2*statistics.overflow_slots_per_cpu);
        Cpu cpu={.sp=0x10000};uint32_t word=0x91000000u|(2044u<<10);test_nested=1;CHECK(!native_blocks_run(&cpu,&word,1,1u));CHECK(atomic_load(&nested_rejections)==1);CHECK(!native_blocks_thread_cleanup());
    }
    CHECK(!atomic_load(&protection_violations));
    ngb_decoder_destroy(&decoder);printf("assertions=%u\n",assertions);return 0;
}
#else
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>
#include <unicorn/unicorn.h>
#include <unicorn/arm64.h>
#define THREADS 4u
#define ITERATIONS 32u
#define CHURN_ITERATIONS 128u

typedef struct __attribute__((aligned(16))) HostObserved {
    uint64_t x[12];__uint128_t q[8];uint64_t fpcr,fpsr,x18,tls,sp_before,sp_after;
} HostObserved;
_Static_assert(offsetof(HostObserved,q)==96 && offsetof(HostObserved,fpcr)==224 && offsetof(HostObserved,sp_before)==256,"host observation ABI");
int probe_checked_run(Cpu*,const uint32_t*,size_t,uint32_t,HostObserved*);
/* Explicit observations prevent the C compiler assuming its callees preserved
 * canaries and optimizing the ABI checks away. Physical X18 is only read. */
__asm__(
".text\n.p2align 2\n.hidden probe_checked_run\n.global probe_checked_run\nprobe_checked_run:\n"
"sub sp,sp,#256\n"
"stp x19,x20,[sp,#0]\nstp x21,x22,[sp,#16]\nstp x23,x24,[sp,#32]\nstp x25,x26,[sp,#48]\nstp x27,x28,[sp,#64]\nstp x29,x30,[sp,#80]\n"
"stp q8,q9,[sp,#96]\nstp q10,q11,[sp,#128]\nstp q12,q13,[sp,#160]\nstp q14,q15,[sp,#192]\n"
"mrs x9,fpcr\nstr x9,[sp,#224]\nmrs x9,fpsr\nstr x9,[sp,#232]\nstr x4,[sp,#240]\nmov x9,sp\nstr x9,[x4,#256]\n"
"mov x19,#0x9013\nmov x20,#0x9014\nmov x21,#0x9015\nmov x22,#0x9016\nmov x23,#0x9017\nmov x24,#0x9018\nmov x25,#0x9019\nmov x26,#0x901a\nmov x27,#0x901b\nmov x28,#0x901c\nmov x29,#0x901d\n"
"movi v8.16b,#8\nmovi v9.16b,#9\nmovi v10.16b,#10\nmovi v11.16b,#11\nmovi v12.16b,#12\nmovi v13.16b,#13\nmovi v14.16b,#14\nmovi v15.16b,#15\n"
"bl native_blocks_run\n1:\nstr w0,[sp,#248]\nldr x9,[sp,#240]\n"
"stp x19,x20,[x9,#0]\nstp x21,x22,[x9,#16]\nstp x23,x24,[x9,#32]\nstp x25,x26,[x9,#48]\nstp x27,x28,[x9,#64]\nstp x29,x30,[x9,#80]\n"
"stp q8,q9,[x9,#96]\nstp q10,q11,[x9,#128]\nstp q12,q13,[x9,#160]\nstp q14,q15,[x9,#192]\n"
"mrs x10,fpcr\nstr x10,[x9,#224]\nmrs x10,fpsr\nstr x10,[x9,#232]\nstr x18,[x9,#240]\nmrs x10,tpidr_el0\nstr x10,[x9,#248]\nmov x10,sp\nstr x10,[x9,#264]\n"
"adr x10,1b\ncmp x30,x10\nb.eq 2f\nmov w10,#1\nstr w10,[sp,#248]\n2:\n"
"ldr x9,[sp,#224]\nmsr fpcr,x9\nldr x9,[sp,#232]\nmsr fpsr,x9\n"
"ldp q8,q9,[sp,#96]\nldp q10,q11,[sp,#128]\nldp q12,q13,[sp,#160]\nldp q14,q15,[sp,#192]\n"
"ldp x19,x20,[sp,#0]\nldp x21,x22,[sp,#16]\nldp x23,x24,[sp,#32]\nldp x25,x26,[sp,#48]\nldp x27,x28,[sp,#64]\nldp x29,x30,[sp,#80]\nldr w0,[sp,#248]\nadd sp,sp,#256\nret\n");
static uint64_t tls_value(void){uint64_t v;__asm__ volatile("mrs %0,tpidr_el0":"=r"(v));return v;}
static uint64_t x18_value(void){uint64_t v;__asm__ volatile("mov %0,x18":"=r"(v));return v;}
static void seed(Cpu *cpu,uintptr_t sp,unsigned salt){
    memset(cpu,0xa5^salt,sizeof *cpu);
    for(unsigned i=0;i<32;i++){cpu->x[i]=UINT64_C(0x13579bdf00000000)^((uint64_t)i<<32)^salt;cpu->v[i]=((__uint128_t)(UINT64_C(0x9988776655443322)^((uint64_t)i<<32)^salt)<<64)|(UINT64_C(0x1020304050607080)^i^salt);}
    cpu->sp=sp;cpu->nzcv=0x90000000;cpu->fpcr=0x01400000;cpu->fpsr=0x10;cpu->thread=NULL;
}
static int guards_ok(unsigned char *stack,size_t usable,uint64_t cookie){
    const uint64_t *low=(const uint64_t*)stack,*high=(const uint64_t*)(stack+usable-64);
    for(unsigned i=0;i<8;i++)if(low[i]!=(cookie^i)||high[i]!=(~cookie^i))return 0;return 1;
}
typedef struct Worker {unsigned index,failures,calls,guards,arch_failures,host_failures,memory_failures;uint64_t host_tls,host_x18;_Atomic unsigned *ready,*start;} Worker;
static int check_run(Worker *w,Cpu *cpu,const Cpu *expected,const uint32_t *words,size_t count,uint32_t usage,uint64_t fpcr,uint64_t fpsr){
    HostObserved observed={0};int failed=probe_checked_run(cpu,words,count,usage,&observed)!=0;
    int arch_bad=memcmp(cpu,expected,sizeof *cpu)!=0;w->arch_failures+=arch_bad;failed|=arch_bad;
    int host_bad=0;
    for(unsigned i=0;i<11;i++)host_bad|=observed.x[i]!=0x9013u+i;
    /* AAPCS64 preserves the low64 bits of V8..V15; guest Q checks cover128. */
    for(unsigned i=0;i<8;i++)host_bad|=(uint64_t)observed.q[i]!=UINT64_C(0x0101010101010101)*(8+i);
    host_bad|=observed.fpcr!=fpcr||observed.fpsr!=fpsr||observed.tls!=w->host_tls||observed.x18!=w->host_x18||observed.sp_before!=observed.sp_after;
    w->host_failures+=host_bad;failed|=host_bad;w->calls++;w->failures+=failed;return !failed;
}
static void *worker(void *argument){
    Worker *w=argument;size_t page=(size_t)sysconf(_SC_PAGESIZE),usable=131072,total=usable+2*page;
    unsigned char *mapping=mmap(NULL,total,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);Cpu *cpu=NULL,*expected=NULL,*alternate=NULL;
    if(mapping==MAP_FAILED||mprotect(mapping+page,usable,PROT_READ|PROT_WRITE)||posix_memalign((void**)&cpu,16,sizeof *cpu)||posix_memalign((void**)&expected,16,sizeof *expected)||posix_memalign((void**)&alternate,16,sizeof *alternate)){w->failures++;goto ready;}
    unsigned char *stack=mapping+page;uintptr_t top=(uintptr_t)(stack+usable-128);uint64_t cookie=UINT64_C(0xfeed012345678900)^w->index;
    for(unsigned i=0;i<8;i++){((uint64_t*)stack)[i]=cookie^i;((uint64_t*)(stack+usable-64))[i]=~cookie^i;}
    w->guards=2;w->host_tls=tls_value();w->host_x18=x18_value();
ready:
    atomic_fetch_add(w->ready,1);while(!atomic_load(w->start)){}
    if(!cpu||!expected||!alternate||mapping==MAP_FAILED)goto done;
    uint64_t old_fpcr=rt_get_fpcr(),old_fpsr=rt_get_fpsr();rt_set_fpcr(0x00800000);rt_set_fpsr(0x1f);uint64_t host_fpcr=rt_get_fpcr(),host_fpsr=rt_get_fpsr();
    for(unsigned it=0;it<ITERATIONS;it++){
        seed(cpu,top,w->index*ITERATIONS+it);memcpy(expected,cpu,sizeof *cpu);
        for(unsigned i=0;i<31;i++)if(i!=18&&i!=19)expected->x[i]+=i+1;expected->nzcv=0x60000000;
        check_run(w,cpu,expected,probe_gpr_words,sizeof probe_gpr_words/4,PROBE_GPR_MASK,host_fpcr,host_fpsr);
        seed(cpu,top,it);memcpy(expected,cpu,sizeof *cpu);for(unsigned i=0;i<31;i++)expected->v[i]^=cpu->v[31];expected->v[31]=0;
        check_run(w,cpu,expected,probe_vector_words,sizeof probe_vector_words/4,PROBE_VECTOR_MASK,host_fpcr,host_fpsr);
        seed(cpu,top,it);cpu->v[1]=(cpu->v[1]&~(__uint128_t)0xffffffffu)|0x3f800000u;cpu->v[2]&=~(__uint128_t)0xffffffffu;memcpy(expected,cpu,sizeof *cpu);expected->v[0]=0x7f800000u;expected->fpsr|=2;
        check_run(w,cpu,expected,probe_fp_words,sizeof probe_fp_words/4,PROBE_FP_MASK,host_fpcr,host_fpsr);
        seed(cpu,top,it);memset((void*)(top-64),0xa5,64);unsigned char memory_expected[64];memset(memory_expected,0xa5,64);
        memcpy(memory_expected,&cpu->x[16],16);memcpy(memory_expected+16,&cpu->x[29],16);memcpy(memory_expected+32,&cpu->v[31],16);
        memcpy(expected,cpu,sizeof *cpu);expected->sp-=64;expected->x[16]=cpu->x[29];expected->x[17]=cpu->x[30];expected->x[29]=cpu->x[16];expected->x[30]=cpu->x[17];expected->v[0]=cpu->v[31];expected->x[0]=expected->sp;
        check_run(w,cpu,expected,probe_stack_words,sizeof probe_stack_words/4,PROBE_STACK_MASK,host_fpcr,host_fpsr);int memory_bad=memcmp((void*)(top-64),memory_expected,64)!=0;w->memory_failures+=memory_bad;w->failures+=memory_bad;
        expected->x[1]=((uint64_t*)memory_expected)[0];expected->x[2]=((uint64_t*)memory_expected)[1];expected->sp+=64;
        check_run(w,cpu,expected,probe_pop_words,sizeof probe_pop_words/4,PROBE_POP_MASK,host_fpcr,host_fpsr);
        seed(cpu,top,it);memcpy(expected,cpu,sizeof *cpu);expected->x[19]+=7;expected->x[16]=expected->x[19]+cpu->x[17];expected->v[19]^=cpu->v[18];expected->x[30]+=11;expected->x[1]=expected->x[19]+1;expected->x[20]=expected->x[19];expected->nzcv=0;
        check_run(w,cpu,expected,probe_context19_words,sizeof probe_context19_words/4,PROBE_CONTEXT19_MASK,host_fpcr,host_fpsr);
        seed(cpu,top,it);memset((void*)(top-32),0xa5,32);unsigned char stack19_expected[32];memset(stack19_expected,0xa5,32);memcpy(stack19_expected,&cpu->x[16],16);uint64_t new19=top+16;memcpy(stack19_expected+16,&new19,8);
        memcpy(expected,cpu,sizeof *cpu);expected->sp-=32;expected->x[19]=new19;expected->x[20]=cpu->x[16];expected->x[21]=cpu->x[17];
        check_run(w,cpu,expected,probe_stack19_words,sizeof probe_stack19_words/4,PROBE_STACK19_MASK,host_fpcr,host_fpsr);memory_bad=memcmp((void*)(top-32),stack19_expected,32)!=0;w->memory_failures+=memory_bad;w->failures+=memory_bad;
        seed(cpu,top,it);memcpy(expected,cpu,sizeof *cpu);for(unsigned i=0;i<31;i++)if(i!=17&&i!=18)expected->x[i]+=i+1;expected->nzcv=0x60000000;
        check_run(w,cpu,expected,probe_temp17_words,sizeof probe_temp17_words/4,PROBE_TEMP17_MASK,host_fpcr,host_fpsr);
        seed(cpu,top,it);memcpy(expected,cpu,sizeof *cpu);for(unsigned i=0;i<31;i++)if(i!=18&&i!=30)expected->x[i]+=i+1;expected->nzcv=0x60000000;
        check_run(w,cpu,expected,probe_temp30_words,sizeof probe_temp30_words/4,PROBE_TEMP30_MASK,host_fpcr,host_fpsr);
        w->failures+=!guards_ok(stack,usable,cookie)||tls_value()!=w->host_tls||x18_value()!=w->host_x18;
    }
    for(unsigned it=0;it<CHURN_ITERATIONS;it++){
        Cpu *current=(it&1)?alternate:cpu;seed(current,top,1000+w->index*CHURN_ITERATIONS+it);
        current->v[1]=(current->v[1]&~(__uint128_t)0xffffffffu)|0x3f800000u;current->v[2]&=~(__uint128_t)0xffffffffu;
        uint32_t words[64];size_t count;uint32_t usage;unsigned kind=it%3;
        memcpy(expected,current,sizeof *current);
        if(!kind){count=sizeof probe_context19_words/4;memcpy(words,probe_context19_words,count*4);usage=PROBE_CONTEXT19_MASK;
            words[0]=(words[0]&~0x003ffc00u)|((it+7)<<10);expected->x[19]+=it+7;expected->x[16]=expected->x[19]+current->x[17];expected->v[19]^=current->v[18];expected->x[30]+=11;expected->x[1]=expected->x[19]+1;expected->x[20]=expected->x[19];expected->nzcv=0;
        }else{
            const uint32_t *original=kind==1?probe_temp17_words:probe_temp30_words;count=kind==1?sizeof probe_temp17_words/4:sizeof probe_temp30_words/4;memcpy(words,original,count*4);usage=kind==1?PROBE_TEMP17_MASK:PROBE_TEMP30_MASK;
            words[0]=(words[0]&~0x003ffc00u)|((it+1)<<10);for(unsigned i=0;i<31;i++)if(i!=18&&i!=(kind==1?17:30))expected->x[i]+=i?i+1:it+1;expected->nzcv=0x60000000;
        }
        words[count++]=probe_fp_words[0];expected->v[0]=0x7f800000u;expected->fpsr|=2;
        check_run(w,current,expected,words,count,usage,host_fpcr,host_fpsr);
    }
    rt_set_fpcr(old_fpcr);rt_set_fpsr(old_fpsr);
done:
    free(cpu);free(expected);free(alternate);if(mapping!=MAP_FAILED)munmap(mapping,total);return NULL;
}

typedef struct UcApi {
    uc_err(*open)(uc_arch,uc_mode,uc_engine**);uc_err(*close)(uc_engine*);
    uc_err(*map)(uc_engine*,uint64_t,size_t,uint32_t,void*);
    uc_err(*read)(uc_engine*,int,void*);uc_err(*write)(uc_engine*,int,const void*);
    uc_err(*start)(uc_engine*,uint64_t,uint64_t,uint64_t,size_t);
} UcApi;
static int gpr_id(unsigned i){return i<29?UC_ARM64_REG_X0+(int)i:i==29?UC_ARM64_REG_X29:UC_ARM64_REG_X30;}
static int uc_sync(UcApi *api,uc_engine *uc,Cpu *cpu,int writing){
#define SYNC(id,ptr) do{if((writing?api->write(uc,id,ptr):api->read(uc,id,ptr))!=UC_ERR_OK)return 0;}while(0)
    for(unsigned i=0;i<31;i++)SYNC(gpr_id(i),&cpu->x[i]);
    SYNC(UC_ARM64_REG_SP,&cpu->sp);SYNC(UC_ARM64_REG_PC,&cpu->pc);SYNC(UC_ARM64_REG_NZCV,&cpu->nzcv);SYNC(UC_ARM64_REG_TPIDR_EL0,&cpu->tpidr);SYNC(UC_ARM64_REG_TPIDRRO_EL0,&cpu->tpidrro);SYNC(UC_ARM64_REG_FPCR,&cpu->fpcr);SYNC(UC_ARM64_REG_FPSR,&cpu->fpsr);
    for(unsigned i=0;i<32;i++)SYNC(UC_ARM64_REG_Q0+(int)i,&cpu->v[i]);
#undef SYNC
    return 1;
}
static int uc_step(UcApi *api,uc_engine *uc,Cpu *cpu){return uc_sync(api,uc,cpu,1)&&api->start(uc,cpu->pc,UINT64_MAX,0,1)==UC_ERR_OK&&uc_sync(api,uc,cpu,0);}
static int unicorn_cases(const char *path,unsigned *success,unsigned *interference,unsigned *clear){
    int failed=0;void *library=dlopen(path,RTLD_NOW|RTLD_LOCAL);UcApi api={0};uc_engine *uc=NULL;void *code=MAP_FAILED,*data=MAP_FAILED;size_t page=(size_t)sysconf(_SC_PAGESIZE);
    if(!library)return 1;
#define LOAD(field,name) do{void *symbol=dlsym(library,name);memcpy(&api.field,&symbol,sizeof symbol);if(!api.field){failed=1;goto done;}}while(0)
    LOAD(open,"uc_open");LOAD(close,"uc_close");LOAD(map,"uc_mem_map_ptr");LOAD(read,"uc_reg_read");LOAD(write,"uc_reg_write");LOAD(start,"uc_emu_start");
#undef LOAD
    code=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);data=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(code==MAP_FAILED||data==MAP_FAILED||api.open(UC_ARCH_ARM64,UC_MODE_ARM,&uc)!=UC_ERR_OK){failed=1;goto done;}
    memcpy(code,probe_exclusive_words,sizeof probe_exclusive_words);uint64_t cpacr=3u<<20;
    if(api.write(uc,UC_ARM64_REG_CPACR_EL1,&cpacr)!=UC_ERR_OK||api.map(uc,(uintptr_t)code,page,UC_PROT_ALL,code)!=UC_ERR_OK||api.map(uc,(uintptr_t)data,page,UC_PROT_ALL,data)!=UC_ERR_OK){failed=1;goto done;}
    Cpu cpu;uintptr_t base=(uintptr_t)code;uint64_t *slot=data;
    for(unsigned i=0;i<64;i++){
        seed(&cpu,(uintptr_t)data+page-64,i);cpu.pc=base;cpu.x[2]=(uintptr_t)data;*slot=41+i;
        if(!uc_step(&api,uc,&cpu)||cpu.x[0]!=41+i||!uc_step(&api,uc,&cpu)||!uc_step(&api,uc,&cpu)||cpu.x[1]!=0||*slot!=42+i)failed++;else(*success)++;
    }
    seed(&cpu,(uintptr_t)data+page-64,0);cpu.pc=base;cpu.x[2]=(uintptr_t)data;*slot=5;
    if(!uc_step(&api,uc,&cpu))failed++;else{
        __atomic_store_n(slot,9,__ATOMIC_SEQ_CST);
        if(!uc_step(&api,uc,&cpu)||!uc_step(&api,uc,&cpu)||cpu.x[1]!=1||*slot!=9)failed++;else(*interference)++;
    }
    seed(&cpu,(uintptr_t)data+page-64,0);cpu.pc=base;cpu.x[2]=(uintptr_t)data;*slot=11;
    if(!uc_step(&api,uc,&cpu))failed++;else{
        cpu.pc=base+12;if(!uc_step(&api,uc,&cpu))failed++;else{
            cpu.pc=base+8;cpu.x[0]=12;if(!uc_step(&api,uc,&cpu)||cpu.x[1]!=1||*slot!=11)failed++;else(*clear)++;
        }
    }
done:
    if(uc)api.close(uc);if(code!=MAP_FAILED)munmap(code,page);if(data!=MAP_FAILED)munmap(data,page);dlclose(library);return failed;
}
__attribute__((visibility("default"))) int native_blocks_probe(const char *unicorn_path,char *output,size_t capacity){
    if(!unicorn_path||!output||capacity<4096)return -1;
    Cpu denied={.sp=0x10000},denied_before=denied;
    unsigned resource_rejections=resource_decline_cases(),rejected=0;
    rejected+=native_blocks_run(&denied,probe_gpr_words,1,UINT32_C(1)<<18)==1 && !memcmp(&denied,&denied_before,sizeof denied);
    rejected+=native_blocks_run(&denied,probe_gpr_words,1,NGB_CONTEXT_GPR_MASK|(UINT32_C(1)<<19))==1 && !memcmp(&denied,&denied_before,sizeof denied);
    Worker workers[THREADS]={0};pthread_t threads[THREADS];_Atomic unsigned ready=0,start=0;unsigned created=0,failures=0,calls=0,guards=0,arch_failures=0,host_failures=0,memory_failures=0;
    for(unsigned i=0;i<THREADS;i++){workers[i].index=i;workers[i].ready=&ready;workers[i].start=&start;if(pthread_create(&threads[i],NULL,worker,&workers[i])){failures++;break;}created++;}
    while(atomic_load(&ready)!=created){}atomic_store(&start,1);
    for(unsigned i=0;i<created;i++){if(pthread_join(threads[i],NULL))abort();failures+=workers[i].failures;arch_failures+=workers[i].arch_failures;host_failures+=workers[i].host_failures;memory_failures+=workers[i].memory_failures;calls+=workers[i].calls;guards+=workers[i].guards;}
    unsigned success=0,interference=0,clear=0;int uc_failures=unicorn_cases(unicorn_path,&success,&interference,&clear);failures+=uc_failures!=0;failures+=calls!=THREADS*(ITERATIONS*9+CHURN_ITERATIONS)||rejected!=2||resource_rejections!=2||guards!=THREADS*2;
    NativeBlocksCacheStats statistics;native_blocks_cache_stats(&statistics);
    failures+=statistics.global_pages>statistics.global_page_limit||statistics.overflow_live_pages||atomic_load(&protection_violations);
    if(statistics.global_page_limit<=2)failures+=statistics.global_pages!=statistics.global_page_limit||!statistics.overflow_rewrites||statistics.overflow_peak_pages>THREADS*2*statistics.overflow_slots_per_cpu;
    snprintf(output,capacity,"{\"schema\":1,\"state\":\"%s\",\"failures\":%u,\"native_calls\":%u,\"global_cached_pages\":%llu,\"global_page_limit\":%llu,\"overflow_slots_per_cpu\":%llu,\"overflow_live_pages_after_join\":%llu,\"overflow_peak_pages\":%llu,\"overflow_rewrites\":%llu,\"protection_violations\":%u,\"resource_declines_before_effect\":%u,\"native_arch_failures\":%u,\"host_abi_failures\":%u,\"native_stack_memory_failures\":%u,\"threads_joined\":%u,\"guard_pages\":%u,\"guest_x19_dynamic_context_checked\":true,\"forced_temp17_temp30_checked\":true,\"rejected_unsafe_masks\":%u,\"guest_gpr_16_17_30_checked\":true,\"guest_q128_checked\":true,\"opaque_cpu_checked\":true,\"host_callee_abi_tls_x18_fp_checked\":true,\"unicorn_failures\":%d,\"unicorn_exclusive_success\":%u,\"unicorn_interference_failure\":%u,\"unicorn_clrex_failure\":%u,\"synthetic_cpu_only\":true,\"game_execution\":false}",failures?"failed":"passed",failures,calls,(unsigned long long)statistics.global_pages,(unsigned long long)statistics.global_page_limit,(unsigned long long)statistics.overflow_slots_per_cpu,(unsigned long long)statistics.overflow_live_pages,(unsigned long long)statistics.overflow_peak_pages,(unsigned long long)statistics.overflow_rewrites,atomic_load(&protection_violations),resource_rejections,arch_failures,host_failures,memory_failures,created,guards,rejected,uc_failures,success,interference,clear);return failures!=0;
}
#endif
