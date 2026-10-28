/* Synthetic ISA/patch metadata only. Every address is fixture-owned. */
#include "continuous-nce/patch_plan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "continuous_nce_words.h"
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
static uint32_t read32(const void *p){uint32_t x;memcpy(&x,p,4);return x;}
static uint64_t read64(const void *p){uint64_t x;memcpy(&x,p,8);return x;}
static uint64_t branch_target(uint64_t from,uint32_t word) {
    int64_t offset=(int64_t)((word&0x03ffffffu)^0x02000000u)-0x02000000;
    return from+(uint64_t)(offset*4);
}
static void classify(void) {
    const struct {uint32_t word,kind;} systems[]={
        {0xd53bd040,CNCE_PATCH_TPIDR_READ},{0xd53bd060,CNCE_PATCH_TPIDRRO_READ},
        {0xd51bd040,CNCE_PATCH_TPIDR_WRITE},{0xd53be020,CNCE_PATCH_COUNTER_READ},
        {0xd53be040,CNCE_PATCH_COUNTER_READ},{0xd53be000,CNCE_PATCH_FREQUENCY_READ}};
    for(unsigned i=0;i<sizeof systems/sizeof systems[0];i++)for(unsigned reg=0;reg<32;reg++) {
        uint32_t kind=99,operand=99;CHECK(cnce_patch_classify(systems[i].word|reg,&kind,&operand)==1);
        CHECK(kind==systems[i].kind&&operand==reg);
    }
    for(unsigned imm=0;imm<=65535;imm++) {
        uint32_t kind=99,operand=99;
        CHECK(cnce_patch_classify(0xd4000001u|(imm<<5),&kind,&operand)==1);
        CHECK(kind==CNCE_PATCH_SVC&&operand==imm);
    }
    for(unsigned i=0;i<CNCE_EXCLUSIVE_CASE_COUNT;i++) {
        uint32_t kind,operand=99;
        CHECK(cnce_patch_classify(exclusive_cases[i].relaxed,&kind,&operand)==1);
        CHECK(kind==CNCE_PATCH_ORDERED_EXCLUSIVE&&operand==0);
        CHECK((exclusive_cases[i].relaxed|0x8000u)==exclusive_cases[i].ordered);
        CHECK(cnce_patch_classify(exclusive_cases[i].ordered,&kind,&operand)==0&&kind==0&&operand==0);
    }
    for(unsigned i=0;i<CNCE_ORDINARY_COUNT;i++) {
        uint32_t kind=99,operand=99;
        CHECK(cnce_patch_classify(ordinary_words[i],&kind,&operand)==0&&kind==0&&operand==0);
    }
    uint32_t kind=0xa5,operand=0xa5;
    CHECK(cnce_patch_classify(0xd51bd060,&kind,&operand)==-1); /* TPIDRRO write */
    CHECK(kind==0xa5&&operand==0xa5);
    CHECK(cnce_patch_classify(0xd5380000,NULL,NULL)==-1); /* MIDR_EL1 */
    CHECK(cnce_patch_classify(0xc87f0000,NULL,NULL)==-1); /* overlapping pair load */
    CHECK(cnce_patch_classify(0xc8007c00,NULL,NULL)==-1); /* status/data overlap */
}
static void zero_release_preservation(void) {
    for(unsigned size=0;size<4;size++)for(unsigned rn=0;rn<32;rn++) {
        uint32_t word=0x0800fc00u|(size<<30)|(31u<<16)|(rn<<5)|31u;
        uint32_t words[]={0xd503201f,word,0xd4000021},before[3];memcpy(before,words,sizeof words);
        uint32_t kind=99,operand=99;size_t count=99;CnceSite sites[3];
        CHECK(cnce_patch_classify(word,&kind,&operand)==0&&kind==CNCE_PATCH_NONE&&operand==0);
        CHECK(cnce_patch_collect(words,sizeof words,0x10000000,NULL,0,sites,3,&count)==CNCE_PATCH_OK&&count==1);
        CHECK(!memcmp(words,before,sizeof words)&&sites[0].original_pc==0x10000008&&sites[0].kind==CNCE_PATCH_SVC);
        CHECK(cnce_patch_classify(word&~0x8000u,NULL,NULL)==-1); /* unordered overlap still rejects */
        uint32_t nonzero=(word&~((31u<<16)|31u))|(5u<<16)|5u;
        CHECK(cnce_patch_classify(nonzero,NULL,NULL)==-1);
        uint32_t pair=word|(1u<<21);CHECK(cnce_patch_classify(pair,NULL,NULL)==-1);
        CnceSite unchanged={.original_pc=0x10000004,.resume_pc=0x10000008,.original_word=word,.original_kind=CNCE_PATCH_NONE,.kind=CNCE_PATCH_ORDERED_EXCLUSIVE};
        uint32_t output=0xa5a5a5a5;unsigned char stub[CNCE_EXIT_STUB_SIZE];memset(stub,0x5a,sizeof stub);
        CHECK(cnce_patch_emit(&unchanged,0x10010000,0x20000000,0x30000000,&output,stub,sizeof stub)==CNCE_PATCH_ARGUMENT&&output==0xa5a5a5a5);
        unchanged.kind=CNCE_PATCH_HOST_FUNCTION;CHECK(cnce_patch_emit(&unchanged,0x10010000,0x20000000,0x30000000,&output,stub,sizeof stub)==CNCE_PATCH_ARGUMENT&&output==0xa5a5a5a5);
    }
}
static void casp_classification(void) {
    for(unsigned i=0;i<CNCE_CASP_CASE_COUNT;i++) {
        uint32_t kind=99,operand=99;
        CHECK(cnce_patch_classify(casp_words[i],&kind,&operand)==0&&kind==0&&operand==0);
    }
    /* All first-register encodings, base GPR/SP, widths and orderings. Odd
     * first-register numbers are architecturally undefined, not LDXP. */
    for(unsigned size=0;size<2;size++)for(unsigned order=0;order<4;order++)
      for(unsigned rs=0;rs<32;rs++)for(unsigned rt=0;rt<32;rt++)for(unsigned rn=0;rn<32;rn++) {
        uint32_t word=0x08207c00u|(size<<30)|((order&1)<<22)|((order>>1)<<15)|(rs<<16)|(rn<<5)|rt;
        uint32_t before=word,kind=99,operand=99;
        int valid=!((rs|rt)&1);
        CHECK(cnce_patch_classify(word,&kind,&operand)==(valid?0:-1));
        CHECK(valid?(kind==0&&operand==0):(kind==99&&operand==99));
        if(valid) {
            size_t count=99;CnceSite site,prior;memset(&site,0xa5,sizeof site);prior=site;
            CHECK(cnce_patch_collect(&word,4,0x10000000,NULL,0,&site,1,&count)==CNCE_PATCH_OK&&count==0);
            CHECK(word==before&&!memcmp(&site,&prior,sizeof site));
        }
    }
    for(unsigned size=0;size<2;size++)for(unsigned order=0;order<4;order++)for(unsigned rt2=0;rt2<31;rt2++) {
        uint32_t word=0x08200002u|(size<<30)|((order&1)<<22)|((order>>1)<<15)|(rt2<<10);
        CHECK(cnce_patch_classify(word,NULL,NULL)==-1); /* fixed11111 violated */
    }
    /* These remain true exclusive pairs: invalid load Rs, overlapping outputs,
     * and store status/data or status/base overlaps must still fail closed. */
    const uint32_t invalid_exclusives[]={0x88207c82u|0x00400000u,0xc87f0000u,0x88207c80u,0xc8207c00u};
    for(unsigned i=0;i<sizeof invalid_exclusives/sizeof invalid_exclusives[0];i++)
        CHECK(cnce_patch_classify(invalid_exclusives[i],NULL,NULL)==-1);
}
static void branches(void) {
    uint32_t word=0xa5a5a5a5;const uint64_t base=UINT64_C(0x100000000);
    const int64_t deltas[]={-0x8000000,-4,0,4,0x7fffffc};
    for(unsigned i=0;i<sizeof deltas/sizeof deltas[0];i++) {
        uint64_t target=base+(uint64_t)deltas[i];CHECK(cnce_encode_b26(base,target,&word)==CNCE_PATCH_OK);
        CHECK((word&0xfc000000u)==0x14000000u&&branch_target(base,word)==target);
    }
    word=0xa5a5a5a5;
    CHECK(cnce_encode_b26(base,base+0x8000000,&word)==CNCE_PATCH_RANGE&&word==0xa5a5a5a5);
    CHECK(cnce_encode_b26(base,base-0x8000004,&word)==CNCE_PATCH_RANGE&&word==0xa5a5a5a5);
    CHECK(cnce_encode_b26(base+1,base,&word)==CNCE_PATCH_ARGUMENT&&word==0xa5a5a5a5);
    CHECK(cnce_encode_b26(base,base+1,&word)==CNCE_PATCH_ARGUMENT&&word==0xa5a5a5a5);
    CHECK(cnce_encode_b26(base,base,NULL)==CNCE_PATCH_ARGUMENT);
    CHECK(cnce_encode_b26(UINT64_MAX-3,0,&word)==CNCE_PATCH_RANGE&&word==0xa5a5a5a5);
    CHECK(cnce_encode_b26(0,UINT64_MAX-3,&word)==CNCE_PATCH_RANGE&&word==0xa5a5a5a5);
    CHECK(cnce_encode_b26(UINT64_MAX-3,UINT64_MAX-7,&word)==CNCE_PATCH_OK&&branch_target(UINT64_MAX-3,word)==UINT64_MAX-7);
    uint64_t random=17;
    for(unsigned i=0;i<20000;i++) {
        random=random*UINT64_C(6364136223846793005)+1;
        int64_t delta=((int64_t)(random&0x3ffffff)-0x2000000)*4;
        CHECK(cnce_encode_b26(base,base+(uint64_t)delta,&word)==CNCE_PATCH_OK);
        CHECK(branch_target(base,word)==base+(uint64_t)delta);
    }
}
static void collection(void) {
    uint32_t words[]={0xd503201f,0xd4000061,0xd53bd05f,0xd51bd05f,0xd53be03f,0xd53be05f,0xd53be01f,0xc85f7c40,0x14000001,0xd65f03c0};
    const uint64_t base=UINT64_C(0x12340000);CnceSite sites[16],before[16];size_t count=99;
    memset(sites,0xa5,sizeof sites);memcpy(before,sites,sizeof sites);
    CHECK(cnce_patch_collect(words,sizeof words,base,NULL,0,NULL,0,&count)==CNCE_PATCH_CAPACITY&&count==7);
    CHECK(cnce_patch_collect(words,sizeof words,base,NULL,0,sites,6,&count)==CNCE_PATCH_CAPACITY&&!memcmp(sites,before,sizeof sites));
    CHECK(cnce_patch_collect(words,sizeof words,base,NULL,0,sites,16,&count)==CNCE_PATCH_OK&&count==7);
    for(unsigned i=0;i<count;i++)CHECK(sites[i].original_pc==base+4*(i+1)&&sites[i].resume_pc==sites[i].original_pc+4&&sites[i].original_word==words[i+1]&&sites[i].kind==sites[i].original_kind);
    CncePatchRequest requests[]={{.pc=base,.kind=CNCE_PATCH_HOST_FUNCTION,.payload=123},{.pc=base+4,.kind=CNCE_PATCH_INSTRUCTION_HOOK,.payload=456}};
    CHECK(cnce_patch_collect(words,sizeof words,base,requests,2,sites,16,&count)==CNCE_PATCH_OK&&count==8);
    CHECK(sites[0].kind==CNCE_PATCH_HOST_FUNCTION&&sites[0].original_kind==0&&sites[0].payload==123);
    CHECK(sites[1].kind==CNCE_PATCH_INSTRUCTION_HOOK&&sites[1].original_kind==CNCE_PATCH_SVC&&sites[1].operand==3&&sites[1].payload==456);
    memcpy(before,sites,sizeof sites);requests[1].pc=base;
    CHECK(cnce_patch_collect(words,sizeof words,base,requests,2,sites,16,&count)==CNCE_PATCH_DUPLICATE&&!memcmp(sites,before,sizeof sites));
    requests[1].pc=base-4;
    CHECK(cnce_patch_collect(words,sizeof words,base,requests,2,sites,16,&count)==CNCE_PATCH_ARGUMENT);
    requests[1].pc=base+sizeof words;
    CHECK(cnce_patch_collect(words,sizeof words,base,requests,2,sites,16,&count)==CNCE_PATCH_ARGUMENT);
    requests[1].pc=base+4;requests[1].reserved=1;
    CHECK(cnce_patch_collect(words,sizeof words,base,requests,2,sites,16,&count)==CNCE_PATCH_ARGUMENT);
    requests[1].reserved=0;requests[1].kind=CNCE_PATCH_SVC;
    CHECK(cnce_patch_collect(words,sizeof words,base,requests,2,sites,16,&count)==CNCE_PATCH_ARGUMENT);
    words[9]=0xd51bd060;
    CHECK(cnce_patch_collect(words,sizeof words,base,NULL,0,sites,16,&count)==CNCE_PATCH_UNSUPPORTED&&!memcmp(sites,before,sizeof sites));
    CHECK(cnce_patch_collect(NULL,0,0,NULL,0,NULL,0,&count)==CNCE_PATCH_OK&&count==0);
    CHECK(cnce_patch_collect(NULL,4,base,NULL,0,sites,16,&count)==CNCE_PATCH_ARGUMENT);
    CHECK(cnce_patch_collect(words,sizeof words,base+1,NULL,0,sites,16,&count)==CNCE_PATCH_ARGUMENT);
    CHECK(cnce_patch_collect(words,3,base,NULL,0,sites,16,&count)==CNCE_PATCH_ARGUMENT);
    CHECK(cnce_patch_collect(words,8,UINT64_MAX-3,NULL,0,sites,16,&count)==CNCE_PATCH_RANGE);
    CHECK(cnce_patch_collect(words,4,base,NULL,0,sites,16,NULL)==CNCE_PATCH_ARGUMENT);
    unsigned char unaligned[5];memcpy(unaligned+1,words,4);
    CHECK(cnce_patch_collect(unaligned+1,4,base,NULL,0,sites,16,&count)==CNCE_PATCH_OK&&!count);
}
static CnceSite site_for(uint32_t word,uint64_t pc) {
    CnceSite site={.original_pc=pc,.resume_pc=pc+4,.original_word=word};
    CHECK(cnce_patch_classify(word,&site.kind,&site.operand)==1);site.original_kind=site.kind;return site;
}
static void emission(void) {
    const uint64_t pc=UINT64_C(0x20000000),stub_pc=pc+0x100000,descriptor=UINT64_C(0xabcdef000),gateway=UINT64_C(0x123456788);
    CnceSite site=site_for(0xd4002461,pc);unsigned char stub[40],before[40];uint32_t word;
    memset(stub,0x5a,sizeof stub);CHECK(cnce_patch_emit(&site,stub_pc,descriptor,gateway,&word,stub,sizeof stub)==CNCE_PATCH_OK);
    CHECK(branch_target(pc,word)==stub_pc);
    for(unsigned i=0;i<4;i++)CHECK(read32(stub+4*i)==exit_stub_words[i]);
    CHECK(read64(stub+16)==descriptor&&read64(stub+24)==gateway);
    for(unsigned i=32;i<40;i++)CHECK(stub[i]==0x5a);
    memcpy(before,stub,sizeof stub);word=0xa5a5a5a5;
    CHECK(cnce_patch_emit(&site,stub_pc,descriptor,gateway,&word,stub,31)==CNCE_PATCH_CAPACITY&&word==0xa5a5a5a5&&!memcmp(stub,before,sizeof stub));
    CHECK(cnce_patch_emit(&site,pc+0x8000000,descriptor,gateway,&word,stub,sizeof stub)==CNCE_PATCH_RANGE&&word==0xa5a5a5a5&&!memcmp(stub,before,sizeof stub));
    CHECK(cnce_patch_emit(&site,stub_pc+4,descriptor,gateway,&word,stub,sizeof stub)==CNCE_PATCH_ARGUMENT);
    CHECK(cnce_patch_emit(&site,stub_pc,descriptor+4,gateway,&word,stub,sizeof stub)==CNCE_PATCH_ARGUMENT);
    CHECK(cnce_patch_emit(&site,stub_pc,descriptor,gateway+1,&word,stub,sizeof stub)==CNCE_PATCH_ARGUMENT);
    site.original_kind=0;CHECK(cnce_patch_emit(&site,stub_pc,descriptor,gateway,&word,stub,sizeof stub)==CNCE_PATCH_ARGUMENT);
    site=site_for(0xc85f7c40,pc);
    CHECK(cnce_patch_emit(&site,0,0,0,&word,NULL,0)==CNCE_PATCH_OK&&word==0xc85ffc40);
    CHECK(!memcmp(stub,before,sizeof stub));
    site=site_for(0xd53bd05f,pc); /* XZR still needs a safe virtual TLS patch. */
    CHECK(cnce_patch_emit(&site,stub_pc,descriptor,gateway,&word,stub,sizeof stub)==CNCE_PATCH_OK);
    CHECK(cnce_resume_emit(stub_pc,pc,stub,sizeof stub)==CNCE_PATCH_OK);
    CHECK(read32(stub)==resume_first_word&&branch_target(stub_pc+4,read32(stub+4))==pc);
    memcpy(before,stub,sizeof stub);
    CHECK(cnce_resume_emit(stub_pc,pc,stub,7)==CNCE_PATCH_CAPACITY&&!memcmp(stub,before,sizeof stub));
    CHECK(cnce_resume_emit(stub_pc,stub_pc+4+0x8000000,stub,sizeof stub)==CNCE_PATCH_RANGE&&!memcmp(stub,before,sizeof stub));
    CHECK(cnce_resume_emit(stub_pc,stub_pc+4-0x8000000,stub,sizeof stub)==CNCE_PATCH_OK);
    CHECK(cnce_resume_emit(UINT64_MAX-3,pc,stub,sizeof stub)==CNCE_PATCH_RANGE);
}
int main(void) {
    classify();zero_release_preservation();casp_classification();branches();collection();emission();
    printf("{\"state\":\"passed\",\"checks\":%u,\"svc_immediates\":65536,\"gpr_variants\":32,\"random_branches\":20000,\"site_bytes\":%zu,\"ordinary_branches_unchanged\":true,\"casp_valid_encodings\":65536,\"casp_odd_pair_rejections\":196608}\n",checks,sizeof(CnceSite));
    return 0;
}
