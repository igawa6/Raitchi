#include "block_plan.h"
#include <capstone/capstone.h>
#include <capstone/arm64.h>
#include <limits.h>
#include <string.h>

_Static_assert(sizeof(csh)==sizeof(uintptr_t), "Capstone handle ABI");

enum NgbStatus ngb_decoder_init(NgbDecoder *decoder) {
    if(!decoder) return NGB_ARGUMENT;
    memset(decoder,0,sizeof *decoder);
    int major=0,minor=0;cs_version(&major,&minor);
    if(major!=5 || minor!=0 || cs_support(CS_SUPPORT_DIET) || !cs_support(CS_ARCH_ARM64)) return NGB_DECODER_ERROR;
    csh handle=0;
    if(cs_open(CS_ARCH_ARM64,CS_MODE_ARM,&handle)!=CS_ERR_OK) return NGB_DECODER_ERROR;
    if(cs_option(handle,CS_OPT_DETAIL,CS_OPT_ON)!=CS_ERR_OK || !(decoder->instruction=cs_malloc(handle))) {
        cs_close(&handle);return NGB_DECODER_ERROR;
    }
    decoder->handle=(uintptr_t)handle;
    return NGB_OK;
}
void ngb_decoder_destroy(NgbDecoder *decoder) {
    if(!decoder) return;
    if(decoder->instruction) cs_free(decoder->instruction,1);
    if(decoder->handle) {csh handle=(csh)decoder->handle;cs_close(&handle);}
    memset(decoder,0,sizeof *decoder);
}

/* Operand metadata is used rather than treating SIMD V18/V19 as host GPRs.
 * SVE/SME, predicate and unrepresented registers never qualify for native. */
static int register_masks(unsigned reg,int write,NgbInstruction *out) {
    unsigned index;
    if(reg==ARM64_REG_INVALID || reg==ARM64_REG_XZR || reg==ARM64_REG_WZR) return 1;
    if(reg==ARM64_REG_SP || reg==ARM64_REG_WSP) {out->flags|=write?NGB_WRITE_SP:NGB_READ_SP;return 1;}
    if(reg==ARM64_REG_NZCV) {out->flags|=write?NGB_WRITE_NZCV:NGB_READ_NZCV;return 1;}
    if(reg==ARM64_REG_FP) index=29;
    else if(reg==ARM64_REG_LR) index=30;
    else if(reg>=ARM64_REG_X0 && reg<=ARM64_REG_X28) index=reg-ARM64_REG_X0;
    else if(reg>=ARM64_REG_W0 && reg<=ARM64_REG_W30) index=reg-ARM64_REG_W0;
    else {
        static const unsigned starts[]={ARM64_REG_B0,ARM64_REG_D0,ARM64_REG_H0,ARM64_REG_Q0,ARM64_REG_S0,ARM64_REG_V0};
        for(unsigned i=0;i<sizeof starts/sizeof starts[0];i++) if(reg>=starts[i] && reg<starts[i]+32) {
            index=reg-starts[i];
            if(write)out->write_vector|=UINT32_C(1)<<index;else out->read_vector|=UINT32_C(1)<<index;
            return 1;
        }
        return 0;
    }
    if(write)out->write_gpr|=UINT32_C(1)<<index;else out->read_gpr|=UINT32_C(1)<<index;
    if(index==18 || index==19) out->flags|=NGB_VIRTUAL_REGISTER;
    return 1;
}

