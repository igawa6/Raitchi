/* Own implementation of generic AArch64 ISA encodings. No Eden source copied. */
#include "patch_plan.h"
#include <limits.h>
#include <string.h>

_Static_assert(sizeof(CnceSite)==40,"continuous site ABI");
_Static_assert(offsetof(CnceSite,kind)==20,"continuous site kind ABI");
_Static_assert(offsetof(CnceSite,payload)==32,"continuous site payload ABI");
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "continuous AArch64 patch encoding requires little endian"
#endif

static int preserve_zero_release(uint32_t word) {
    /* Size and base register vary; all other STLXR[B/H] fields are exact. */
    return (word&UINT32_C(0x3ffffc1f))==UINT32_C(0x081ffc1f);
}

int cnce_patch_classify(uint32_t word,uint32_t *kind,uint32_t *operand) {
    uint32_t k=CNCE_PATCH_NONE,o=0,system=word&UINT32_C(0xffffffe0);
    if((word&UINT32_C(0xffe0001f))==UINT32_C(0xd4000001)) {
        k=CNCE_PATCH_SVC;o=(word>>5)&65535;
    } else {
        switch(system) {
        case UINT32_C(0xd53bd040):k=CNCE_PATCH_TPIDR_READ;break;
        case UINT32_C(0xd53bd060):k=CNCE_PATCH_TPIDRRO_READ;break;
        case UINT32_C(0xd51bd040):k=CNCE_PATCH_TPIDR_WRITE;break;
        case UINT32_C(0xd53be020):case UINT32_C(0xd53be040):k=CNCE_PATCH_COUNTER_READ;break;
        case UINT32_C(0xd53be000):k=CNCE_PATCH_FREQUENCY_READ;break;
        /* Guest mode owns hardware FP and flags, restored at every boundary. */
        case UINT32_C(0xd53b4400):case UINT32_C(0xd53b4420):case UINT32_C(0xd53b4200):
        case UINT32_C(0xd51b4400):case UINT32_C(0xd51b4420):case UINT32_C(0xd51b4200):break;
        /* Cache maintenance needs the actual host cache-line geometry. */
        case UINT32_C(0xd53b0020):break; /* MRS CTR_EL0 */
        default:
            if((word&UINT32_C(0xfff00000))==UINT32_C(0xd5300000)||
               (word&UINT32_C(0xfff00000))==UINT32_C(0xd5100000))return -1;
            break;
        }
        if(k!=CNCE_PATCH_NONE)o=word&31;
    }
    /* Exclusive loads/stores retain their physical hardware monitor. Upgrading
     * only the ordering bit provides the stronger original A57 expectation. */
    if((word&UINT32_C(0x3f800000))==UINT32_C(0x08000000)) {
        unsigned pair=(word>>21)&1,load=(word>>22)&1,size=word>>30;
        unsigned rs=(word>>16)&31,rt2=(word>>10)&31,rt=word&31,rn=(word>>5)&31;
        if(pair&&size<2) {
            /* FEAT_LSE CASP: size0/1 selects 32/64-bit elements, bit22 is
             * acquire and bit15 release. These are NOT exclusive loads/stores.
             * Rs/Rt name even first registers; second registers are implicit.
             * Arm DDI0602 CASP decode: Rt2 fixed11111, Rs[0]=Rt[0]=0.
             * Keep the original atomic instruction and its ordering unchanged. */
            if(rt2!=31||((rs|rt)&1))return -1;
        } else {
            /* Preserve an already-release single store with discarded status
             * and zero data exactly as supplied. Identical Rs/Rt is still
             * architecturally constrained unpredictable, including register31;
             * this is not an operand-validity exemption. No ordering rewrite
             * is needed, so retain original hardware behavior rather than
             * inventing a store result. Other overlap cases remain rejected. */
            if(!preserve_zero_release(word)&&((!pair&&rt2!=31)||(load&&rs!=31)||
               (load&&pair&&rt==rt2&&rt!=31)||(!load&&(rs==rt||(pair&&rs==rt2)||(rn!=31&&rs==rn)))))return -1;
            if(!(word&(1u<<15)))k=CNCE_PATCH_ORDERED_EXCLUSIVE;
        }
    }
    if(kind)*kind=k;if(operand)*operand=o;
    return k!=CNCE_PATCH_NONE;
}

enum CncePatchStatus cnce_patch_collect(const void *text,size_t size,uint64_t base,
    const CncePatchRequest *requests,size_t request_count,CnceSite *sites,
    size_t capacity,size_t *required) {
    if(!required||(!text&&size)||(!requests&&request_count)||(!sites&&capacity)||
       (size&3)||(base&3))return CNCE_PATCH_ARGUMENT;
    if(size>UINT64_MAX-base)return CNCE_PATCH_RANGE;
    uint64_t end=base+size;
    for(size_t i=0;i<request_count;i++) {
        const CncePatchRequest *request=&requests[i];
        if(request->reserved||(request->pc&3)||request->pc<base||request->pc>=end||
           (request->kind!=CNCE_PATCH_HOST_FUNCTION&&request->kind!=CNCE_PATCH_INSTRUCTION_HOOK))return CNCE_PATCH_ARGUMENT;
        if(i&&request->pc<=requests[i-1].pc)return CNCE_PATCH_DUPLICATE;
    }
    size_t count=0,request=0;
    for(size_t offset=0;offset<size;offset+=4) {
        uint32_t word,kind;memcpy(&word,(const unsigned char*)text+offset,4);
        int classified=cnce_patch_classify(word,&kind,NULL);
        if(classified<0)return CNCE_PATCH_UNSUPPORTED;
        int explicit_site=request<request_count&&requests[request].pc==base+offset;
        if(classified||explicit_site)count++;
        if(explicit_site)request++;
    }
    *required=count;
    if(count>capacity)return CNCE_PATCH_CAPACITY;
    count=0;request=0;
    for(size_t offset=0;offset<size;offset+=4) {
        uint32_t word,kind,operand;memcpy(&word,(const unsigned char*)text+offset,4);
        (void)cnce_patch_classify(word,&kind,&operand);
        int explicit_site=request<request_count&&requests[request].pc==base+offset;
        if(kind||explicit_site) {
            CnceSite site={.original_pc=base+offset,.resume_pc=base+offset+4,
                .original_word=word,.kind=kind,.operand=operand,.original_kind=kind};
            if(explicit_site){site.kind=requests[request].kind;site.payload=requests[request].payload;}
            sites[count++]=site;
        }
        if(explicit_site)request++;
    }
    return CNCE_PATCH_OK;
}

