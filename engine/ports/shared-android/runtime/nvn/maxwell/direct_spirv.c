/* Direct Maxwell-to-SPIR-V emitter for a proved straight-line graphics subset.
 * Encoding and binding contract follows translate.c (MIT; Copyright2026 Peiikai,
 * raulzito12345rrs and EoWRecomp contributors). No GLSL or shaderc is used here.
 * Whole-program validation precedes emission; unsupported programs fall back.
 */
#include "direct_spirv.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DS_MAX_WORDS (2u * 1024u * 1024u)
#define DS_MAX_INSTRUCTIONS 65536u
#define DS_CONSTANT_SLOTS 131072u
#define DS_RZ 255u
#define DS_INST(c, section, op, ...) ds_inst((c), &(c)->section, (op), (uint32_t[]){__VA_ARGS__}, sizeof((uint32_t[]){__VA_ARGS__}) / sizeof(uint32_t))
enum {
    DS_ExtInstImport=11, DS_ExtInst=12, DS_MemoryModel=14, DS_EntryPoint=15, DS_ExecutionMode=16,
    DS_Capability=17, DS_TypeVoid=19, DS_TypeBool=20, DS_TypeInt=21, DS_TypeFloat=22,
    DS_TypeVector=23, DS_TypeArray=28, DS_TypeStruct=30, DS_TypePointer=32, DS_TypeFunction=33,
    DS_Constant=43, DS_Function=54, DS_FunctionEnd=56, DS_Variable=59, DS_Load=61, DS_Store=62,
    DS_AccessChain=65, DS_Decorate=71, DS_MemberDecorate=72, DS_CompositeConstruct=80,
    DS_CompositeExtract=81, DS_Bitcast=124, DS_FNegate=127, DS_FAdd=129, DS_FMul=133,
    DS_FDiv=136, DS_IsNan=156, DS_Select=169, DS_IEqual=170, DS_BitwiseXor=198, DS_BitwiseAnd=199,
    DS_Label=248, DS_Return=253
};
enum { DS_VOID, DS_BOOL, DS_UINT, DS_INT, DS_FLOAT, DS_VEC4, DS_UVEC4, DS_IVEC4, DS_NTYPES };
enum { DI_SKIP, DI_EXIT, DI_MOV, DI_MOV32, DI_LDC, DI_LDA, DI_STA, DI_IPA, DI_FADD, DI_FADD32,
       DI_FMUL, DI_FMUL32, DI_FFMA, DI_FFMA32 };