static int safe_instruction(unsigned id) {
    switch(id) {
#define SAFE(name) case ARM64_INS_##name:
    SAFE(ADC) SAFE(ADCS) SAFE(ADD) SAFE(ADDS) SAFE(AND) SAFE(ANDS) SAFE(ASR) SAFE(ASRV)
    SAFE(BFI) SAFE(BFM) SAFE(BFXIL) SAFE(BIC) SAFE(BICS) SAFE(CCMN) SAFE(CCMP)
    SAFE(CINC) SAFE(CINV) SAFE(CLS) SAFE(CLZ) SAFE(CMN) SAFE(CMP) SAFE(CNEG)
    SAFE(CSEL) SAFE(CSET) SAFE(CSETM) SAFE(CSINC) SAFE(CSINV) SAFE(CSNEG)
    SAFE(EON) SAFE(EOR) SAFE(EXTR) SAFE(LSL) SAFE(LSLV) SAFE(LSR) SAFE(LSRV)
    SAFE(MADD) SAFE(MNEG) SAFE(MOV) SAFE(MOVI) SAFE(MOVK) SAFE(MOVN) SAFE(MOVZ)
    SAFE(MSUB) SAFE(MUL) SAFE(MVN) SAFE(NEG) SAFE(NEGS) SAFE(NGC) SAFE(NGCS) SAFE(NOP)
    SAFE(ORN) SAFE(ORR) SAFE(RBIT) SAFE(REV) SAFE(REV16) SAFE(REV32) SAFE(REV64)
    SAFE(ROR) SAFE(RORV) SAFE(SBC) SAFE(SBCS) SAFE(SBFIZ) SAFE(SBFM) SAFE(SBFX)
    SAFE(SDIV) SAFE(SMADDL) SAFE(SMNEGL) SAFE(SMSUBL) SAFE(SMULH) SAFE(SMULL)
    SAFE(SUB) SAFE(SUBS) SAFE(SXTB) SAFE(SXTH) SAFE(SXTW) SAFE(TST)
    SAFE(UBFIZ) SAFE(UBFM) SAFE(UBFX) SAFE(UDIV) SAFE(UMADDL) SAFE(UMNEGL)
    SAFE(UMSUBL) SAFE(UMULH) SAFE(UMULL) SAFE(UXTB) SAFE(UXTH) SAFE(UXTW)
    SAFE(LDP) SAFE(LDPSW) SAFE(LDNP) SAFE(LDR) SAFE(LDRB) SAFE(LDRH) SAFE(LDRSB)
    SAFE(LDRSH) SAFE(LDRSW) SAFE(LDUR) SAFE(LDURB) SAFE(LDURH) SAFE(LDURSB)
    SAFE(LDURSH) SAFE(LDURSW) SAFE(STP) SAFE(STNP) SAFE(STR) SAFE(STRB) SAFE(STRH)
    SAFE(STUR) SAFE(STURB) SAFE(STURH)
    SAFE(ABS) SAFE(ADDP) SAFE(ADDV) SAFE(BSL) SAFE(BIT) SAFE(BIF) SAFE(CNT)
    SAFE(DUP) SAFE(EXT) SAFE(FABS) SAFE(FADD) SAFE(FCMP) SAFE(FCMPE) SAFE(FCSEL)
    SAFE(FCVT) SAFE(FCVTAS) SAFE(FCVTAU) SAFE(FCVTMS) SAFE(FCVTMU) SAFE(FCVTNS)
    SAFE(FCVTNU) SAFE(FCVTPS) SAFE(FCVTPU) SAFE(FCVTZS) SAFE(FCVTZU)
    SAFE(FDIV) SAFE(FMADD) SAFE(FMAX) SAFE(FMAXNM) SAFE(FMIN) SAFE(FMINNM)
    SAFE(FMOV) SAFE(FMSUB) SAFE(FMUL) SAFE(FNEG) SAFE(FNMADD) SAFE(FNMSUB)
    SAFE(FNMUL) SAFE(FSQRT) SAFE(FSUB) SAFE(INS) SAFE(NOT)
    SAFE(SCVTF) SAFE(UCVTF) SAFE(SMOV) SAFE(UMOV) SAFE(TBL) SAFE(TBX)
    SAFE(TRN1) SAFE(TRN2) SAFE(UZP1) SAFE(UZP2) SAFE(ZIP1) SAFE(ZIP2)
    SAFE(SSHL) SAFE(USHL) SAFE(SSHR) SAFE(USHR) SAFE(SHL) SAFE(SLI) SAFE(SRI)
    SAFE(SMIN) SAFE(SMAX) SAFE(UMIN) SAFE(UMAX) SAFE(SADDLV) SAFE(UADDLV)
    SAFE(LD1) SAFE(LD2) SAFE(LD3) SAFE(LD4) SAFE(ST1) SAFE(ST2) SAFE(ST3) SAFE(ST4)
        return 1;
#undef SAFE
    default:return 0;
    }
}
static int is_exclusive_load(unsigned id) {
    return id==ARM64_INS_LDXR || id==ARM64_INS_LDXRB || id==ARM64_INS_LDXRH || id==ARM64_INS_LDXP ||
           id==ARM64_INS_LDAXR || id==ARM64_INS_LDAXRB || id==ARM64_INS_LDAXRH || id==ARM64_INS_LDAXP;
}
static int is_exclusive_store(unsigned id) {
    return id==ARM64_INS_STXR || id==ARM64_INS_STXRB || id==ARM64_INS_STXRH || id==ARM64_INS_STXP ||
           id==ARM64_INS_STLXR || id==ARM64_INS_STLXRB || id==ARM64_INS_STLXRH || id==ARM64_INS_STLXP;
}

