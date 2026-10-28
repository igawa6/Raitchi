/* Actual orchestration C with synthetic exits/mocked gateway; no guest assets. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "runtime/aot_hooks.h"
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
#ifdef CNCE_REGISTRY_ENUM_TEST
static void dummy(Cpu *cpu){cpu->x[0]++;}
static void second(Cpu *cpu){cpu->x[0]+=2;}
static int hook(Cpu *cpu,uint64_t pc){(void)cpu;(void)pc;return 1;}
static int hook2(Cpu *cpu,uint64_t pc){(void)cpu;(void)pc;return 0;}
void rt_svc(Cpu *cpu,uint32_t imm,uint64_t pc){(void)cpu;(void)imm;(void)pc;abort();}
int main(void) {
    const uint64_t base=UINT64_C(0x3000000000);ModuleAbiModuleSpec specs[2];ModuleAbiModule modules[2];
    uint64_t addresses[2]={base+4,base+0x100004};GuestFn functions[2]={dummy,dummy};
    memset(specs,0,sizeof specs);memset(modules,0,sizeof modules);
    for(unsigned i=0;i<2;i++) {
        specs[i]=(ModuleAbiModuleSpec){.struct_size=sizeof specs[i],.module_id=i+1,.canonical_base=base+0x100000*i,.mapped_size=0x40000,.text_size=0x20000,.function_count=1};
        specs[i].nso_build_id[0]=(uint8_t)(i+1);specs[i].nso_sha256[0]=(uint8_t)(i+2);
        modules[i]=(ModuleAbiModule){.struct_size=sizeof modules[i],.image=specs[i],.address_count=1,.addresses=&addresses[i],.functions=&functions[i]};
    }
    ModuleAbiIdentity identity={.struct_size=sizeof identity,.abi_version=MODULE_ABI_VERSION,.target=MODULE_ABI_TARGET_ANDROID_ARM64_LP64,.cpu_identity=MODULE_ABI_CPU_A64_SHARED_66560,.cpu_size=sizeof(Cpu),.cpu_alignment=_Alignof(Cpu),.guest_window_lo=base,.guest_window_hi=base+0x1000000};
    ModuleAbiProfile profile={.struct_size=sizeof profile,.module_count=2,.identity=identity,.modules=specs};
    ModuleAbiDescriptor descriptor={.struct_size=sizeof descriptor,.module_count=2,.identity=identity,.modules=modules};
    CHECK(!aot_dispatch_override_count()&&!aot_instruction_hook_count());uint64_t value=0xa5;
    CHECK(!aot_dispatch_override_address(0,&value)&&value==0xa5&&!aot_instruction_hook_address(0,&value)&&value==0xa5);
    CHECK(aot_dispatch_bind(&descriptor,&profile,1,1,NULL)==AOT_OK);CHECK(aot_dispatch_set_execution_backend(dummy)==AOT_OK);
    for(unsigned i=0;i<AOT_MAX_DISPATCH_OVERRIDES;i++)CHECK(aot_dispatch_override(base+i*4,dummy)==AOT_OK);
    for(unsigned i=0;i<AOT_MAX_INSTRUCTION_HOOKS;i++)CHECK(rt_register_instruction_hook(base+0x10000+i*4,hook)==AOT_OK);
    CHECK(aot_dispatch_override_count()==AOT_MAX_DISPATCH_OVERRIDES&&aot_instruction_hook_count()==AOT_MAX_INSTRUCTION_HOOKS);
    for(unsigned i=0;i<AOT_MAX_DISPATCH_OVERRIDES;i++){CHECK(aot_dispatch_override_address(i,&value)&&value==base+i*4);CHECK(aot_dispatch_host_lookup(value)==dummy);}
    for(unsigned i=0;i<AOT_MAX_INSTRUCTION_HOOKS;i++){CHECK(aot_instruction_hook_address(i,&value)&&value==base+0x10000+i*4);CHECK(aot_instruction_hook_registered(value));}
    CHECK(aot_dispatch_override(base,second)==AOT_OK&&aot_dispatch_override_count()==AOT_MAX_DISPATCH_OVERRIDES&&aot_dispatch_host_lookup(base)==second);
    CHECK(rt_register_instruction_hook(base+0x10000,hook2)==AOT_OK&&aot_instruction_hook_count()==AOT_MAX_INSTRUCTION_HOOKS);
    Cpu cpu={0};CHECK(rt_hook(&cpu,base+0x10000)==0);
    value=0xa5;CHECK(!aot_dispatch_override_address(SIZE_MAX,&value)&&value==0xa5);CHECK(!aot_instruction_hook_address(SIZE_MAX,&value)&&value==0xa5);
    CHECK(!aot_dispatch_override_address(0,NULL)&&!aot_instruction_hook_address(0,NULL));
    CHECK(aot_dispatch_override(base+0x10000,second)==AOT_CAPACITY);CHECK(rt_register_instruction_hook(base+0x18000,hook)==AOT_CAPACITY);
    printf("{\"state\":\"passed\",\"suite\":\"actual-registry-enumeration\",\"checks\":%u,\"overrides\":%u,\"instruction_hooks\":%u}\n",checks,AOT_MAX_DISPATCH_OVERRIDES,AOT_MAX_INSTRUCTION_HOOKS);return 0;
}
#else
#include "continuous_runtime.h"
#include "continuous_context.h"
#include "continuous_image.h"
#include "continuous_signals.h"
#include "native_hooks.h"
#include "rt.h"
#define START UINT64_C(0x10000000)
#define SENTINEL UINT64_C(0x90000000)
const NativeManifest *shared_native_manifest_current(void) { return NULL; }
static Cpu *current_cpu;
static CnceSite sites[64],sentinel={.original_pc=SENTINEL,.resume_pc=SENTINEL+4,.original_word=0xd503201f,.kind=CNCE_PATCH_HOST_FUNCTION};
static unsigned site_count,context_calls,host_hits,svc_hits,hook_hits,unwind_hits,init_count,finish_count,interrupt_count;
static CnceContext *contexts[128];
static uint64_t expected_resume,hook_target,armed_original;
static int hook_result,escape_allowed=1,signal_once,stop_on_unwind,expected_fatal,signals_fail,missing_event,invalid_event;
static uint32_t signal_reason;
static jmp_buf recover;
static char fatal_message[512];
static GuestFn host_function;
static const SessionLayout layout={0};
Module g_modules[4]={{.base=START,.size=0x10000,.text_size=0x8000},{.base=START+0x100000,.size=0x10000,.text_size=0x8000}};
int g_nmodules=2;
const SessionLayout *session_layout_current(void){return &layout;}
int aot_dispatch_is_bound(void){return 1;}
size_t aot_dispatch_override_count(void){return 2;}
int aot_dispatch_override_address(size_t index,uint64_t *pc){if(index>=2||!pc)return 0;*pc=START+0x400+index*4;return 1;}
size_t aot_instruction_hook_count(void){return 2;}
int aot_instruction_hook_address(size_t index,uint64_t *pc){if(index>=2||!pc)return 0;*pc=START+0x500+index*4;return 1;}
GuestFn aot_dispatch_host_lookup(uint64_t pc){(void)pc;return host_function;}
int aot_original_call_take(Cpu *cpu,uint64_t pc){CHECK(cpu==current_cpu);if(armed_original!=pc)return 0;armed_original=0;return 1;}
int continuous_image_prepare(const SessionLayout *selected,const CnceImageModule *modules,size_t count,const CncePatchRequest *requests,size_t requests_count,uint64_t gateway,char *error,size_t capacity){
    CHECK(selected==&layout&&modules&&count==2&&requests_count==4&&gateway);
    CHECK(requests[0].kind==CNCE_PATCH_HOST_FUNCTION&&requests[0].pc==START+0x400);
    CHECK(requests[2].kind==CNCE_PATCH_INSTRUCTION_HOOK&&requests[2].pc==START+0x500);
    if(error&&capacity)error[0]=0;return 0;
}
void continuous_image_finish_after_join(void){finish_count++;}
uint64_t continuous_image_return_target(void){return SENTINEL;}
const void *continuous_image_resume(Cpu *cpu,uint64_t pc){CHECK(cpu==current_cpu);if(pc==SENTINEL)return NULL;expected_resume=pc;return (const void*)(uintptr_t)(pc|1);}
const void *continuous_image_escape(Cpu *cpu,const CnceSite *site){CHECK(cpu==current_cpu);return escape_allowed?(const void*)(uintptr_t)(site->original_pc|2):NULL;}
int continuous_image_valid_event(const CnceSite *site){return site==&sentinel||((uintptr_t)site>=(uintptr_t)sites&&(uintptr_t)site<(uintptr_t)(sites+site_count));}
int continuous_image_is_original_guest_pc(uint64_t pc){return pc>=START&&pc<START+0x200000;}
uint64_t continuous_image_original_pc(uint64_t pc){return continuous_image_is_original_guest_pc(pc)?pc:0;}
void continuous_context_exit(void){}
int continuous_context_init(CnceContext *ctx,Cpu *cpu){CHECK(ctx&&cpu==current_cpu);memset(ctx,0,sizeof *ctx);ctx->cpu=cpu;ctx->magic=CNCE_CONTEXT_MAGIC;ctx->self=ctx;if(init_count<128)contexts[init_count++]=ctx;return 0;}
int continuous_context_run(CnceContext *ctx,const void *entry){
    CHECK(ctx->cpu==current_cpu&&ctx->magic==CNCE_CONTEXT_MAGIC&&ctx->self==ctx);context_calls++;ctx->event=NULL;
    if(missing_event)return 0;
    if(invalid_event){ctx->event=(const CnceSite*)(uintptr_t)4;return 0;}
    if(signal_once){signal_once=0;signal_reason=CONTINUOUS_SIGNAL_PAUSE;return 0;}
    uint64_t pc=(uintptr_t)entry&~UINT64_C(3);
    if(((uintptr_t)entry&3)==2){current_cpu->x[7]++;pc+=4;}
    for(unsigned i=0;i<site_count;i++)if(sites[i].original_pc==pc){ctx->event=&sites[i];current_cpu->pc=sites[i].resume_pc;return 0;}
    (void)expected_resume;ctx->event=&sentinel;current_cpu->pc=SENTINEL+4;return 0;
}
int continuous_signals_configure(const ContinuousSignalHooks *hooks){CHECK(hooks&&hooks->is_guest_pc&&hooks->original_pc);return signals_fail;}
uint32_t continuous_signals_take_reason(Cpu *cpu){CHECK(cpu==current_cpu);uint32_t value=signal_reason;signal_reason=0;return value;}
int continuous_signals_interrupt_cpu(Cpu *cpu,uint32_t reasons){CHECK(cpu==current_cpu&&reasons==CONTINUOUS_SIGNAL_STOP);interrupt_count++;return 0;}
Cpu *kernel_current_cpu(void){return current_cpu;}
uint64_t rt_ticks(void){return UINT64_C(123456789);}
uint64_t plat_now_ns(void){static uint64_t tick;return ++tick;}
uint64_t renderer_present_count(void){return 0;}
void rt_unwind_point(void){unwind_hits++;if(stop_on_unwind)longjmp(recover,2);}
void rt_fatal_msg(const char *format,...){va_list args;va_start(args,format);vsnprintf(fatal_message,sizeof fatal_message,format,args);va_end(args);}
void rt_fatal_trace(void){if(expected_fatal)longjmp(recover,1);fprintf(stderr,"unexpected runtime fatal: %s",fatal_message);exit(1);}
void rt_svc(Cpu *cpu,uint32_t imm,uint64_t pc){CHECK(cpu==current_cpu&&pc);svc_hits++;cpu->x[6]=imm;if(imm==0xbeef)longjmp(recover,2);}
int native_hooks_apply(Cpu *cpu,uint64_t pc){CHECK(cpu==current_cpu&&pc);hook_hits++;if(hook_result==2)cpu->pc=hook_target;return hook_result;}
static CnceSite *add_site(uint64_t pc,uint32_t kind,uint32_t word,unsigned operand){CHECK(site_count<64);CnceSite *site=&sites[site_count++];*site=(CnceSite){.original_pc=pc,.resume_pc=pc+4,.original_word=word,.kind=kind,.operand=operand};return site;}
static void invoke(Cpu *cpu,uint64_t pc){cpu->pc=pc;cpu->x[30]=0;continuous_runtime_run(cpu);CHECK(!cpu->pc&&!cpu->x[30]);}
static void simple_host(Cpu *cpu){CHECK(cpu==current_cpu);host_hits++;cpu->x[0]+=9;}
static void nested_host(Cpu *cpu){host_hits++;uint64_t lr=cpu->x[30];cpu->pc=START+0x100;cpu->x[30]=0;continuous_runtime_run(cpu);cpu->x[30]=lr;cpu->x[5]++;}
static void test_host(Cpu *cpu){add_site(START,CNCE_PATCH_HOST_FUNCTION,0xd503201f,0);host_function=simple_host;invoke(cpu,START);CHECK(host_hits==1&&cpu->x[0]==9);}
static void test_nested(Cpu *cpu,int exiting){add_site(START,CNCE_PATCH_HOST_FUNCTION,0xd503201f,0);add_site(START+0x100,CNCE_PATCH_SVC,0xd4000021,exiting?0xbeef:1);host_function=nested_host;
    if(!exiting){invoke(cpu,START);CHECK(host_hits==1&&svc_hits==1&&cpu->x[5]==1&&init_count>=2&&contexts[0]!=contexts[1]);}
    else {if(!setjmp(recover)){invoke(cpu,START);CHECK(0);}CHECK(svc_hits==1&&host_hits==1&&init_count>=2);for(unsigned i=0;i<init_count;i++)CHECK(contexts[i]->magic==CNCE_CONTEXT_MAGIC&&contexts[i]->cpu==cpu);}
}
static void test_virtual(Cpu *cpu){unsigned index=0;cpu->tpidr=0x1234;cpu->tpidrro=0x5678;cpu->x[4]=0xabcd;
    add_site(START+4*index++,CNCE_PATCH_TPIDR_READ,0,0);add_site(START+4*index++,CNCE_PATCH_TPIDRRO_READ,0,1);add_site(START+4*index++,CNCE_PATCH_TPIDR_WRITE,0,4);add_site(START+4*index++,CNCE_PATCH_TPIDR_READ,0,2);add_site(START+4*index++,CNCE_PATCH_TPIDR_WRITE,0,31);add_site(START+4*index++,CNCE_PATCH_COUNTER_READ,0,3);add_site(START+4*index++,CNCE_PATCH_FREQUENCY_READ,0,4);add_site(START+4*index++,CNCE_PATCH_COUNTER_READ,0,31);
    invoke(cpu,START);CHECK(cpu->x[0]==0x1234&&cpu->x[1]==0x5678&&cpu->x[2]==0xabcd&&!cpu->tpidr&&cpu->x[3]==123456789&&cpu->x[4]==19200000&&cpu->x[31]==0);}
static void test_original(Cpu *cpu){add_site(START,CNCE_PATCH_HOST_FUNCTION,0x91000400,0);host_function=simple_host;armed_original=START;invoke(cpu,START);CHECK(!host_hits&&!armed_original&&cpu->x[7]==1);}
static void test_hooks(Cpu *cpu){
    add_site(START,CNCE_PATCH_INSTRUCTION_HOOK,0x14000004,0);hook_result=0;invoke(cpu,START);CHECK(hook_hits==1&&!cpu->x[7]);
    hook_result=1;invoke(cpu,START);CHECK(hook_hits==2&&!cpu->x[7]);
    hook_result=2;hook_target=START+0x200;invoke(cpu,START);CHECK(hook_hits==3&&!cpu->x[7]);
}
static void test_literals(Cpu *cpu){_Alignas(16) unsigned char bytes[128];const uint32_t words[]={0x18000200,0x58000200,0x98000200,0x1c000200,0x5c000200,0x9c000200,0xd8000200};
    for(unsigned k=0;k<sizeof words/sizeof words[0];k++) {
        memset(bytes,0xa5,sizeof bytes);uint32_t low=0x87654321;memcpy(bytes+64,&low,4);uint64_t pc=(uintptr_t)bytes;site_count=0;add_site(pc,CNCE_PATCH_INSTRUCTION_HOOK,words[k],0);hook_result=0;cpu->x[0]=0x1122;cpu->v[0]=~(__uint128_t)0;invoke(cpu,pc);
        if(k==0)CHECK(cpu->x[0]==low);if(k==1)CHECK(cpu->x[0]==UINT64_C(0xa5a5a5a587654321));if(k==2)CHECK(cpu->x[0]==UINT64_C(0xffffffff87654321));
        if(k>=3&&k<=5){__uint128_t value=0;memcpy(&value,bytes+64,(size_t)4<<(k-3));CHECK(cpu->v[0]==value);}if(k==6)CHECK(cpu->x[0]==0x1122);
    }
}
static void test_stop(Cpu *cpu){signal_once=1;invoke(cpu,START);CHECK(context_calls==2);continuous_runtime_interrupt_all();CHECK(interrupt_count==1);stop_on_unwind=1;if(!setjmp(recover)){invoke(cpu,START);CHECK(0);}stop_on_unwind=0;CHECK(unwind_hits>0);}
static void test_fatal(Cpu *cpu,const char *name){
    if(!strcmp(name,"invalid-event"))invalid_event=1;
    else if(!strcmp(name,"missing-event"))missing_event=1;
    else if(!strcmp(name,"unbound-host"))add_site(START,CNCE_PATCH_HOST_FUNCTION,0xd503201f,0);
    else {add_site(START,CNCE_PATCH_INSTRUCTION_HOOK,0xd503201f,0);hook_result=3;}
    expected_fatal=1;if(!setjmp(recover)){invoke(cpu,START);CHECK(0);}CHECK(fatal_message[0]);expected_fatal=0;
}
int main(int argc,char **argv){CHECK(argc==2);Cpu cpu={0};current_cpu=&cpu;
    if(!strcmp(argv[1],"configure-failure")){signals_fail=1;CHECK(continuous_runtime_configure()==-1&&finish_count==1);signals_fail=0;finish_count=0;}
    CHECK(!continuous_runtime_configure());
    if(!strcmp(argv[1],"sentinel"))invoke(&cpu,START);
    else if(!strcmp(argv[1],"host"))test_host(&cpu);
    else if(!strcmp(argv[1],"nested"))test_nested(&cpu,0);
    else if(!strcmp(argv[1],"exit-thread"))test_nested(&cpu,1);
    else if(!strcmp(argv[1],"virtual"))test_virtual(&cpu);
    else if(!strcmp(argv[1],"original"))test_original(&cpu);
    else if(!strcmp(argv[1],"hooks"))test_hooks(&cpu);
    else if(!strcmp(argv[1],"literals"))test_literals(&cpu);
    else if(!strcmp(argv[1],"stop"))test_stop(&cpu);
    else if(!strcmp(argv[1],"configure-failure"))invoke(&cpu,START);
    else if(!strcmp(argv[1],"invalid-event")||!strcmp(argv[1],"missing-event")||!strcmp(argv[1],"unbound-host")||!strcmp(argv[1],"invalid-hook"))test_fatal(&cpu,argv[1]);
    else CHECK(0);
    continuous_runtime_finish_after_join();CHECK(finish_count==1);printf("{\"state\":\"passed\",\"case\":\"%s\",\"checks\":%u,\"context_calls\":%u,\"mock_gateway_only\":true}\n",argv[1],checks,context_calls);return 0;
}
#endif
