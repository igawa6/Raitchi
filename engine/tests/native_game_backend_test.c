/* Synthetic instruction words only; no game code, mappings or execution. */
#include "block_plan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned assertions;
#define CHECK(x) do {++assertions;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static NgbDecoder decoder;
static NgbInstruction classify(uint32_t word,enum NgbExitKind kind) {
    NgbInstruction instruction;
    CHECK(ngb_classify(&decoder,&word,0x12341000,&instruction)==NGB_OK);
    CHECK(instruction.kind==kind);CHECK(instruction.word==word);CHECK(instruction.pc==0x12341000);
    return instruction;
}
static NgbBlockPlan blank(void) {return (NgbBlockPlan){.struct_size=sizeof(NgbBlockPlan),.version=NGB_PLAN_VERSION};}
int main(void) {
    CHECK(ngb_decoder_init(NULL)==NGB_ARGUMENT);
    CHECK(ngb_decoder_init(&decoder)==NGB_OK);
    for(unsigned rd=0;rd<32;rd++)for(unsigned rn=0;rn<32;rn++) {
        uint32_t word=0x8b010000u|(rn<<5)|rd; /* ADD Xd,Xn,X1 */
        int reserved=rd==18||rd==19||rn==18||rn==19;
        NgbInstruction instruction=classify(word,reserved?NGB_EXIT_RESERVED_REGISTER:NGB_EXIT_LIMIT);
        CHECK(!!(instruction.flags&NGB_VIRTUAL_REGISTER)==reserved);
        CHECK(instruction.read_gpr&(1u<<1));
        if(rn!=31)CHECK(instruction.read_gpr&(1u<<rn));
        if(rd!=31)CHECK(instruction.write_gpr&(1u<<rd));
    }
    for(unsigned rt=0;rt<32;rt++)for(unsigned rn=0;rn<32;rn++) {
        uint32_t word=0xf9400000u|(rn<<5)|rt; /* LDR Xt,[Xn] */
        int reserved=rt==18||rt==19||rn==18||rn==19;
        NgbInstruction instruction=classify(word,reserved?NGB_EXIT_RESERVED_REGISTER:NGB_EXIT_LIMIT);
        CHECK(!!(instruction.flags&NGB_MEMORY));
        if(rn==31)CHECK(instruction.flags&NGB_READ_SP);
    }
    NgbInstruction instruction=classify(0x1e332812u,NGB_EXIT_LIMIT); /* FADD S18,S0,S19 */
    CHECK(instruction.read_vector&(1u<<19));CHECK(instruction.write_vector&(1u<<18));
    CHECK(!(instruction.flags&NGB_VIRTUAL_REGISTER));
    instruction=classify(0xa9bf7bfdu,NGB_EXIT_LIMIT); /* STP X29,X30,[SP,#-16]! */
    CHECK((instruction.flags&(NGB_READ_SP|NGB_WRITE_SP|NGB_MEMORY))==(NGB_READ_SP|NGB_WRITE_SP|NGB_MEMORY));
    CHECK(instruction.read_gpr&(1u<<29));CHECK(instruction.read_gpr&(1u<<30));
    classify(0xa8c17bfdu,NGB_EXIT_LIMIT); /* LDP X29,X30,[SP],#16 */
    classify(0xd503201fu,NGB_EXIT_LIMIT); /* NOP */
    instruction=classify(0x14000002u,NGB_EXIT_BRANCH);CHECK(instruction.target==0x12341008);
    instruction=classify(0x94000002u,NGB_EXIT_CALL);CHECK(instruction.target==0x12341008);
    instruction=classify(0x54000040u,NGB_EXIT_BRANCH);CHECK(instruction.target==0x12341008);
    instruction=classify(0xd61f0240u,NGB_EXIT_BRANCH);CHECK(instruction.flags&NGB_VIRTUAL_REGISTER);
    classify(0xd65f03c0u,NGB_EXIT_RETURN);
    instruction=classify(0xd40004a1u,NGB_EXIT_SVC);CHECK(instruction.target==0x25);
    classify(0xd53bd040u,NGB_EXIT_TLS);classify(0xd53bd060u,NGB_EXIT_TLS);classify(0xd51bd040u,NGB_EXIT_TLS);
    instruction=classify(0xd53bd052u,NGB_EXIT_TLS);CHECK(instruction.flags&NGB_VIRTUAL_REGISTER);
    classify(0xd53be040u,NGB_EXIT_SYSTEM);classify(0xd51b4200u,NGB_EXIT_SYSTEM);
    instruction=classify(0x10000020u,NGB_EXIT_PC_RELATIVE);CHECK(instruction.target==0x12341004);
    instruction=classify(0x90000000u,NGB_EXIT_PC_RELATIVE);CHECK(instruction.target==0x12341000);
    instruction=classify(0x58000020u,NGB_EXIT_PC_RELATIVE);CHECK(instruction.target==0x12341004);
    instruction=classify(0xc85f7c20u,NGB_EXIT_ATOMIC);CHECK(instruction.flags&NGB_EXCLUSIVE_LOAD);
    instruction=classify(0xc85ffc20u,NGB_EXIT_ATOMIC);CHECK(instruction.flags&NGB_EXCLUSIVE_LOAD);
    instruction=classify(0xc8027c20u,NGB_EXIT_ATOMIC);CHECK(instruction.flags&NGB_EXCLUSIVE_STORE);
    instruction=classify(0xc802fc20u,NGB_EXIT_ATOMIC);CHECK(instruction.flags&NGB_EXCLUSIVE_STORE);
    instruction=classify(0xd5033f5fu,NGB_EXIT_ATOMIC);CHECK(instruction.flags&NGB_CLEAR_EXCLUSIVE);
    classify(0xc8dffc20u,NGB_EXIT_ATOMIC);classify(0xf8200022u,NGB_EXIT_ATOMIC);
    classify(0xd5033bbfu,NGB_EXIT_SYSTEM);
    classify(0x2518e3e0u,NGB_EXIT_EMULATE); /* PTRUE P0.B: unrepresented SVE state */
    const uint32_t body[]={0x8b010000u,0xd503201fu,0xd65f03c0u,0x8b010000u};
    NgbBlockPlan plan=blank();CHECK(ngb_plan_block(&decoder,body,sizeof body,0x1000,64,&plan)==NGB_OK);
    CHECK(plan.native_count==2&&plan.next_pc==0x1008&&plan.exit.kind==NGB_EXIT_RETURN);
    CHECK(!memcmp(plan.words,body,8));CHECK(plan.words[2]==0); /* exit is never copied */
    plan=blank();CHECK(ngb_plan_block(&decoder,body,sizeof body,0x1000,1,&plan)==NGB_OK);
    CHECK(plan.native_count==1&&plan.next_pc==0x1004&&plan.exit.kind==NGB_EXIT_LIMIT);
    plan=blank();CHECK(ngb_plan_block(&decoder,body,8,0x1000,64,&plan)==NGB_OK);
    CHECK(plan.native_count==2&&plan.next_pc==0x1008&&plan.exit.kind==NGB_EXIT_INPUT_END);
    const uint32_t reserved[]={0x8b010240u};plan=blank();
    CHECK(ngb_plan_block(&decoder,reserved,4,0x1000,64,&plan)==NGB_OK);
    CHECK(!plan.native_count&&plan.next_pc==0x1000&&plan.exit.kind==NGB_EXIT_RESERVED_REGISTER);
    const uint32_t invalid[]={0xd503201fu,0xffffffffu};plan=blank();
    CHECK(ngb_plan_block(&decoder,invalid,sizeof invalid,0x1000,64,&plan)==NGB_INVALID_INSTRUCTION);
    CHECK(plan.native_count==1&&plan.next_pc==0x1004&&plan.exit.kind==NGB_EXIT_INVALID);
    plan=blank();CHECK(ngb_plan_block(&decoder,body,3,0x1000,64,&plan)==NGB_ARGUMENT);
    CHECK(ngb_plan_block(&decoder,body,sizeof body,0x1001,64,&plan)==NGB_ARGUMENT);
    CHECK(ngb_plan_block(&decoder,body,sizeof body,UINT64_MAX-3,64,&plan)==NGB_ARGUMENT);
    CHECK(ngb_plan_block(&decoder,body,sizeof body,0x1000,65,&plan)==NGB_ARGUMENT);
    CHECK(ngb_plan_block(&decoder,body,sizeof body,0x1000,0,&plan)==NGB_ARGUMENT);
    plan.version=0;CHECK(ngb_plan_block(&decoder,body,sizeof body,0x1000,1,&plan)==NGB_ARGUMENT);
    NgbDecoder empty={0};plan=blank();CHECK(ngb_plan_block(&empty,body,sizeof body,0x1000,1,&plan)==NGB_ARGUMENT);
    /* Optional dynamic-context planning admits original X19 words only when
     * another GPR is provably unused by the complete emitted prefix. */
    for(unsigned rd=0;rd<32;rd++)for(unsigned rn=0;rn<32;rn++) {
        uint32_t word=0x8b010000u|(rn<<5)|rd;
        plan=blank();CHECK(ngb_plan_block_context(&decoder,&word,4,0x1000,64,&plan)==NGB_OK);
        if(rd==18||rn==18) {
            CHECK(!plan.native_count&&plan.exit.kind==NGB_EXIT_RESERVED_REGISTER&&plan.next_pc==0x1000);
        } else {
            CHECK(plan.native_count==1&&plan.words[0]==word&&plan.next_pc==0x1004);
            CHECK(plan.gpr_usage&(1u<<1));
            if(rn!=31)CHECK(plan.gpr_usage&(1u<<rn));
            if(rd!=31)CHECK(plan.gpr_usage&(1u<<rd));
            CHECK(NGB_CONTEXT_GPR_MASK&~plan.gpr_usage);
            CHECK(!(plan.gpr_usage&(1u<<31)));
        }
    }
    const struct {uint32_t word,usage;} uses19[]={
        {0xf8736820u,(1u<<0)|(1u<<1)|(1u<<19)}, /* LDR X0,[X1,X19] */
        {0xf8408660u,(1u<<0)|(1u<<19)}, /* LDR X0,[X19],#8 */
        {0xa9bf7bf3u,(1u<<19)|(1u<<30)}, /* STP X19,X30,[SP,#-16]! */
        {0x2a0003f3u,(1u<<0)|(1u<<19)}, /* MOV W19,W0 */
        {0xaa1303e0u,(1u<<0)|(1u<<19)}, /* MOV X0,X19 */
    };
    for(unsigned i=0;i<sizeof uses19/sizeof uses19[0];i++) {
        plan=blank();CHECK(ngb_plan_block(&decoder,&uses19[i].word,4,0x1000,64,&plan)==NGB_OK);
        CHECK(!plan.native_count&&plan.exit.kind==NGB_EXIT_RESERVED_REGISTER);
        plan=blank();CHECK(ngb_plan_block_context(&decoder,&uses19[i].word,4,0x1000,64,&plan)==NGB_OK);
        CHECK(plan.native_count==1&&plan.gpr_usage==uses19[i].usage);
        CHECK(NGB_CONTEXT_GPR_MASK&~plan.gpr_usage);
    }
    const struct {uint32_t word;enum NgbExitKind kind;} special19[]={
        {0xd53bd053u,NGB_EXIT_TLS}, /* MRS X19,TPIDR_EL0 */
        {0x10000033u,NGB_EXIT_PC_RELATIVE}, /* ADR X19,#4 */
        {0xd61f0260u,NGB_EXIT_BRANCH}, /* BR X19 */
        {0xc85f7e60u,NGB_EXIT_ATOMIC}, /* LDXR X0,[X19] */
        {0xf9800260u,NGB_EXIT_RESERVED_REGISTER}, /* PRFM [X19]: not admitted ordinary whitelist */
    };
    for(unsigned i=0;i<sizeof special19/sizeof special19[0];i++) {
        plan=blank();CHECK(ngb_plan_block_context(&decoder,&special19[i].word,4,0x1000,64,&plan)==NGB_OK);
        CHECK(!plan.native_count&&plan.exit.kind==special19[i].kind);
    }
    unsigned candidates[31],candidate_count=0;
    for(unsigned i=0;i<31;i++)if(NGB_CONTEXT_GPR_MASK&(1u<<i))candidates[candidate_count++]=i;
    CHECK(candidate_count==28);
    for(unsigned rotation=0;rotation<candidate_count;rotation++) {
        uint32_t crowded[32]={0xaa1303f3u}; /* MOV X19,X19 followed by every scratch candidate. */
        for(unsigned i=0;i<candidate_count;i++) {
            unsigned reg=candidates[(i+rotation)%candidate_count];
            crowded[i+1]=0x91000000u|(reg<<5)|reg; /* ADD Xr,Xr,#0 */
        }
        plan=blank();CHECK(ngb_plan_block_context(&decoder,crowded,(candidate_count+1)*4,0x1000,64,&plan)==NGB_OK);
        CHECK(plan.native_count==candidate_count&&plan.exit.kind==NGB_EXIT_LIMIT);
        CHECK(plan.next_pc==0x1000+candidate_count*4);
        unsigned unused=candidates[(rotation+candidate_count-1)%candidate_count];
        CHECK((NGB_CONTEXT_GPR_MASK&~plan.gpr_usage)==(1u<<unused));
        CHECK(!memcmp(plan.words,crowded,plan.native_count*4));CHECK(!plan.words[plan.native_count]);
        NgbBlockPlan continuation=blank();
        CHECK(ngb_plan_block_context(&decoder,crowded+plan.native_count,4,plan.next_pc,64,&continuation)==NGB_OK);
        CHECK(continuation.native_count==1&&continuation.next_pc==0x1000+(candidate_count+1)*4);
        CHECK(continuation.gpr_usage==(1u<<unused));
    }
    ngb_decoder_destroy(&decoder);ngb_decoder_destroy(&decoder);ngb_decoder_destroy(NULL);
    CHECK(!decoder.handle&&!decoder.instruction);
    printf("native block planner assertions=%u\n",assertions);return 0;
}