enum NgbStatus ngb_classify(NgbDecoder *decoder,const void *word,uint64_t pc,NgbInstruction *out) {
    if(!decoder || !decoder->handle || !decoder->instruction || !word || !out || (pc&3) || pc>UINT64_MAX-4) return NGB_ARGUMENT;
    memset(out,0,sizeof *out);out->pc=pc;memcpy(&out->word,word,4);
    out->kind=NGB_EXIT_INVALID;
    const uint8_t *data=word;size_t size=4;uint64_t address=pc;
    cs_insn *instruction=decoder->instruction;
    if(!cs_disasm_iter((csh)decoder->handle,&data,&size,&address,instruction) || instruction->size!=4 || !instruction->detail) return NGB_INVALID_INSTRUCTION;
    out->id=instruction->id;
    cs_regs read,write;uint8_t nr=0,nw=0;
    if(cs_regs_access((csh)decoder->handle,instruction,read,&nr,write,&nw)!=CS_ERR_OK) return NGB_DECODER_ERROR;
    int represented=1;
    for(unsigned i=0;i<nr;i++) represented&=register_masks(read[i],0,out);
    for(unsigned i=0;i<nw;i++) represented&=register_masks(write[i],1,out);
    cs_arm64 *arm=&instruction->detail->arm64;
    for(unsigned i=0;i<arm->op_count;i++) {
        const cs_arm64_op *op=&arm->operands[i];
        if(op->type==ARM64_OP_MEM) {
            out->flags|=NGB_MEMORY;
            represented&=register_masks(op->mem.base,0,out);
            represented&=register_masks(op->mem.index,0,out);
            if(arm->writeback) represented&=register_masks(op->mem.base,1,out);
        } else if(op->type==ARM64_OP_REG) {
            if(op->access&CS_AC_READ) represented&=register_masks(op->reg,0,out);
            if(op->access&CS_AC_WRITE) represented&=register_masks(op->reg,1,out);
            /* Even absent access metadata, never pass through an unknown or
             * reserved architectural register in an explicit operand. */
            NgbInstruction check={0};
            represented&=register_masks(op->reg,0,&check);
            out->flags|=check.flags&NGB_VIRTUAL_REGISTER;
            if(!(op->access&(CS_AC_READ|CS_AC_WRITE))) {
                /* Conservatively include operands for which Capstone omits
                 * access direction. An executor may only borrow an actually
                 * unused register, never a register hidden by absent flags. */
                out->read_gpr|=check.read_gpr;
                out->read_vector|=check.read_vector;
            }
        }
    }
    unsigned id=instruction->id;
    if(is_exclusive_load(id)) out->flags|=NGB_EXCLUSIVE_LOAD;
    if(is_exclusive_store(id)) out->flags|=NGB_EXCLUSIVE_STORE;
    if(id==ARM64_INS_CLREX) out->flags|=NGB_CLEAR_EXCLUSIVE;
    if(id==ARM64_INS_B || id==ARM64_INS_BL || id==ARM64_INS_BR || id==ARM64_INS_BLR ||
       id==ARM64_INS_CBZ || id==ARM64_INS_CBNZ || id==ARM64_INS_TBZ || id==ARM64_INS_TBNZ || id==ARM64_INS_RET) {
        out->kind=id==ARM64_INS_RET?NGB_EXIT_RETURN:(id==ARM64_INS_BL || id==ARM64_INS_BLR)?NGB_EXIT_CALL:NGB_EXIT_BRANCH;
        for(unsigned i=0;i<arm->op_count;i++) if(arm->operands[i].type==ARM64_OP_IMM) out->target=(uint64_t)arm->operands[i].imm;
    } else if(id==ARM64_INS_SVC) {out->kind=NGB_EXIT_SVC;out->target=(out->word>>5)&0xffffu;}
    else if(id==ARM64_INS_MRS || id==ARM64_INS_MSR) {
        uint32_t system=out->word&0xffffffe0u;
        out->kind=system==0xd53bd040u || system==0xd53bd060u || system==0xd51bd040u?NGB_EXIT_TLS:NGB_EXIT_SYSTEM;
    } else if(id==ARM64_INS_ADR || id==ARM64_INS_ADRP || (out->word&0x3b000000u)==0x18000000u) {
        out->kind=NGB_EXIT_PC_RELATIVE;
        for(unsigned i=0;i<arm->op_count;i++) if(arm->operands[i].type==ARM64_OP_IMM) out->target=(uint64_t)arm->operands[i].imm;
    } else if((out->flags&(NGB_EXCLUSIVE_LOAD|NGB_EXCLUSIVE_STORE|NGB_CLEAR_EXCLUSIVE)) ||
              (out->word&0x3f000000u)==0x08000000u || (out->word&0x3b200c00u)==0x38200000u) out->kind=NGB_EXIT_ATOMIC;
    else if(id==ARM64_INS_DMB || id==ARM64_INS_DSB || id==ARM64_INS_ISB || id==ARM64_INS_BRK || id==ARM64_INS_HLT) out->kind=NGB_EXIT_SYSTEM;
    else if(!represented) out->kind=NGB_EXIT_EMULATE;
    else if(out->flags&NGB_VIRTUAL_REGISTER) out->kind=NGB_EXIT_RESERVED_REGISTER;
    else out->kind=safe_instruction(id)?NGB_EXIT_LIMIT:NGB_EXIT_EMULATE;
    return NGB_OK;
}

