#define _GNU_SOURCE
#include "continuous_image.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static unsigned checks;
#ifdef CNCE_IMAGE_WRAP_PROTECTION
static _Atomic unsigned protection_calls,fail_protection;
int __real_mprotect(void *address,size_t length,int protection);
int __wrap_mprotect(void *address,size_t length,int protection) {
    unsigned fail=atomic_load_explicit(&fail_protection,memory_order_relaxed);
    unsigned call=atomic_fetch_add_explicit(&protection_calls,1,memory_order_relaxed)+1;
    if(fail&&call==fail){atomic_store_explicit(&fail_protection,0,memory_order_relaxed);return -1;}
    return __real_mprotect(address,length,protection);
}
#endif
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
static uint32_t word_at(uint64_t pc){uint32_t word;memcpy(&word,(void*)(uintptr_t)pc,4);return word;}
static uint64_t branch_target(uint64_t pc,uint32_t word){int64_t imm=(int64_t)((word&0x3ffffff)^0x2000000)-0x2000000;return pc+(uint64_t)(imm*4);}
static void permissions(uint64_t address,char result[5]) {
    FILE *file=fopen("/proc/self/maps","r");CHECK(file);char line[2048],mode[5];unsigned long long lo,hi;result[0]=0;
    while(fgets(line,sizeof line,file))if(sscanf(line,"%llx-%llx %4s",&lo,&hi,mode)==3&&address>=lo&&address<hi){memcpy(result,mode,5);break;}
    fclose(file);CHECK(result[0]);
}
typedef struct Worker {Cpu cpu;uint64_t base;int ok;} Worker;
static void *worker(void *opaque) {
    Worker *w=opaque;w->ok=1;
    for(unsigned i=0;i<300;i++) {
        uint64_t target=w->base+256+4*(i%180);const void *code=continuous_image_resume(&w->cpu,target);CnceImagePcInfo info;
        if(!code||!continuous_image_pc_info((uintptr_t)code,&info)||info.kind!=CNCE_IMAGE_PC_RESUME||info.original_pc!=target)w->ok=0;
    }
    return NULL;
}
#define MIB UINT64_C(0x100000)
#define BANK_BYTES (16u*1024u*1024u)
static void no_bank_unchanged(const SessionLayout *layout,const CnceImageModule *modules,
                              uint64_t lead,const char *reason) {
    /* Snapshot every range preparation can write: text, both candidate banks,
     * and HLE. A no-fit rejection must not even alter their protections. */
    SessionRange ranges[]={{lead,lead+BANK_BYTES},
        {modules[0].base+modules[0].text_offset,modules[0].base+modules[0].text_offset+layout->page_size},
        {modules[0].base+modules[0].image_size,modules[0].base+modules[0].image_size+BANK_BYTES},
        layout->regions[SESSION_HLE]};
    unsigned char *snapshots[4];
    for(unsigned i=0;i<4;i++){size_t n=(size_t)(ranges[i].hi-ranges[i].lo);snapshots[i]=malloc(n);CHECK(snapshots[i]);memcpy(snapshots[i],(void*)(uintptr_t)ranges[i].lo,n);}
#ifdef CNCE_IMAGE_WRAP_PROTECTION
    atomic_store(&protection_calls,0);
#endif
    char error[256],mode[5];
    CHECK(continuous_image_prepare(layout,modules,layout->module_count,NULL,0,0x12340000,error,sizeof error)==-1);
    CHECK(strstr(error,"has no branch-reachable trampoline bank")&&strstr(error,reason));
    CHECK(strstr(error,"text +0x")&&strstr(error,"after +0x")&&!continuous_image_return_target());
#ifdef CNCE_IMAGE_WRAP_PROTECTION
    CHECK(atomic_load(&protection_calls)==0);
#endif
    for(unsigned i=0;i<4;i++){
        CHECK(!memcmp(snapshots[i],(void*)(uintptr_t)ranges[i].lo,(size_t)(ranges[i].hi-ranges[i].lo)));
        permissions(ranges[i].lo,mode);CHECK(!strcmp(mode,"rw-p"));free(snapshots[i]);
    }
}
static void before_bank_checks(void) {
    size_t page=(size_t)sysconf(_SC_PAGESIZE),size=320u*1024u*1024u;
    unsigned char *mapping=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(mapping!=MAP_FAILED);
    uint64_t lead=(uintptr_t)mapping,base=lead+BANK_BYTES;
    SessionLayout layout={.struct_size=sizeof layout,.version=SESSION_LAYOUT_VERSION,.module_count=1,
        .page_size=(uint32_t)page,.window={lead,lead+size}};
    CnceImageModule modules[2]={{base,256*MIB,0,page},{lead+8*MIB,page,0,4}};
    layout.modules[0].image=(SessionRange){base,base+modules[0].image_size};
    layout.regions[SESSION_HLE]=(SessionRange){lead+292*MIB,lead+293*MIB};
    for(unsigned r=1;r<SESSION_REGION_COUNT;r++)layout.regions[r]=(SessionRange){lead+(300+r)*MIB,lead+(301+r)*MIB};
    uint32_t svc=0xd4000021,nop=0xd503201f;
    for(size_t off=0;off<page;off+=4)memcpy((void*)(uintptr_t)(base+off),&nop,4);
    memcpy((void*)(uintptr_t)base,&svc,4);memcpy((void*)(uintptr_t)(base+page-4),&svc,4);
    memcpy((void*)(uintptr_t)modules[1].base,&nop,4);
    SessionLayout bad=layout;bad.window.lo=base;
    no_bank_unchanged(&bad,modules,lead,"before [-0x1000000,0) outside");
    bad=layout;bad.regions[SESSION_TLS]=(SessionRange){lead,lead+page};
    no_bank_unchanged(&bad,modules,lead,"before [-0x1000000,0) occupied");
    bad=layout;bad.module_count=2;bad.modules[1].image=(SessionRange){modules[1].base,modules[1].base+page};
    no_bank_unchanged(&bad,modules,lead,"before [-0x1000000,0) occupied");
    /* Module zero can select the lead-in, then a later module can fail to
     * fit. Cleanup must leave even the already selected bank untouched. */
    modules[1]=(CnceImageModule){base+modules[0].image_size,20*MIB,0,4};
    memcpy((void*)(uintptr_t)modules[1].base,&nop,4);
    bad=layout;bad.module_count=2;
    bad.modules[1].image=(SessionRange){modules[1].base,modules[1].base+modules[1].image_size};
    no_bank_unchanged(&bad,modules,lead,"module 1 has no branch-reachable trampoline bank");
    /* Exact signed B26 endpoints, including the asymmetric positive bound. */
    uint32_t branch;
    CHECK(cnce_encode_b26(base,base+0x7fffffc,&branch)==CNCE_PATCH_OK);
    CHECK(cnce_encode_b26(base,base+0x8000000,&branch)==CNCE_PATCH_RANGE);
    CHECK(cnce_encode_b26(base+0x8000000,base,&branch)==CNCE_PATCH_OK);
    CHECK(cnce_encode_b26(base+0x8000004,base,&branch)==CNCE_PATCH_RANGE);
    char error[256],mode[5];
#ifdef CNCE_IMAGE_WRAP_PROTECTION
    /* Exercise rollback after publishing a before-image bank, including after
     * its original text has become RX. The owned gap stays reusable and RW. */
    for(unsigned fail=1;fail<=4;fail++) {
        atomic_store(&protection_calls,0);atomic_store(&fail_protection,fail);
        CHECK(continuous_image_prepare(&layout,modules,1,NULL,0,0x12340000,error,sizeof error)==-1);
        CHECK(word_at(base)==svc&&!continuous_image_site_at(base)&&!continuous_image_return_target());
        CHECK(!word_at(layout.regions[SESSION_HLE].lo)&&!word_at(layout.regions[SESSION_HLE].hi-page));
        permissions(lead,mode);CHECK(!strcmp(mode,"rw-p"));
        permissions(base,mode);CHECK(!strcmp(mode,"rw-p"));
        permissions(layout.regions[SESSION_HLE].lo,mode);CHECK(!strcmp(mode,"rw-p"));
    }
#endif
    for(unsigned boundary=0;boundary<2;boundary++) {
        modules[0].text_offset=boundary?112*MIB:0;
        modules[0].text_size=boundary?4:page;
        uint64_t text=base+modules[0].text_offset;memcpy((void*)(uintptr_t)text,&svc,4);
        CHECK(continuous_image_prepare(&layout,modules,1,NULL,0,0x12340000,error,sizeof error)==0);
        uint64_t exit_pc=branch_target(text,word_at(text));
        CHECK(exit_pc==lead&&exit_pc+CNCE_EXIT_STUB_SIZE<=base);
        uint64_t endpoints[]={lead,base-4};
        for(unsigned i=0;i<2;i++){
            CHECK(cnce_encode_b26(text,endpoints[i],&branch)==CNCE_PATCH_OK);
            CHECK(cnce_encode_b26(text+modules[0].text_size-4,endpoints[i],&branch)==CNCE_PATCH_OK);
        }
        if(!boundary)CHECK(branch_target(text+page-4,word_at(text+page-4))==lead+CNCE_EXIT_STUB_SIZE);
        permissions(lead,mode);CHECK(!strcmp(mode,"r-xp"));
        continuous_image_finish_after_join();CHECK(word_at(text)==svc);
        permissions(lead,mode);CHECK(!strcmp(mode,"rw-p"));
    }
    /* Moving only the final word four bytes further makes the lead-in fail.
     * The after-image fallback remains beyond positive B26 range. */
    modules[0].text_size=8;memcpy((void*)(uintptr_t)(base+112*MIB+4),&nop,4);
    no_bank_unchanged(&layout,modules,lead,"before [-0x1000000,0) B26");
    /* The existing after-image fallback accepts the exact positive endpoint
     * for its complete bank, and rejects the next page beyond it. */
    modules[0].text_offset=0;modules[0].text_size=page;modules[0].image_size=112*MIB;
    layout.window.lo=base;layout.modules[0].image.hi=base+modules[0].image_size;
    CHECK(continuous_image_prepare(&layout,modules,1,NULL,0,0x12340000,error,sizeof error)==0);
    CHECK(branch_target(base,word_at(base))==base+112*MIB);
    CHECK(cnce_encode_b26(base,base+128*MIB-4,&branch)==CNCE_PATCH_OK);
    continuous_image_finish_after_join();CHECK(word_at(base)==svc&&word_at(base+page-4)==svc);
    modules[0].image_size+=page;layout.modules[0].image.hi+=page;
    no_bank_unchanged(&layout,modules,lead,"after +0x");
    CHECK(!munmap(mapping,size));
}
int main(void) {
    before_bank_checks();
    size_t page=(size_t)sysconf(_SC_PAGESIZE),window_size=96u*1024u*1024u;unsigned char *mapping=mmap(NULL,window_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(mapping!=MAP_FAILED);uint64_t base=(uintptr_t)mapping;
    SessionLayout layout={.struct_size=sizeof layout,.version=SESSION_LAYOUT_VERSION,.module_count=2,.page_size=(uint32_t)page,.window={base,base+window_size}};
    CnceImageModule modules[]={{base,2u*1024u*1024u,0,page},{base+4u*1024u*1024u,2u*1024u*1024u,0,page}};
    for(unsigned i=0;i<2;i++)layout.modules[i].image=(SessionRange){modules[i].base,modules[i].base+modules[i].image_size};
    layout.regions[SESSION_HLE]=(SessionRange){base+64u*1024u*1024u,base+65u*1024u*1024u};
    for(unsigned r=1;r<SESSION_REGION_COUNT;r++)layout.regions[r]=(SessionRange){base+(70u+r)*1024u*1024u,base+(71u+r)*1024u*1024u};
    for(unsigned m=0;m<2;m++)for(size_t off=0;off<page;off+=4){uint32_t nop=0xd503201f;memcpy((void*)(uintptr_t)(modules[m].base+off),&nop,4);}
    const uint32_t original[]={0xd503201f,0x94000000,0xd503201f,0xd63f0000,0xd503201f,0x91000652,0x14000001,0x10000000,0xc85f7c40,0xd4000021,0xd53bd053,0x58000000,0xf90003e0};
    memcpy(mapping,original,sizeof original);unsigned char *snapshot=malloc(page);CHECK(snapshot);memcpy(snapshot,mapping,page);
    CncePatchRequest requests[]={{.pc=base+48,.kind=CNCE_PATCH_INSTRUCTION_HOOK,.payload=77},{.pc=base+44,.kind=CNCE_PATCH_INSTRUCTION_HOOK},{.pc=base+32,.kind=CNCE_PATCH_HOST_FUNCTION},{.pc=base+28,.kind=CNCE_PATCH_INSTRUCTION_HOOK},{.pc=base+24,.kind=CNCE_PATCH_INSTRUCTION_HOOK},{.pc=base+20,.kind=CNCE_PATCH_INSTRUCTION_HOOK}};
    char error[256],mode[5];SessionLayout bad=layout;bad.regions[SESSION_TLS]=bad.regions[SESSION_HLE];
    CHECK(continuous_image_prepare(&bad,modules,2,requests,6,0x12340000,error,sizeof error)==-1&&!memcmp(mapping,snapshot,page));
    uint32_t unsupported=0xd5380000;memcpy(mapping,&unsupported,4);
    CHECK(continuous_image_prepare(&layout,modules,2,NULL,0,0x12340000,error,sizeof error)==-1);
    CHECK(strstr(error,"module 0 patch collection: unsupported system instruction")&&strstr(error,"text offset 0x0"));
    CHECK(word_at(base)==unsupported&&!continuous_image_site_at(base));memcpy(mapping,snapshot,page);
    CncePatchRequest duplicate[]={requests[0],requests[0]};
    CHECK(continuous_image_prepare(&layout,modules,2,duplicate,2,0x12340000,error,sizeof error)==-1&&!memcmp(mapping,snapshot,page));
#ifdef CNCE_IMAGE_WRAP_PROTECTION
    /* Fail every publication/protection stage, including after one module has
     * already become RX. Rollback must restore original bytes and RW ownership. */
    for(unsigned fail=1;fail<=6;fail++) {
        atomic_store(&protection_calls,0);atomic_store(&fail_protection,fail);
        CHECK(continuous_image_prepare(&layout,modules,2,requests,6,0x12340000,error,sizeof error)==-1);
        CHECK(!memcmp(mapping,snapshot,page)&&!continuous_image_site_at(base+36));
        permissions(base,mode);CHECK(!strcmp(mode,"rw-p"));
        permissions(layout.regions[SESSION_HLE].lo,mode);CHECK(!strcmp(mode,"rw-p"));
        CHECK(!word_at(layout.regions[SESSION_HLE].lo)&&!word_at(layout.regions[SESSION_HLE].hi-page));
    }
#endif
    CHECK(!continuous_image_site_at(base+36)&&!continuous_image_return_target());
    CHECK(continuous_image_prepare(&layout,modules,2,requests,6,0x12340000,error,sizeof error)==0&&!error[0]);
    CHECK(continuous_image_prepare(&layout,modules,2,requests,6,0x12340000,error,sizeof error)==-1);
    const CnceSite *site=continuous_image_site_at(base+36);CHECK(site&&site->kind==CNCE_PATCH_SVC&&site->operand==1&&site->original_word==original[9]);
    CHECK(continuous_image_valid_event(site)&&!continuous_image_valid_event((const CnceSite*)((uintptr_t)site+1))&&!continuous_image_valid_event(NULL));
    permissions((uintptr_t)site,mode);CHECK(!strcmp(mode,"r--p"));permissions(base,mode);CHECK(!strcmp(mode,"r-xp"));permissions(layout.regions[SESSION_HLE].lo,mode);CHECK(!strcmp(mode,"r-xp"));
    uint64_t exit_pc=branch_target(base+36,word_at(base+36));CHECK(exit_pc>=base+6u*1024u*1024u&&exit_pc<base+38u*1024u*1024u);
    CnceImagePcInfo info;for(unsigned i=0;i<4;i++)CHECK(continuous_image_pc_info(exit_pc+i*4,&info)&&info.kind==CNCE_IMAGE_PC_EXIT&&info.instruction_index==i&&info.original_pc==base+36&&info.site==site);
    CHECK(!continuous_image_pc_info(exit_pc+16,&info));CHECK(!continuous_image_is_original_guest_pc(exit_pc));CHECK(continuous_image_is_original_guest_pc(base)&&continuous_image_is_original_guest_pc(modules[1].base));
    CHECK(!continuous_image_is_original_guest_pc(layout.regions[SESSION_HLE].lo)&&!continuous_image_is_original_guest_pc(base+page));
    for(unsigned i=0;i<4096;i++) {
        uint64_t pc=layout.regions[SESSION_HLE].lo+16*i;site=continuous_image_site_at(pc);
        CHECK(site&&site->kind==CNCE_PATCH_HOST_FUNCTION&&continuous_image_valid_event(site));
        CHECK((word_at(pc)&0xfc000000u)==0x14000000u);
    }
    uint64_t sentinel=continuous_image_return_target();CHECK(sentinel>=layout.regions[SESSION_HLE].lo+65536&&sentinel<layout.regions[SESSION_HLE].hi);
    CHECK(continuous_image_valid_event(continuous_image_site_at(sentinel))&&!continuous_image_is_original_guest_pc(sentinel));
    Cpu cpu={0},other={0};const void *resume=continuous_image_resume(&cpu,base+8);CHECK(resume&&word_at((uintptr_t)resume)==0xa8c147f0);
    CHECK(continuous_image_resume(&other,base+8)==resume); /* immutable return cache */
    CHECK(continuous_image_pc_info((uintptr_t)resume,&info)&&info.kind==CNCE_IMAGE_PC_RESUME&&info.original_pc==base+8);
    CHECK(continuous_image_pc_info((uintptr_t)resume+4,&info)&&info.instruction_index==1);
    CHECK(continuous_image_resume(&cpu,base+16));CHECK(continuous_image_resume(&cpu,base+40));
    CHECK(continuous_image_resume(&cpu,layout.regions[SESSION_HLE].lo+16*4095));
    CHECK(!continuous_image_resume(&cpu,base+page)&&!continuous_image_resume(&cpu,base+1)&&!continuous_image_resume(NULL,base));
    const void *dynamic=continuous_image_resume(&cpu,base+256);CHECK(dynamic&&continuous_image_resume(&cpu,base+256)==dynamic);
    const void *dynamic_other=continuous_image_resume(&other,base+256);CHECK(dynamic_other&&((uintptr_t)dynamic&~(page-1))!=((uintptr_t)dynamic_other&~(page-1)));
    permissions((uintptr_t)dynamic,mode);CHECK(!strcmp(mode,"r-xp"));
    site=continuous_image_site_at(base+20);const void *escape=continuous_image_escape(&cpu,site);CHECK(escape&&word_at((uintptr_t)escape)==0xa8c147f0&&word_at((uintptr_t)escape+4)==original[5]);
    for(unsigned i=0;i<3;i++)CHECK(continuous_image_pc_info((uintptr_t)escape+i*4,&info)&&info.kind==CNCE_IMAGE_PC_ESCAPE&&info.instruction_index==i&&info.original_pc==(i==2?base+24:base+20));
    CHECK(!continuous_image_pc_info((uintptr_t)escape+12,&info));
    CHECK(!continuous_image_escape(&cpu,continuous_image_site_at(base+24))&&!continuous_image_escape(&cpu,continuous_image_site_at(base+28))&&!continuous_image_escape(&cpu,continuous_image_site_at(base+44)));
    escape=continuous_image_escape(&cpu,continuous_image_site_at(base+32));CHECK(escape&&word_at((uintptr_t)escape+4)==(original[8]|0x8000u));
    CHECK(continuous_image_escape(&cpu,continuous_image_site_at(base+48)));
    Worker *workers=calloc(4,sizeof *workers);CHECK(workers);pthread_t threads[4];
    for(unsigned i=0;i<4;i++){workers[i].base=base;CHECK(!pthread_create(&threads[i],NULL,worker,&workers[i]));}
    for(unsigned i=0;i<4;i++){CHECK(!pthread_join(threads[i],NULL));CHECK(workers[i].ok);}
    continuous_image_finish_after_join();CHECK(!continuous_image_site_at(base+36)&&!continuous_image_pc_info(exit_pc,&info)&&!continuous_image_return_target());
    CHECK(!memcmp(mapping,snapshot,page));permissions(base,mode);CHECK(!strcmp(mode,"rw-p"));permissions(layout.regions[SESSION_HLE].lo,mode);CHECK(!strcmp(mode,"rw-p"));
    CHECK(continuous_image_prepare(&layout,modules,2,requests,6,0x12340000,error,sizeof error)==0);continuous_image_finish_after_join();CHECK(!memcmp(mapping,snapshot,page));
    free(workers);free(snapshot);CHECK(!munmap(mapping,window_size));
    printf("{\"state\":\"passed\",\"checks\":%u,\"fake_hle\":4096,\"concurrent_cache_owners\":4,\"cache_evictions\":true,\"rw_then_rx_no_rwx\":true,\"fixed_bases\":true,\"actual_guest_execution\":false}\n",checks);return 0;
}