enum CncePatchStatus cnce_encode_b26(uint64_t from,uint64_t to,uint32_t *word) {
    if(!word||(from&3)||(to&3))return CNCE_PATCH_ARGUMENT;
    uint64_t displacement;
    if(to>=from) {
        displacement=to-from;if(displacement>UINT64_C(0x7fffffc))return CNCE_PATCH_RANGE;
        displacement/=4;
    } else {
        displacement=from-to;if(displacement>UINT64_C(0x8000000))return CNCE_PATCH_RANGE;
        displacement=0-displacement/4;
    }
    *word=UINT32_C(0x14000000)|(uint32_t)(displacement&UINT32_C(0x03ffffff));
    return CNCE_PATCH_OK;
}

enum CncePatchStatus cnce_patch_emit(const CnceSite *site,uint64_t stub_pc,
    uint64_t descriptor_pc,uint64_t gateway_pc,uint32_t *replacement,
    void *stub,size_t stub_capacity) {
    if(!site||!replacement||(site->original_pc&3)||site->original_pc>UINT64_MAX-4||
       site->resume_pc!=site->original_pc+4||!site->kind||site->kind>CNCE_PATCH_INSTRUCTION_HOOK)return CNCE_PATCH_ARGUMENT;
    uint32_t original_kind,operand;
    if(cnce_patch_classify(site->original_word,&original_kind,&operand)<0||
       original_kind!=site->original_kind||operand!=site->operand)return CNCE_PATCH_ARGUMENT;
    if(site->kind!=CNCE_PATCH_HOST_FUNCTION&&site->kind!=CNCE_PATCH_INSTRUCTION_HOOK&&site->kind!=original_kind)return CNCE_PATCH_ARGUMENT;
    if(preserve_zero_release(site->original_word))return CNCE_PATCH_ARGUMENT;
    if(site->kind==CNCE_PATCH_ORDERED_EXCLUSIVE){*replacement=site->original_word|(1u<<15);return CNCE_PATCH_OK;}
    if(!stub||stub_capacity<CNCE_EXIT_STUB_SIZE)return CNCE_PATCH_CAPACITY;
    if((stub_pc&7)||!descriptor_pc||(descriptor_pc&7)||!gateway_pc||(gateway_pc&3))return CNCE_PATCH_ARGUMENT;
    if(stub_pc>UINT64_MAX-CNCE_EXIT_STUB_SIZE)return CNCE_PATCH_RANGE;
    uint32_t branch;enum CncePatchStatus status=cnce_encode_b26(site->original_pc,stub_pc,&branch);
    if(status!=CNCE_PATCH_OK)return status;
    /* STP x16,x17,[sp,#-16]!; LDR x16,[pc+12]; LDR x17,[pc+16]; BR x17. */
    unsigned char bytes[CNCE_EXIT_STUB_SIZE];
    const uint32_t code[]={UINT32_C(0xa9bf47f0),UINT32_C(0x58000070),UINT32_C(0x58000091),UINT32_C(0xd61f0220)};
    memcpy(bytes,code,sizeof code);memcpy(bytes+16,&descriptor_pc,8);memcpy(bytes+24,&gateway_pc,8);
    memcpy(stub,bytes,sizeof bytes);*replacement=branch;return CNCE_PATCH_OK;
}
enum CncePatchStatus cnce_resume_emit(uint64_t stub_pc,uint64_t target_pc,void *stub,size_t capacity) {
    if(!stub||capacity<CNCE_RESUME_STUB_SIZE)return CNCE_PATCH_CAPACITY;
    if((stub_pc&3)||(target_pc&3))return CNCE_PATCH_ARGUMENT;
    if(stub_pc>UINT64_MAX-CNCE_RESUME_STUB_SIZE)return CNCE_PATCH_RANGE;
    uint32_t code[]={UINT32_C(0xa8c147f0),0};
    enum CncePatchStatus status=cnce_encode_b26(stub_pc+4,target_pc,&code[1]);
    if(status!=CNCE_PATCH_OK)return status;
    memcpy(stub,code,sizeof code);return CNCE_PATCH_OK;
}
const char *cnce_patch_status_string(enum CncePatchStatus status) {
    switch(status) {
    case CNCE_PATCH_OK:return "ok";case CNCE_PATCH_ARGUMENT:return "invalid argument";
    case CNCE_PATCH_CAPACITY:return "insufficient output capacity";case CNCE_PATCH_RANGE:return "address or branch range";
    case CNCE_PATCH_DUPLICATE:return "requests not strictly ordered";case CNCE_PATCH_UNSUPPORTED:return "unsupported system instruction";
    default:return "unknown patch status";
    }
}