static enum NgbStatus plan_block(NgbDecoder *decoder,const void *code,size_t size,uint64_t pc,unsigned limit,NgbBlockPlan *plan,int dynamic_context) {
    if(!decoder || !decoder->handle || !decoder->instruction || !code || !plan || plan->struct_size!=sizeof *plan || plan->version!=NGB_PLAN_VERSION ||
       !limit || limit>NGB_MAX_NATIVE_WORDS || size<4 || (pc&3) || pc>UINT64_MAX-size || (size&3)) return NGB_ARGUMENT;
    memset(plan,0,sizeof *plan);plan->struct_size=sizeof *plan;plan->version=NGB_PLAN_VERSION;
    plan->start_pc=pc;plan->next_pc=pc;
    for(unsigned i=0;i<limit && (size_t)i*4<size;i++) {
        NgbInstruction instruction={0};
        enum NgbStatus result=ngb_classify(decoder,(const uint8_t *)code+i*4,pc+i*4,&instruction);
        plan->exit=instruction;plan->next_pc=pc+i*4;
        if(result!=NGB_OK) return result;
        uint32_t usage=instruction.read_gpr|instruction.write_gpr;
        int ordinary=instruction.kind==NGB_EXIT_LIMIT;
        if(dynamic_context && instruction.kind==NGB_EXIT_RESERVED_REGISTER &&
           !(usage&(UINT32_C(1)<<18)) && (usage&(UINT32_C(1)<<19)) && safe_instruction(instruction.id)) ordinary=1;
        if(!ordinary) return NGB_OK;
        if(dynamic_context && !(NGB_CONTEXT_GPR_MASK&~(plan->gpr_usage|usage))) {
            /* Execute the already admitted prefix, then replan this original
             * instruction with a fresh free register. No word was rewritten. */
            plan->exit.kind=NGB_EXIT_LIMIT;
            return NGB_OK;
        }
        plan->gpr_usage|=usage;
        plan->words[plan->native_count++]=instruction.word;
        plan->next_pc+=4;
    }
    memset(&plan->exit,0,sizeof plan->exit);plan->exit.pc=plan->next_pc;
    plan->exit.kind=plan->native_count==limit?NGB_EXIT_LIMIT:NGB_EXIT_INPUT_END;
    return NGB_OK;
}
enum NgbStatus ngb_plan_block(NgbDecoder *decoder,const void *code,size_t size,uint64_t pc,unsigned limit,NgbBlockPlan *plan) {
    return plan_block(decoder,code,size,pc,limit,plan,0);
}
enum NgbStatus ngb_plan_block_context(NgbDecoder *decoder,const void *code,size_t size,uint64_t pc,unsigned limit,NgbBlockPlan *plan) {
    return plan_block(decoder,code,size,pc,limit,plan,1);
}
const char *ngb_status_string(enum NgbStatus status) {
    static const char *const names[]={"ok","argument","decoder-error","invalid-instruction"};
    return (unsigned)status<sizeof names/sizeof names[0]?names[status]:"unknown";
}
const char *ngb_exit_string(enum NgbExitKind kind) {
    static const char *const names[]={"limit","input-end","branch","call","return","svc","tls","pc-relative","reserved-register","atomic","system","emulate","invalid"};
    return (unsigned)kind<sizeof names/sizeof names[0]?names[kind]:"unknown";
}