typedef struct { uint32_t *v; size_t n, cap; } DsWords;
typedef struct { uint32_t bits, id; } DsConstant;
typedef struct {
    MaxwellSpirvOutput *out;
    MaxwellSpirvStatus status;
    const ShaderKey *key;
    int stage;
    uint32_t next, ext, fn, ty[DS_NTYPES], fn_ty;
    uint32_t ptr_in[DS_NTYPES], ptr_out[DS_NTYPES], ptr_uniform_uint;
    uint32_t in[32], varying[32], color[8], cb[18], rscale;
    uint32_t position, pointsize, fragcoord, frontface, vertexid, instanceid, depth;
    uint32_t reg[256], pos_value[4], varying_value[32][4], point_value;
    uint32_t flat_mask, omap;
    bool depth_output, need_fragcoord, need_frontface, need_vertexid, need_instanceid;
    uint8_t reg_used[256];
    DsConstant *constants;
    uint32_t interfaces[80]; size_t ninterfaces;
    DsWords declarations, annotations, body;
} DsContext;
static uint64_t ds_bits(uint64_t w,unsigned start,unsigned count) { return (w>>start)&((UINT64_C(1)<<count)-1); }
static uint64_t ds_read64(const uint8_t *p) { uint64_t w; memcpy(&w,p,sizeof w); return w; }
static uint32_t ds_read32(const uint8_t *p) { uint32_t w; memcpy(&w,p,sizeof w); return w; }
static bool ds_fail(DsContext *c, MaxwellSpirvStatus status, const char *reason) {
    if (c->status == MAXWELL_SPIRV_OK) { c->status=status; snprintf(c->out->reason,sizeof c->out->reason,"%s",reason); }
    return false;
}
static bool ds_reserve(DsContext *c, DsWords *v, size_t count) {
    if(c->status!=MAXWELL_SPIRV_OK)return false;
    if(count>DS_MAX_WORDS-v->n)return ds_fail(c,MAXWELL_SPIRV_UNSUPPORTED,"SPIR-V module word limit");
    if(v->n+count<=v->cap)return true;
    size_t cap=v->cap?v->cap:256; while(cap<v->n+count)cap*=2;
    if(cap>DS_MAX_WORDS)cap=DS_MAX_WORDS;
    void *p=realloc(v->v,cap*sizeof(uint32_t));if(!p)return ds_fail(c,MAXWELL_SPIRV_OOM,"SPIR-V allocation failed");
    v->v=p;v->cap=cap;return true;
}
static void ds_inst(DsContext*c,DsWords*v,unsigned opcode,const uint32_t *args,size_t n) {
    if(n>=65535||!ds_reserve(c,v,n+1))return;
    v->v[v->n++]=(uint32_t)((n+1)<<16)|opcode;
    if(n)memcpy(v->v+v->n,args,n*sizeof(uint32_t));v->n+=n;
}
static uint32_t ds_id(DsContext*c) {return c->next++;}
static uint32_t ds_result(DsContext*c,unsigned op,uint32_t type,const uint32_t*a,size_t n) {
    uint32_t args[10],id=ds_id(c); if(n>8){ds_fail(c,MAXWELL_SPIRV_INVALID,"internal operand limit");return 0;}
    args[0]=type;args[1]=id;if(n)memcpy(args+2,a,n*4);ds_inst(c,&c->body,op,args,n+2);return id;
}
#define DS_RES(c,op,type,...) ds_result((c),(op),(type),(uint32_t[]){__VA_ARGS__},sizeof((uint32_t[]){__VA_ARGS__})/sizeof(uint32_t))
static uint32_t ds_uint(DsContext*c,uint32_t bits) {
    size_t slot=(size_t)(bits*UINT32_C(2654435761))&(DS_CONSTANT_SLOTS-1);
    while(c->constants[slot].id&&c->constants[slot].bits!=bits)slot=(slot+1)&(DS_CONSTANT_SLOTS-1);
    if(!c->constants[slot].id){uint32_t id=ds_id(c);c->constants[slot]=(DsConstant){bits,id};DS_INST(c,declarations,DS_Constant,c->ty[DS_UINT],id,bits);}
    return c->constants[slot].id;
}
static uint32_t ds_float(DsContext*c,uint32_t bits) {uint32_t id=ds_id(c);DS_INST(c,declarations,DS_Constant,c->ty[DS_FLOAT],id,bits);return id;}
static uint32_t ds_bitcast(DsContext*c,uint32_t type,uint32_t value){return DS_RES(c,DS_Bitcast,type,value);}
static uint32_t ds_f(DsContext*c,uint32_t value){return ds_bitcast(c,c->ty[DS_FLOAT],value);}
static uint32_t ds_u(DsContext*c,uint32_t value){return ds_bitcast(c,c->ty[DS_UINT],value);}
static uint32_t ds_binary(DsContext*c,unsigned opcode,uint32_t a,uint32_t b){
    uint32_t id=DS_RES(c,opcode,c->ty[DS_FLOAT],a,b);
    if(c->stage==STAGE_VERTEX)DS_INST(c,annotations,DS_Decorate,id,42); // NoContraction
    return id;
}
static uint32_t ds_ext(DsContext*c,unsigned instruction,uint32_t a,uint32_t b,uint32_t d,unsigned count){
    uint32_t args[5]={c->ext,instruction,a,b,d};uint32_t id=ds_result(c,DS_ExtInst,c->ty[DS_FLOAT],args,count+2);
    if(instruction==50&&c->stage==STAGE_VERTEX)DS_INST(c,annotations,DS_Decorate,id,42);
    return id;
}
static uint32_t ds_negate(DsContext*c,uint32_t v){
    uint32_t id=DS_RES(c,DS_FNegate,c->ty[DS_FLOAT],v);
    if(c->stage==STAGE_VERTEX)DS_INST(c,annotations,DS_Decorate,id,42);
    return id;
}
static uint32_t ds_mod(DsContext*c,uint32_t v,bool neg,bool abs_){if(abs_)v=ds_ext(c,4,v,0,0,1);if(neg)v=ds_negate(c,v);return v;}
static uint32_t ds_saturate(DsContext*c,uint32_t v){
    uint32_t zero=ds_float(c,0),one=ds_float(c,0x3f800000),clamp=ds_ext(c,43,v,zero,one,3);
    uint32_t nan=DS_RES(c,DS_IsNan,c->ty[DS_BOOL],v);return DS_RES(c,DS_Select,c->ty[DS_FLOAT],nan,zero,clamp);
}
/* Maxwell FTZ flushes both inputs and results to sign-preserving zero.
 * Express it in integer bits so this does not depend on optional Vulkan
 * float-control execution modes or the host's default denormal behavior. */
