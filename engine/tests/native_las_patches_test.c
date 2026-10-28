/* Synthetic selected-version table; actual patch helpers and framepace C. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "native_las_patches.h"
#include "native_hooks.h"
#include "runtime/aot_hooks.h"
#include "runtime/session_layout.h"
#include "game_addresses.h"
#include "settings.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks,registered,fallback_calls,aspect_calls,tilt_calls;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
static GameAddrs selected;
const GameAddrs *g_ga=&selected;
static SessionLayout layout={.guest_shift=0x20000000};
static int is_las=1,is_native=2,dynres=1,delegate_skip,fallback_selection;
static uint64_t text_lo,text_hi,tilt_obj;
static int aspect_id,delegate_slot;
static struct {uint64_t pc;AotInstructionHook callback;} callbacks[32];
Settings g_settings;
volatile uint64_t g_vsync_count;
extern volatile bool g_pace_slow;
extern uint32_t g_game_interval;
int native_session_requested(void){return is_native;}
bool shared_title_las(void){return is_las;}
const SessionLayout *session_layout_current(void){return &layout;}
int aot_dispatch_is_bound(void){return 1;}
uint32_t aot_dispatch_capabilities(void){return AOT_CAP_INSTRUCTION_HOOKS;}
int aot_dispatch_contains_text(uint64_t pc){return !(pc&3)&&pc>=text_lo&&pc<text_hi;}
size_t aot_instruction_hook_count(void){return registered;}
int aot_instruction_hook_registered(uint64_t pc){for(unsigned i=0;i<registered;i++)if(callbacks[i].pc==pc)return 1;return 0;}
enum AotStatus rt_register_instruction_hook(uint64_t pc,AotInstructionHook callback){CHECK(registered<32&&!aot_instruction_hook_registered(pc));callbacks[registered].pc=pc;callbacks[registered++].callback=callback;return AOT_OK;}
size_t aot_instruction_hook_limit(uint64_t pc,size_t words){for(size_t i=0;i<words;i++)if(aot_instruction_hook_registered(pc+i*4))return i;return words;}
int rt_hook(Cpu *cpu,uint64_t pc){(void)cpu;(void)pc;fallback_calls++;return fallback_selection;}
int rt_game_dynres(void){return dynres;}
float rt_aspect_site(int site){aspect_calls++;aspect_id=site;return 2.0f+(float)site/8.0f;}
float rt_cull_aspect(float value){return value*1.25f;}
float rt_fovy(float value){return value+0.5f;}
float rt_skysea_margin(void){return 5.0f;}
float rt_indoor_aspect(void){return 1.875f;}
void rt_tilt_obj(uint64_t object){tilt_calls++;tilt_obj=object;}
int rt_delegate_null(Cpu *cpu,int slot){delegate_slot=slot;if(delegate_skip)cpu->x[0]=0;return delegate_skip;}
uint64_t plat_now_ns(void){static uint64_t value;return value+=16666667;}
uint64_t doorjump_player(uint64_t frame){(void)frame;return 0;}
void rt_fatal_msg(const char *format,...){va_list args;va_start(args,format);vfprintf(stderr,format,args);va_end(args);}
void rt_fatal_trace(void){abort();}
static uint32_t load32(const void *pointer){uint32_t value;memcpy(&value,pointer,4);return value;}
static float vector_float(const Cpu *cpu,unsigned reg){float value;memcpy(&value,&cpu->v[reg],4);return value;}
static void put_float(unsigned char *memory,size_t offset,float value){memcpy(memory+offset,&value,4);}
static uint64_t actual(size_t field){uint64_t value;memcpy(&value,(const unsigned char*)&selected+field,8);return value-layout.guest_shift;}
#define PC(field_) actual(offsetof(GameAddrs,field_))
typedef struct Fixture {size_t field;uint32_t word;} Fixture;
#define FIX(field_,word_) {offsetof(GameAddrs,field_),word_}
static const Fixture fixtures[]={FIX(FRAME_STEPS_SITE,0xb8696908),FIX(INTERVAL_CHOICE_SITE,0x391386e9),FIX(GAME_DYNRES_SITE,0x395922e8),FIX(GAME_DYNRES_INIT_SITE,0xb86d698c),FIX(ASPECT_SITES_0,0xbd4ba401),FIX(ASPECT_SITES_1,0xbd4ba422),FIX(ASPECT_SITES_2,0xbd4ba443),FIX(ASPECT_SITES_3,0xbd4ba464),FIX(ASPECT_SITES_4,0xbd4ba485),FIX(ASPECT_SITES_5,0xbd4ba4bf),FIX(CULL_ASPECT,0xbd457369),FIX(FOVY_SITES_0,0xbd4566e0),FIX(FOVY_SITES_1,0xbd456768),FIX(SKYSEA_BOX_MAX_SITE,0x3dc0bae1),FIX(SKYSEA_BOX_MIN_SITE,0x3dc0b6e2),FIX(INDOOR_SITE,0xbd4ba4c7),FIX(TILT_INIT_SITE,0xd10243ff),FIX(FONT_BOX_GLYPH_SITE,0x3940e008),FIX(DELEGATE_FORWARDER_SITES_0,0xaa0003e1),FIX(DELEGATE_FORWARDER_SITES_1,0xaa0003e1)};
#undef FIX
static void fixture(unsigned char *code){text_lo=(uintptr_t)code;text_hi=text_lo+4096;
    for(unsigned i=0;i<sizeof fixtures/sizeof fixtures[0];i++){uint64_t canonical=text_lo+i*16+layout.guest_shift;memcpy((unsigned char*)&selected+fixtures[i].field,&canonical,8);memcpy(code+i*16,&fixtures[i].word,4);}
    selected.FONT_BOX_GLYPH_SKIP=text_lo+0x800+layout.guest_shift;
    selected.CRASHOUT_SITE=text_lo+0x900+layout.guest_shift;selected.CRASHOUT_SKIP=text_lo+0x910+layout.guest_shift;
}
static void invoke(Cpu *cpu,uint64_t pc){CHECK(native_hooks_apply(cpu,pc)==1);}
static void scalar_output(Cpu *cpu,Cpu *before,unsigned reg,float expected){before->v[reg]=0;memcpy(&before->v[reg],&expected,4);CHECK(!memcmp(cpu,before,sizeof *cpu));}
int main(int argc,char **argv){CHECK(argc==2);_Alignas(16) unsigned char code[4096]={0},memory[4096]={0};fixture(code);Cpu *cpu=malloc(sizeof *cpu),*before=malloc(sizeof *before);CHECK(cpu&&before);
    if(!strcmp(argv[1],"eevee")){is_las=0;CHECK(!native_las_patches_register()&&!registered);goto done;}
    if(!strcmp(argv[1],"aot")){is_native=0;CHECK(!native_las_patches_register()&&!registered);goto done;}
    if(!strcmp(argv[1],"bad-opcode")){code[19*16]^=1;CHECK(native_las_patches_register()==-1&&!registered);goto done;}
    if(!strcmp(argv[1],"bad-target")){selected.FONT_BOX_GLYPH_SKIP=text_hi+layout.guest_shift;CHECK(native_las_patches_register()==-1&&!registered);goto done;}
    CHECK(!native_las_patches_register()&&registered==20);CHECK(!native_las_patches_register()&&registered==20);
    memset(cpu,0xa5,sizeof *cpu);cpu->x[8]=(uintptr_t)(memory+1);cpu->x[9]=2;uint32_t interval=2;memcpy(memory+3,&interval,4);*before=*cpu;before->x[8]=2;invoke(cpu,PC(FRAME_STEPS_SITE));CHECK(cpu->x[8]==2&&g_game_interval==2&&!memcmp(cpu,before,sizeof *cpu));
    cpu->x[23]=(uintptr_t)memory;cpu->x[24]=17;cpu->x[19]=3;cpu->x[21]=8;memory[0x4e2]=0;memory[0x4e3]=1;
    for(int rate=0;rate<3;rate++){g_settings.frame_rate=rate;g_pace_slow=false;cpu->x[9]=0;*before=*cpu;before->x[9]=rate==FRAME_RATE_FIXED_30?1:0;invoke(cpu,PC(INTERVAL_CHOICE_SITE));CHECK(cpu->x[9]==before->x[9]&&memory[0x4e1]==before->x[9]&&!memcmp(cpu,before,sizeof *cpu));}
    g_settings.frame_rate=FRAME_RATE_AUTO;g_pace_slow=true;cpu->x[9]=0;invoke(cpu,PC(INTERVAL_CHOICE_SITE));CHECK(cpu->x[9]==1&&memory[0x4e1]==1);
    dynres=0;cpu->x[23]=1;invoke(cpu,PC(GAME_DYNRES_SITE));CHECK(!cpu->x[8]);cpu->x[12]=1;cpu->x[13]=0;invoke(cpu,PC(GAME_DYNRES_INIT_SITE));CHECK(!cpu->x[12]);
    dynres=1;cpu->x[23]=(uintptr_t)memory;memory[0x648]=0xfe;invoke(cpu,PC(GAME_DYNRES_SITE));CHECK(cpu->x[8]==0xfe);cpu->x[12]=(uintptr_t)(memory+1);cpu->x[13]=2;uint32_t level=0xf1234567;memcpy(memory+3,&level,4);invoke(cpu,PC(GAME_DYNRES_INIT_SITE));CHECK(cpu->x[12]==level);
    for(unsigned i=4;i<10;i++){memset(cpu,0xa5,sizeof *cpu);*before=*cpu;invoke(cpu,actual(fixtures[i].field));CHECK(aspect_id==(int)i-4);scalar_output(cpu,before,fixtures[i].word&31,2.0f+(float)(i-4)/8.0f);}
    memset(cpu,0xa5,sizeof *cpu);cpu->x[27]=(uintptr_t)memory;put_float(memory,0x570,2.0f);*before=*cpu;invoke(cpu,PC(CULL_ASPECT));scalar_output(cpu,before,9,2.5f);
    for(unsigned i=11;i<13;i++){memset(cpu,0xa5,sizeof *cpu);cpu->x[i==11?23:27]=(uintptr_t)memory;put_float(memory,0x564,1.0f);*before=*cpu;invoke(cpu,actual(fixtures[i].field));scalar_output(cpu,before,fixtures[i].word&31,1.5f);}
    for(unsigned i=13;i<15;i++){memset(cpu,0xa5,sizeof *cpu);cpu->x[23]=(uintptr_t)memory;float values[]={1,2,3,4};size_t offset=i==13?0x2e0:0x2d0;memcpy(memory+offset,values,16);*before=*cpu;values[0]+=i==13?5.0f:-5.0f;memcpy(&before->v[fixtures[i].word&31],values,16);invoke(cpu,actual(fixtures[i].field));CHECK(!memcmp(cpu,before,sizeof *cpu));}
    memset(cpu,0xa5,sizeof *cpu);*before=*cpu;invoke(cpu,PC(INDOOR_SITE));scalar_output(cpu,before,fixtures[15].word&31,1.875f);
    cpu->x[0]=0x1234;cpu->sp=0x10000;*before=*cpu;before->sp-=0x90;invoke(cpu,PC(TILT_INIT_SITE));CHECK(tilt_calls==1&&tilt_obj==0x1234&&!memcmp(cpu,before,sizeof *cpu));
    cpu->x[0]=0;CHECK(native_hooks_apply(cpu,PC(FONT_BOX_GLYPH_SITE))==2&&cpu->pc==PC(FONT_BOX_GLYPH_SKIP));cpu->x[0]=(uintptr_t)memory;memory[0x38]=0xe7;invoke(cpu,PC(FONT_BOX_GLYPH_SITE));CHECK(cpu->x[8]==0xe7);
    for(unsigned i=18;i<20;i++){delegate_skip=0;cpu->x[0]=0x1234;invoke(cpu,actual(fixtures[i].field));CHECK(cpu->x[1]==0x1234&&delegate_slot==(i==18?0x28:0x30));delegate_skip=1;cpu->x[30]=0xabcd;CHECK(native_hooks_apply(cpu,actual(fixtures[i].field))==2&&cpu->pc==0xabcd&&!cpu->x[0]);}
    CHECK(!fallback_calls&&aspect_calls==6);fallback_selection=1;callbacks[registered++].pc=PC(CRASHOUT_SITE);invoke(cpu,PC(CRASHOUT_SITE));CHECK(fallback_calls==1);fallback_selection=2;CHECK(native_hooks_apply(cpu,PC(CRASHOUT_SITE))==2&&cpu->pc==PC(CRASHOUT_SKIP));
    CHECK(native_hooks_limit(cpu,PC(FRAME_STEPS_SITE)-4,10)==1);CHECK(native_las_patches_apply(cpu,text_hi)==-1&&native_las_patches_apply(NULL,PC(FRAME_STEPS_SITE))==-1);
    CHECK(load32(code)==fixtures[0].word);CHECK(vector_float(cpu,7)==1.875f);
done:
    free(before);free(cpu);printf("{\"state\":\"passed\",\"case\":\"%s\",\"checks\":%u,\"actual_framepace_source\":true,\"selected_sites\":%u,\"game_assets\":false}\n",argv[1],checks,registered);return 0;
}