static uint32_t ds_flush(DsContext*c,uint32_t v,bool enabled){
    if(!enabled)return v;
    uint32_t bits=ds_u(c,v);
    uint32_t exponent=DS_RES(c,DS_BitwiseAnd,c->ty[DS_UINT],bits,ds_uint(c,0x7f800000));
    uint32_t tiny=DS_RES(c,DS_IEqual,c->ty[DS_BOOL],exponent,ds_uint(c,0));
    uint32_t sign=DS_RES(c,DS_BitwiseAnd,c->ty[DS_UINT],bits,ds_uint(c,0x80000000));
    return ds_f(c,DS_RES(c,DS_Select,c->ty[DS_UINT],tiny,sign,bits));
}
static uint32_t ds_float_result(DsContext*c,uint32_t v,bool ftz,bool sat){
    v=ds_flush(c,v,ftz);return sat?ds_saturate(c,v):v;
}
static uint32_t ds_multiply(DsContext*c,uint32_t a,uint32_t b,bool ftz,bool sat){
    uint32_t v=ds_flush(c,ds_binary(c,DS_FMul,a,b),ftz);
    if(ftz){
        // Some drivers fold a zero constant-buffer product to +0 despite
        // signed operands. Maxwell requires the product's XOR sign, including
        // when an underflowed result was just flushed. Nonzero/NaN/Inf bits
        // remain untouched; saturation is applied after sign restoration.
        uint32_t bits=ds_u(c,v);
        uint32_t magnitude=DS_RES(c,DS_BitwiseAnd,c->ty[DS_UINT],bits,ds_uint(c,0x7fffffff));
        uint32_t zero=DS_RES(c,DS_IEqual,c->ty[DS_BOOL],magnitude,ds_uint(c,0));
        uint32_t xor_sign=DS_RES(c,DS_BitwiseXor,c->ty[DS_UINT],ds_u(c,a),ds_u(c,b));
        uint32_t sign=DS_RES(c,DS_BitwiseAnd,c->ty[DS_UINT],xor_sign,ds_uint(c,0x80000000));
        v=ds_f(c,DS_RES(c,DS_Select,c->ty[DS_UINT],zero,sign,bits));
    }
    return sat?ds_saturate(c,v):v;
}
static uint32_t ds_reg(DsContext*c,unsigned r){if(r>=256){ds_fail(c,MAXWELL_SPIRV_UNSUPPORTED,"register group exceeds RZ");return ds_uint(c,0);}if(r==DS_RZ)return ds_uint(c,0);c->reg_used[r]=1;return c->reg[r];}
static void ds_set(DsContext*c,unsigned r,uint32_t value){if(r>=256){ds_fail(c,MAXWELL_SPIRV_UNSUPPORTED,"register group exceeds RZ");return;}if(r!=DS_RZ){c->reg_used[r]=1;c->reg[r]=value;}}
static bool ds_cbuf_metadata(DsContext*c,unsigned slot,uint32_t off){
    if(slot>=18||off>0xfff0||(off&3))return ds_fail(c,MAXWELL_SPIRV_UNSUPPORTED,"constant buffer span unsupported");
    ShaderInfo*i=&c->out->info;i->cbuf_mask|=1u<<slot;
    uint32_t size=(off+16+255)&~255u;if(size>i->cbuf_size[slot])i->cbuf_size[slot]=size;
    if(!slot){if(off>=0x110&&off<0x210)i->ssbo_read|=(uint16_t)(1u<<((off-0x110)>>4));else if(off>=0x510&&off<0x610)i->ssbo_read|=(uint16_t)(1u<<((off-0x510)>>4));}
    return true;
}
static uint32_t ds_cbuf(DsContext*c,unsigned slot,uint32_t off){
    uint32_t ptr=DS_RES(c,DS_AccessChain,c->ptr_uniform_uint,c->cb[slot],ds_uint(c,0),ds_uint(c,off>>4),ds_uint(c,(off>>2)&3));
    return DS_RES(c,DS_Load,c->ty[DS_UINT],ptr);
}
static bool ds_b_metadata(DsContext*c,uint64_t w){return (w>>60)!=4||ds_cbuf_metadata(c,(unsigned)ds_bits(w,34,5),(uint32_t)ds_bits(w,20,14)*4);}
static uint32_t ds_b(DsContext*c,uint64_t w,bool float_imm){
    if((w>>60)==5)return ds_reg(c,(unsigned)ds_bits(w,20,8));
    if((w>>60)==4)return ds_cbuf(c,(unsigned)ds_bits(w,34,5),(uint32_t)ds_bits(w,20,14)*4);
    uint32_t bits=(uint32_t)ds_bits(w,20,19)|((uint32_t)ds_bits(w,56,1)<<19);
    if(float_imm)bits<<=12;else if(bits&0x80000)bits|=0xfff00000;
    return ds_uint(c,bits);
}
static bool ds_attr_metadata(DsContext*c,uint32_t addr,bool store){
    if(addr&3)return ds_fail(c,MAXWELL_SPIRV_UNSUPPORTED,"unaligned attribute address");
    if(store){
        if(c->stage!=STAGE_VERTEX)return ds_fail(c,MAXWELL_SPIRV_UNSUPPORTED,"non-vertex attribute store");
        if(addr>=0x70&&addr<0x80)return true;
        if(addr>=0x80&&addr<0x280){c->out->info.out_mask|=1u<<((addr-0x80)>>4);return true;}
        if(addr==0x6c){c->out->info.writes_point_size=true;return true;}
        return ds_fail(c,MAXWELL_SPIRV_UNSUPPORTED,"unsupported vertex output attribute");
    }
    if(addr>=0x80&&addr<0x280){
        unsigned at=(addr-0x80)>>4;
        if(c->stage==STAGE_VERTEX||(c->key->vs_out_mask&(1u<<at)))c->out->info.attr_mask|=1u<<at;
        return true;
    }
    if(c->stage==STAGE_VERTEX){
        if(addr==0x2fc){c->need_vertexid=true;return true;}
        if(addr==0x2f8){c->need_instanceid=true;return true;}
    }else{
        if(addr>=0x70&&addr<0x80){c->need_fragcoord=true;if(addr<0x78)c->out->info.uses_rscale=true;return true;}
        if(addr==0x3fc){c->need_frontface=true;return true;}
    }
    return ds_fail(c,MAXWELL_SPIRV_UNSUPPORTED,"unsupported shader input attribute");
}
static bool ds_match(uint64_t w,uint64_t value,uint64_t mask){return(w&mask)==value;}
static unsigned ds_decode(DsContext*c,uint64_t w){
    if(ds_bits(w,16,4)==15)return DI_SKIP; // @!PT never executes, including unknown instructions
    if(ds_match(w,0x50b0000000000000ull,0xfff8000000000000ull))return DI_SKIP;
    if(ds_bits(w,16,4)!=7){ds_fail(c,MAXWELL_SPIRV_UNSUPPORTED,"predicated instruction requires legacy backend");return DI_SKIP;}
    if(ds_match(w,0xe300000000000000ull,0xfff0000000000000ull))return DI_EXIT;
    if(ds_match(w,0x5c98000000000000ull,0xfff8000000000000ull)||ds_match(w,0x4c98000000000000ull,0xfff8000000000000ull)||ds_match(w,0x3898000000000000ull,0xfef8000000000000ull))return DI_MOV;
    if(ds_match(w,0x0100000000000000ull,0xfff0000000000000ull))return DI_MOV32;
    if(ds_match(w,0xef90000000000000ull,0xfff8000000000000ull))return DI_LDC;
    if(ds_match(w,0xefd8000000000000ull,0xfff8000000000000ull))return DI_LDA;
    if(ds_match(w,0xeff0000000000000ull,0xfff8000000000000ull))return DI_STA;
    if(ds_match(w,0xe00000000000ff00ull,0xff0000400000ff00ull)||ds_match(w,0xe000004000000000ull,0xff00004000000000ull))return DI_IPA;
    if(ds_match(w,0x5c58000000000000ull,0xfff8000000000000ull)||ds_match(w,0x4c58000000000000ull,0xfff8000000000000ull)||ds_match(w,0x3858000000000000ull,0xfef8000000000000ull))return DI_FADD;
    if(ds_match(w,0x0800000000000000ull,0xfc00000000000000ull))return DI_FADD32;
    if(ds_match(w,0x5c68000000000000ull,0xfff8000000000000ull)||ds_match(w,0x4c68000000000000ull,0xfff8000000000000ull)||ds_match(w,0x3868000000000000ull,0xfef8000000000000ull))return DI_FMUL;
    if(ds_match(w,0x1e00000000000000ull,0xff00000000000000ull))return DI_FMUL32;
    if(ds_match(w,0x5980000000000000ull,0xff80000000000000ull)||ds_match(w,0x4980000000000000ull,0xff80000000000000ull)||ds_match(w,0x5180000000000000ull,0xff80000000000000ull)||ds_match(w,0x3280000000000000ull,0xfe80000000000000ull))return DI_FFMA;
    if(ds_match(w,0x0c00000000000000ull,0xfc00000000000000ull))return DI_FFMA32;
    ds_fail(c,MAXWELL_SPIRV_UNSUPPORTED,"instruction not admitted by direct backend");return DI_SKIP;
}
static bool ds_validate_instruction(DsContext*c,uint64_t w,unsigned op){
    unsigned d=(unsigned)ds_bits(w,0,8),a=(unsigned)ds_bits(w,8,8);
    if(op==DI_EXIT&&((w&31)!=15||ds_bits(w,5,1)))
        return ds_fail(c,MAXWELL_SPIRV_UNSUPPORTED,"conditional or KEEP EXIT requires legacy backend");
    if(op==DI_MOV&&ds_bits(w,39,4)!=15&&ds_bits(w,39,4)!=1)
        return ds_fail(c,MAXWELL_SPIRV_UNSUPPORTED,"masked MOV requires legacy backend");
    if(op==DI_MOV32&&ds_bits(w,12,4)!=15&&ds_bits(w,12,4)!=1)
        return ds_fail(c,MAXWELL_SPIRV_UNSUPPORTED,"masked MOV32I requires legacy backend");
    // CC writes are dead in this whole-program subset: no admitted operation
    // consumes CC, predicates or conditional EXIT state. Adding such a reader
    // requires implementing flags before extending this admission proof.
    if((op==DI_FADD&&ds_bits(w,39,2))||
       (op==DI_FMUL&&(ds_bits(w,39,2)||ds_bits(w,41,3)||ds_bits(w,44,2)>1))||
       (op==DI_FMUL32&&ds_bits(w,53,2)>1)||
       (op==DI_FFMA&&(ds_bits(w,51,2)||ds_bits(w,53,2)>1))||
       (op==DI_FFMA32&&ds_bits(w,53,2)>1))
        return ds_fail(c,MAXWELL_SPIRV_UNSUPPORTED,"rounding, scale or FMZ modifier unsupported");
    if(op==DI_MOV||op==DI_FADD||op==DI_FMUL)return ds_b_metadata(c,w);
    if(op==DI_FFMA){
        if(((w>>56)&0xf8)==0x50&&((w>>56)&7))return ds_cbuf_metadata(c,(unsigned)ds_bits(w,34,5),(uint32_t)ds_bits(w,20,14)*4);
        return ds_b_metadata(c,w);
    }
    if(op==DI_LDC){
        unsigned size=(unsigned)ds_bits(w,48,3),n=size==5?2:1,slot=(unsigned)ds_bits(w,36,5);uint32_t off=(uint32_t)ds_bits(w,20,16);
        if(a!=DS_RZ||ds_bits(w,44,2)||(off&0x8000)||off&3||(size!=4&&size!=5)||d+n>256||(size==5&&(d&1)))
            return ds_fail(c,MAXWELL_SPIRV_UNSUPPORTED,"LDC requires aligned static 32/64-bit address");
        for(unsigned k=0;k<n;k++)if(!ds_cbuf_metadata(c,slot,off+4*k))return false;
    }
    if(op==DI_LDA||op==DI_STA){
        unsigned n=1+(unsigned)ds_bits(w,47,2);uint32_t off=(uint32_t)ds_bits(w,20,10);
        if(a!=DS_RZ||ds_bits(w,31,1)||ds_bits(w,39,8)!=DS_RZ||d+n>256)
            return ds_fail(c,MAXWELL_SPIRV_UNSUPPORTED,"indexed, patch or invalid attribute register group");
        for(unsigned k=0;k<n;k++)if(!ds_attr_metadata(c,off+4*k,op==DI_STA))return false;
    }
    if(op==DI_IPA){
        uint32_t off=(uint32_t)ds_bits(w,28,10);unsigned mode=(unsigned)ds_bits(w,54,2);
        if(c->stage!=STAGE_FRAGMENT||ds_bits(w,38,1)||ds_bits(w,52,2))
            return ds_fail(c,MAXWELL_SPIRV_UNSUPPORTED,"indexed, centroid/offset or non-fragment IPA");
        if(off==0x3fc&&ds_bits(w,51,1))return ds_fail(c,MAXWELL_SPIRV_UNSUPPORTED,"saturated front-facing interpolation unsupported");
        if(off>=0x80&&off<0x280){if(mode==2)c->flat_mask|=1u<<((off-0x80)>>4);if(mode<=1)c->need_fragcoord=true;}
        return ds_attr_metadata(c,off,false);
    }
    return c->status==MAXWELL_SPIRV_OK;
}
static uint32_t ds_pointer(DsContext*c,unsigned storage,unsigned type){uint32_t id=ds_id(c);DS_INST(c,declarations,DS_TypePointer,id,storage,c->ty[type]);return id;}
static uint32_t ds_variable(DsContext*c,unsigned storage,unsigned type,int location,int builtin){
    uint32_t *ptr=storage==1?c->ptr_in:c->ptr_out;if(!ptr[type])ptr[type]=ds_pointer(c,storage,type);
    uint32_t id=ds_id(c);DS_INST(c,declarations,DS_Variable,ptr[type],id,storage);
    if(location>=0)DS_INST(c,annotations,DS_Decorate,id,30,(uint32_t)location);
    if(builtin>=0)DS_INST(c,annotations,DS_Decorate,id,11,(uint32_t)builtin);
    if(c->ninterfaces>=sizeof c->interfaces/sizeof*c->interfaces){ds_fail(c,MAXWELL_SPIRV_INVALID,"shader interface limit");return 0;}
    c->interfaces[c->ninterfaces++]=id;return id;
}
static uint32_t ds_uniform(DsContext*c,unsigned binding,unsigned rows,bool floating){
    uint32_t array=ds_id(c),structure=ds_id(c),pointer=ds_id(c),var=ds_id(c);
    DS_INST(c,declarations,DS_TypeArray,array,c->ty[floating?DS_VEC4:DS_UVEC4],ds_uint(c,rows));
    DS_INST(c,annotations,DS_Decorate,array,6,16); // ArrayStride
    DS_INST(c,declarations,DS_TypeStruct,structure,array);DS_INST(c,annotations,DS_Decorate,structure,2);
    DS_INST(c,annotations,DS_MemberDecorate,structure,0,35,0);
    DS_INST(c,declarations,DS_TypePointer,pointer,2,structure);DS_INST(c,declarations,DS_Variable,pointer,var,2);
    DS_INST(c,annotations,DS_Decorate,var,34,c->stage==STAGE_FRAGMENT?1:0);DS_INST(c,annotations,DS_Decorate,var,33,binding);return var;
}
static void ds_declarations(DsContext*c){
    c->ty[DS_VOID]=ds_id(c);DS_INST(c,declarations,DS_TypeVoid,c->ty[DS_VOID]);c->ty[DS_BOOL]=ds_id(c);DS_INST(c,declarations,DS_TypeBool,c->ty[DS_BOOL]);
    c->ty[DS_UINT]=ds_id(c);DS_INST(c,declarations,DS_TypeInt,c->ty[DS_UINT],32,0);c->ty[DS_INT]=ds_id(c);DS_INST(c,declarations,DS_TypeInt,c->ty[DS_INT],32,1);
    c->ty[DS_FLOAT]=ds_id(c);DS_INST(c,declarations,DS_TypeFloat,c->ty[DS_FLOAT],32);
    for(unsigned k=DS_VEC4;k<DS_NTYPES;k++){c->ty[k]=ds_id(c);DS_INST(c,declarations,DS_TypeVector,c->ty[k],c->ty[k==DS_VEC4?DS_FLOAT:k==DS_UVEC4?DS_UINT:DS_INT],4);}
    c->fn_ty=ds_id(c);DS_INST(c,declarations,DS_TypeFunction,c->fn_ty,c->ty[DS_VOID]);c->fn=ds_id(c);
    c->ptr_uniform_uint=ds_id(c);DS_INST(c,declarations,DS_TypePointer,c->ptr_uniform_uint,2,c->ty[DS_UINT]);
    for(unsigned k=0;k<18;k++)if(c->out->info.cbuf_mask&(1u<<k))c->cb[k]=ds_uniform(c,k,c->out->info.cbuf_size[k]/16,false);
    if(c->out->info.uses_rscale)c->rscale=ds_uniform(c,18,c->key->flip_x?66:65,true);
    for(unsigned k=0;k<32;k++){
        if(c->out->info.attr_mask&(1u<<k)){
            unsigned type=c->stage==STAGE_VERTEX?(c->key->attr_kind[k]==1?DS_IVEC4:c->key->attr_kind[k]==2?DS_UVEC4:DS_VEC4):DS_VEC4;
            c->in[k]=ds_variable(c,1,type,(int)k,-1);
            if(c->stage==STAGE_FRAGMENT&&(c->flat_mask&(1u<<k)))DS_INST(c,annotations,DS_Decorate,c->in[k],14);
        }
        if(c->stage==STAGE_VERTEX&&(c->out->info.out_mask&(1u<<k)))c->varying[k]=ds_variable(c,3,DS_VEC4,(int)k,-1);
    }
    if(c->stage==STAGE_VERTEX){
        c->position=ds_variable(c,3,DS_VEC4,-1,0);DS_INST(c,annotations,DS_Decorate,c->position,18); // Invariant
        if(c->out->info.writes_point_size)c->pointsize=ds_variable(c,3,DS_FLOAT,-1,1);
        if(c->need_vertexid)c->vertexid=ds_variable(c,1,DS_INT,-1,42);
        if(c->need_instanceid)c->instanceid=ds_variable(c,1,DS_INT,-1,43);
    }else{
        for(unsigned k=0;k<8;k++)if((c->omap>>(4*k))&15)c->color[k]=ds_variable(c,3,c->key->rt_kind[k]==1?DS_IVEC4:c->key->rt_kind[k]==2?DS_UVEC4:DS_VEC4,(int)k,-1);
        if(c->need_fragcoord||c->depth_output)c->fragcoord=ds_variable(c,1,DS_VEC4,-1,15);
        if(c->need_frontface)c->frontface=ds_variable(c,1,DS_BOOL,-1,17);
        if(c->depth_output)c->depth=ds_variable(c,3,DS_FLOAT,-1,22);
    }
}
static uint32_t ds_component(DsContext*c,uint32_t var,unsigned vector_type,unsigned scalar_type,unsigned component){uint32_t v=DS_RES(c,DS_Load,c->ty[vector_type],var);return DS_RES(c,DS_CompositeExtract,c->ty[scalar_type],v,component);}
static uint32_t ds_scale(DsContext*c,unsigned row,unsigned component){
    uint32_t ptrty=ds_id(c);DS_INST(c,declarations,DS_TypePointer,ptrty,2,c->ty[DS_FLOAT]);
    uint32_t ptr=DS_RES(c,DS_AccessChain,ptrty,c->rscale,ds_uint(c,0),ds_uint(c,row),ds_uint(c,component));return DS_RES(c,DS_Load,c->ty[DS_FLOAT],ptr);
}
static uint32_t ds_attr(DsContext*c,uint32_t addr,int ipa_mode,unsigned ipa_mul){
    unsigned component=(addr>>2)&3;
    if(addr>=0x80&&addr<0x280){
        unsigned at=(addr-0x80)>>4;if(!c->in[at])return ds_uint(c,component==3?0x3f800000:0);
        unsigned kind=c->stage==STAGE_VERTEX?c->key->attr_kind[at]:0;
        uint32_t v=ds_component(c,c->in[at],kind==1?DS_IVEC4:kind==2?DS_UVEC4:DS_VEC4,kind==1?DS_INT:kind==2?DS_UINT:DS_FLOAT,component);
        if(c->stage==STAGE_FRAGMENT&&(ipa_mode==0||(ipa_mode==1&&ipa_mul!=DS_RZ))){
            uint32_t w=ds_component(c,c->fragcoord,DS_VEC4,DS_FLOAT,3);v=ds_binary(c,DS_FMul,v,w);
            if(ipa_mode==1)v=ds_binary(c,DS_FMul,v,ds_f(c,ds_reg(c,ipa_mul)));
        }
        return kind==2?v:ds_u(c,v);
    }
    if(c->stage==STAGE_VERTEX){uint32_t v=DS_RES(c,DS_Load,c->ty[DS_INT],addr==0x2fc?c->vertexid:c->instanceid);return ds_u(c,v);}
    if(addr==0x3fc){uint32_t v=DS_RES(c,DS_Load,c->ty[DS_BOOL],c->frontface);return DS_RES(c,DS_Select,c->ty[DS_UINT],v,ds_uint(c,0xffffffff),ds_uint(c,0));}
    uint32_t v=ds_component(c,c->fragcoord,DS_VEC4,DS_FLOAT,component);
    if(component<2){v=ds_binary(c,131,v,ds_scale(c,0,component+2));v=ds_binary(c,DS_FDiv,v,ds_scale(c,0,component));if(component==0&&c->key->flip_x)v=ds_binary(c,131,ds_scale(c,65,0),v);}
    return ds_u(c,v);
}
static void ds_store_attr(DsContext*c,uint32_t addr,uint32_t value){
    unsigned component=(addr>>2)&3;uint32_t f=ds_f(c,value);
    if(addr>=0x70&&addr<0x80)c->pos_value[component]=f;
    else if(addr>=0x80&&addr<0x280)c->varying_value[(addr-0x80)>>4][component]=f;
    else if(addr==0x6c)c->point_value=f;
}
static void ds_emit_instruction(DsContext*c,uint64_t w,unsigned op){
    unsigned d=(unsigned)ds_bits(w,0,8),a=(unsigned)ds_bits(w,8,8);uint32_t v=0;
    switch(op){
    case DI_MOV:ds_set(c,d,ds_b(c,w,false));break;
    case DI_MOV32:ds_set(c,d,ds_uint(c,(uint32_t)ds_bits(w,20,32)));break;
    case DI_LDC:{unsigned n=ds_bits(w,48,3)==5?2:1,slot=(unsigned)ds_bits(w,36,5);uint32_t off=(uint32_t)ds_bits(w,20,16);for(unsigned k=0;k<n;k++)ds_set(c,d+k,ds_cbuf(c,slot,off+4*k));break;}
    case DI_LDA:case DI_STA:{unsigned n=1+(unsigned)ds_bits(w,47,2);uint32_t off=(uint32_t)ds_bits(w,20,10);for(unsigned k=0;k<n;k++){if(op==DI_LDA)ds_set(c,d+k,ds_attr(c,off+4*k,-1,DS_RZ));else ds_store_attr(c,off+4*k,ds_reg(c,d+k));}break;}
    case DI_IPA:v=ds_f(c,ds_attr(c,(uint32_t)ds_bits(w,28,10),(int)ds_bits(w,54,2),(unsigned)ds_bits(w,20,8)));if(ds_bits(w,51,1))v=ds_saturate(c,v);ds_set(c,d,ds_u(c,v));break;
    case DI_FADD: {
        bool ftz=ds_bits(w,44,1);
        uint32_t av=ds_flush(c,ds_mod(c,ds_f(c,ds_reg(c,a)),ds_bits(w,48,1),ds_bits(w,46,1)),ftz);
        uint32_t bv=ds_flush(c,ds_mod(c,ds_f(c,ds_b(c,w,true)),ds_bits(w,45,1),ds_bits(w,49,1)),ftz);
        v=ds_float_result(c,ds_binary(c,DS_FAdd,av,bv),ftz,ds_bits(w,50,1));ds_set(c,d,ds_u(c,v));break;
    }
    case DI_FADD32: {
        bool ftz=ds_bits(w,55,1);
        uint32_t av=ds_flush(c,ds_mod(c,ds_f(c,ds_reg(c,a)),ds_bits(w,56,1),ds_bits(w,54,1)),ftz);
        uint32_t bv=ds_flush(c,ds_mod(c,ds_f(c,ds_uint(c,(uint32_t)ds_bits(w,20,32))),ds_bits(w,53,1),ds_bits(w,57,1)),ftz);
        v=ds_float_result(c,ds_binary(c,DS_FAdd,av,bv),ftz,false);ds_set(c,d,ds_u(c,v));break;
    }
    case DI_FMUL: {
        bool ftz=ds_bits(w,44,2)==1;
        uint32_t av=ds_flush(c,ds_f(c,ds_reg(c,a)),ftz);
        uint32_t bv=ds_flush(c,ds_mod(c,ds_f(c,ds_b(c,w,true)),ds_bits(w,48,1),false),ftz);
        v=ds_multiply(c,av,bv,ftz,ds_bits(w,50,1));ds_set(c,d,ds_u(c,v));break;
    }
    case DI_FMUL32: {
        bool ftz=ds_bits(w,53,2)==1;
        uint32_t av=ds_flush(c,ds_f(c,ds_reg(c,a)),ftz);
        uint32_t bv=ds_flush(c,ds_f(c,ds_uint(c,(uint32_t)ds_bits(w,20,32))),ftz);
        v=ds_multiply(c,av,bv,ftz,ds_bits(w,55,1));ds_set(c,d,ds_u(c,v));break;
    }
    case DI_FFMA: {
        bool ftz=ds_bits(w,53,2)==1;uint32_t bv,cv;
        if(((w>>56)&0xf8)==0x50&&((w>>56)&7)){bv=ds_reg(c,(unsigned)ds_bits(w,39,8));cv=ds_cbuf(c,(unsigned)ds_bits(w,34,5),(uint32_t)ds_bits(w,20,14)*4);}
        else {bv=ds_b(c,w,true);cv=ds_reg(c,(unsigned)ds_bits(w,39,8));}
        uint32_t av=ds_flush(c,ds_f(c,ds_reg(c,a)),ftz);
        bv=ds_flush(c,ds_mod(c,ds_f(c,bv),ds_bits(w,48,1),false),ftz);
        cv=ds_flush(c,ds_mod(c,ds_f(c,cv),ds_bits(w,49,1),false),ftz);
        v=ds_float_result(c,ds_ext(c,50,av,bv,cv,3),ftz,ds_bits(w,50,1));ds_set(c,d,ds_u(c,v));break;
    }
    case DI_FFMA32: {
        bool ftz=ds_bits(w,53,2)==1;
        uint32_t av=ds_flush(c,ds_mod(c,ds_f(c,ds_reg(c,a)),ds_bits(w,56,1),false),ftz);
        uint32_t bv=ds_flush(c,ds_f(c,ds_uint(c,(uint32_t)ds_bits(w,20,32))),ftz);
        uint32_t cv=ds_flush(c,ds_mod(c,ds_f(c,ds_reg(c,d)),ds_bits(w,57,1),false),ftz);
        v=ds_float_result(c,ds_ext(c,50,av,bv,cv,3),ftz,ds_bits(w,55,1));ds_set(c,d,ds_u(c,v));break;
    }
    default:break;
    }
}
static void ds_store_vector(DsContext*c,uint32_t var,unsigned type,const uint32_t values[4]){uint32_t v=ds_result(c,DS_CompositeConstruct,c->ty[type],values,4);DS_INST(c,body,DS_Store,var,v);}
static void ds_epilogue(DsContext*c){
    if(c->stage==STAGE_VERTEX){
        if(c->key->flip_y)c->pos_value[1]=ds_negate(c,c->pos_value[1]);
        if(c->key->flip_x)c->pos_value[0]=ds_negate(c,c->pos_value[0]);
        if(c->key->depth_minus_one)c->pos_value[2]=ds_binary(c,DS_FMul,ds_binary(c,DS_FAdd,c->pos_value[2],c->pos_value[3]),ds_float(c,0x3f000000));
        ds_store_vector(c,c->position,DS_VEC4,c->pos_value);
        for(unsigned k=0;k<32;k++)if(c->varying[k])ds_store_vector(c,c->varying[k],DS_VEC4,c->varying_value[k]);
        if(c->pointsize)DS_INST(c,body,DS_Store,c->pointsize,c->point_value);
    }else{
        unsigned r=0;
        for(unsigned rt=0;rt<8;rt++){
            unsigned mask=(c->omap>>(4*rt))&15;if(!mask)continue;uint32_t values[4];
            unsigned kind=c->key->rt_kind[rt],type=kind==1?DS_IVEC4:kind==2?DS_UVEC4:DS_VEC4;
            for(unsigned k=0;k<4;k++){
                uint32_t v=ds_uint(c,0);if(mask&(1u<<k)){v=c->reg[r];r++;}
                values[k]=kind==2?v:ds_bitcast(c,c->ty[kind==1?DS_INT:DS_FLOAT],v);
            }
            ds_store_vector(c,c->color[rt],type,values);
        }
        if(c->depth){uint32_t v=c->reg_used[r+1]?ds_f(c,ds_reg(c,r+1)):ds_component(c,c->fragcoord,DS_VEC4,DS_FLOAT,2);DS_INST(c,body,DS_Store,c->depth,v);}
    }
}
void maxwell_direct_spirv_free(MaxwellSpirvOutput *out){if(out){free(out->words);memset(out,0,sizeof*out);}}
MaxwellSpirvStatus maxwell_direct_spirv(const uint8_t*blob,size_t bytes,size_t constants,int stage,const ShaderKey*key,MaxwellSpirvOutput*out){
    if(!out)return MAXWELL_SPIRV_INVALID;memset(out,0,sizeof*out);out->info.stage=stage;
    DsContext c={.out=out,.key=key,.stage=stage,.next=1};
    if(stage!=STAGE_VERTEX&&stage!=STAGE_FRAGMENT){ds_fail(&c,MAXWELL_SPIRV_UNSUPPORTED,"direct backend currently admits graphics stages only");goto done;}
    if(!blob||!key||!key->glsl||bytes<0x90||(bytes&7)||bytes>16u*1024u*1024u||constants>16u*1024u*1024u-bytes){ds_fail(&c,MAXWELL_SPIRV_INVALID,"invalid or unbounded graphics shader span");goto done;}
    for(unsigned k=0;k<32;k++)if(key->attr_kind[k]>2){ds_fail(&c,MAXWELL_SPIRV_INVALID,"invalid vertex input kind");goto done;}
    for(unsigned k=0;k<8;k++)if(key->rt_kind[k]>2){ds_fail(&c,MAXWELL_SPIRV_INVALID,"invalid render target kind");goto done;}
    if(stage==STAGE_FRAGMENT){c.omap=ds_read32(blob+0x78);c.depth_output=(ds_read32(blob+0x7c)&2)!=0;out->info.out_mask=c.omap;out->info.writes_depth=c.depth_output;
        if(ds_read32(blob+0x7c)&1){ds_fail(&c,MAXWELL_SPIRV_UNSUPPORTED,"sample-mask fragment output unsupported");goto done;}
        if((c.omap&7)==7&&key->rt_kind[0]==0){ds_fail(&c,MAXWELL_SPIRV_UNSUPPORTED,"dynamic tint/recolour requires legacy fragment backend");goto done;}}
    size_t count=(bytes-0x80)/8;if(count>DS_MAX_INSTRUCTIONS){ds_fail(&c,MAXWELL_SPIRV_UNSUPPORTED,"shader instruction budget");goto done;}
    uint8_t *ops=calloc(count,1);if(!ops){ds_fail(&c,MAXWELL_SPIRV_OOM,"instruction metadata allocation failed");goto done;}
    bool exited=false;size_t end=0;
    for(size_t k=0;k<count;k++){
        if(k%4==0)continue;out->instruction_offset=0x80+k*8;uint64_t w=ds_read64(blob+0x80+k*8);unsigned op=ds_decode(&c,w);ops[k]=(uint8_t)op;
        if(c.status!=MAXWELL_SPIRV_OK||!ds_validate_instruction(&c,w,op))break;out->info.num_insts++;
        if(op==DI_EXIT){exited=true;end=k+1;break;}
    }
    if(c.status!=MAXWELL_SPIRV_OK){free(ops);goto done;}
    if(!exited){free(ops);ds_fail(&c,MAXWELL_SPIRV_UNSUPPORTED,"no unconditional EXIT in bounded shader span");goto done;}
    c.constants=calloc(DS_CONSTANT_SLOTS,sizeof*c.constants);if(!c.constants){free(ops);ds_fail(&c,MAXWELL_SPIRV_OOM,"constant interning allocation failed");goto done;}
    out->info.uses_vertex_id=c.need_vertexid;
    c.ext=ds_id(&c);ds_declarations(&c);
    DS_INST(&c,body,DS_Function,c.ty[DS_VOID],c.fn,0,c.fn_ty);DS_INST(&c,body,DS_Label,ds_id(&c));
    uint32_t zero=ds_uint(&c,0),fzero=ds_float(&c,0),fone=ds_float(&c,0x3f800000);
    for(unsigned k=0;k<256;k++)c.reg[k]=zero;
    for(unsigned k=0;k<4;k++){c.pos_value[k]=k==3?fone:fzero;for(unsigned j=0;j<32;j++)c.varying_value[j][k]=k==3?fone:fzero;}
    c.point_value=fzero;
    for(size_t k=0;k<end;k++){if(k%4==0)continue;out->instruction_offset=0x80+k*8;ds_emit_instruction(&c,ds_read64(blob+0x80+k*8),ops[k]);if(c.status!=MAXWELL_SPIRV_OK)break;}
    free(ops);if(c.status!=MAXWELL_SPIRV_OK)goto done;ds_epilogue(&c);ds_inst(&c,&c.body,DS_Return,NULL,0);ds_inst(&c,&c.body,DS_FunctionEnd,NULL,0);
    DsWords module={0};uint32_t header[5]={0x07230203,0x00010000,0,c.next,0};
    if(!ds_reserve(&c,&module,5))goto module_done;memcpy(module.v,header,sizeof header);module.n=5;
    ds_inst(&c,&module,DS_Capability,(uint32_t[]){1},1);
    uint32_t import[5]={c.ext,0,0,0,0};memcpy(import+1,"GLSL.std.450",13);ds_inst(&c,&module,DS_ExtInstImport,import,5);
    ds_inst(&c,&module,DS_MemoryModel,(uint32_t[]){0,1},2);
    uint32_t entry[85]={stage==STAGE_VERTEX?0u:4u,c.fn,0x6e69616d,0};memcpy(entry+4,c.interfaces,c.ninterfaces*4);ds_inst(&c,&module,DS_EntryPoint,entry,c.ninterfaces+4);
    if(stage==STAGE_FRAGMENT){ds_inst(&c,&module,DS_ExecutionMode,(uint32_t[]){c.fn,7},2);if(c.depth_output)ds_inst(&c,&module,DS_ExecutionMode,(uint32_t[]){c.fn,12},2);}
    DsWords *parts[3]={&c.annotations,&c.declarations,&c.body};
    for(unsigned k=0;k<3;k++){
        if(!ds_reserve(&c,&module,parts[k]->n))break;
        if(parts[k]->n)memcpy(module.v+module.n,parts[k]->v,parts[k]->n*4);
        module.n+=parts[k]->n;
    }
    if(c.status==MAXWELL_SPIRV_OK){out->words=module.v;out->word_count=module.n;module.v=NULL;out->info.flow_kind=SHADER_FLOW_STRAIGHT;for(unsigned k=0;k<255;k++)out->info.register_count+=c.reg_used[k]!=0;}
module_done:free(module.v);
done:
    free(c.constants);free(c.annotations.v);free(c.declarations.v);free(c.body.v);
    if(c.status!=MAXWELL_SPIRV_OK){free(out->words);out->words=NULL;out->word_count=0;snprintf(out->info.error,sizeof out->info.error,"direct: %s",out->reason);}
    return c.status;
}
