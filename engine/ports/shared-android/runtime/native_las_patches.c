/* Semantics adapted from LAS Recomp Android tools/patches.py, pinned
 * cca791a3e7218e1dac95e7d4a11b8f97dba3ea51. Copyright (c)2026 Peiikai.
 * MIT: native/runtime/LICENSE-LASRecomp. Runtime-selected fields, no addresses. */
#include "native_las_patches.h"
#include "native_session.h"
#include "game_addresses.h"
#include "rt.h"
#include "runtime/aot_hooks.h"
#include <stddef.h>
#include <string.h>

enum PatchKind {FRAME_STEPS,INTERVAL,DYNRES,DYNRES_INIT,ASPECT,CULL,FOVY,SKYBOX,INDOOR,TILT,FONT,DELEGATE};
typedef struct PatchSpec {size_t field;uint32_t mask,expected;unsigned kind,aux;size_t target;} PatchSpec;
#define SPEC(field_,mask_,word_,kind_,aux_) {offsetof(GameAddrs,field_),mask_,word_,kind_,aux_,0}
static const PatchSpec specs[]={
    SPEC(FRAME_STEPS_SITE,0xffffffffu,0xb8696908u,FRAME_STEPS,0),
    SPEC(INTERVAL_CHOICE_SITE,0xffffffffu,0x391386e9u,INTERVAL,0),
    SPEC(GAME_DYNRES_SITE,0xffffffffu,0x395922e8u,DYNRES,0),
    SPEC(GAME_DYNRES_INIT_SITE,0xffffffffu,0xb86d698cu,DYNRES_INIT,0),
    SPEC(ASPECT_SITES_0,0xfffffc00u,0xbd4ba400u,ASPECT,0),
    SPEC(ASPECT_SITES_1,0xfffffc00u,0xbd4ba400u,ASPECT,1),
    SPEC(ASPECT_SITES_2,0xfffffc00u,0xbd4ba400u,ASPECT,2),
    SPEC(ASPECT_SITES_3,0xfffffc00u,0xbd4ba400u,ASPECT,3),
    SPEC(ASPECT_SITES_4,0xfffffc00u,0xbd4ba400u,ASPECT,4),
    SPEC(ASPECT_SITES_5,0xfffffc00u,0xbd4ba400u,ASPECT,5),
    SPEC(CULL_ASPECT,0xffffffffu,0xbd457369u,CULL,0),
    SPEC(FOVY_SITES_0,0xffffffffu,0xbd4566e0u,FOVY,23),
    SPEC(FOVY_SITES_1,0xffffffffu,0xbd456768u,FOVY,27),
    SPEC(SKYSEA_BOX_MAX_SITE,0xffffffffu,0x3dc0bae1u,SKYBOX,1),
    SPEC(SKYSEA_BOX_MIN_SITE,0xffffffffu,0x3dc0b6e2u,SKYBOX,0),
    SPEC(INDOOR_SITE,0xfffffc00u,0xbd4ba400u,INDOOR,0),
    SPEC(TILT_INIT_SITE,0xffffffffu,0xd10243ffu,TILT,0),
    {offsetof(GameAddrs,FONT_BOX_GLYPH_SITE),0xffffffffu,0x3940e008u,FONT,0,offsetof(GameAddrs,FONT_BOX_GLYPH_SKIP)},
    SPEC(DELEGATE_FORWARDER_SITES_0,0xffffffffu,0xaa0003e1u,DELEGATE,0x28),
    SPEC(DELEGATE_FORWARDER_SITES_1,0xffffffffu,0xaa0003e1u,DELEGATE,0x30),
};
#undef SPEC
typedef struct PatchEntry {uint64_t pc,target;uint32_t word;unsigned kind,aux;} PatchEntry;
static PatchEntry entries[sizeof specs/sizeof specs[0]];
static unsigned entry_count;
static const GameAddrs *selected_addresses;
static uint64_t selected_shift;
extern uint32_t rt_frame_steps(uint32_t);
extern uint32_t rt_interval_choice(uint32_t,uint64_t,uint64_t,uint64_t);
extern int rt_game_dynres(void);
extern float rt_aspect_site(int),rt_cull_aspect(float),rt_fovy(float),rt_skysea_margin(void),rt_indoor_aspect(void);
extern void rt_tilt_obj(uint64_t);
extern int rt_delegate_null(Cpu*,int);
static uint64_t field_value(size_t offset){uint64_t value;memcpy(&value,(const unsigned char*)g_ga+offset,8);return value;}
static uint32_t load32(uint64_t address){uint32_t value;memcpy(&value,(const void*)(uintptr_t)address,4);return value;}
static float load_float(uint64_t address){float value;memcpy(&value,(const void*)(uintptr_t)address,4);return value;}
static void scalar_float(Cpu *cpu,unsigned reg,float value){cpu->v[reg]=0;memcpy(&cpu->v[reg],&value,4);}
static int callback(Cpu *cpu,uint64_t pc){int result=native_las_patches_apply(cpu,pc);if(result<0)FATAL("native LAS patch callback has no selected site\n");return result;}
int native_las_patches_register(void) {
    if(!native_session_requested()||!shared_title_las())return 0;
    if(!g_ga||!aot_dispatch_is_bound()||!session_layout_current()||
       !(aot_dispatch_capabilities()&AOT_CAP_INSTRUCTION_HOOKS))return -1;
    const uint64_t shift=session_layout_current()->guest_shift;
    if(entry_count)return selected_addresses==g_ga&&selected_shift==shift?0:-1;
    PatchEntry prepared[sizeof specs/sizeof specs[0]];
    for(unsigned i=0;i<sizeof specs/sizeof specs[0];i++) {
        const PatchSpec *spec=&specs[i];uint64_t canonical=field_value(spec->field);
        if(canonical<shift||!aot_dispatch_contains_text(canonical-shift))return -1;
        uint64_t pc=canonical-shift;uint32_t word=load32(pc);
        if((word&spec->mask)!=spec->expected)return -1;
        uint64_t target=0;
        if(spec->target){uint64_t selected=field_value(spec->target);if(selected<shift||!aot_dispatch_contains_text(selected-shift))return -1;target=selected-shift;}
        for(unsigned prior=0;prior<i;prior++)if(prepared[prior].pc==pc)return -1;
        /* Replacing a preexisting HOOKS callback would hide its semantics. */
        if(aot_instruction_hook_registered(pc))return -1;
        prepared[i]=(PatchEntry){pc,target,word,spec->kind,spec->aux};
    }
    if(aot_instruction_hook_count()>AOT_MAX_INSTRUCTION_HOOKS-sizeof specs/sizeof specs[0])return -1;
    memcpy(entries,prepared,sizeof prepared);
    for(unsigned i=0;i<sizeof specs/sizeof specs[0];i++)if(rt_register_instruction_hook(entries[i].pc,callback)!=AOT_OK)return -1;
    entry_count=sizeof specs/sizeof specs[0];selected_addresses=g_ga;selected_shift=shift;
    LOG("[native-las] %u selected-version pacing, resolution and graphics compatibility patches registered\n",entry_count);
    return 0;
}
int native_las_patches_apply(Cpu *cpu,uint64_t pc) {
    if(!cpu||!shared_title_las())return -1;
    const PatchEntry *entry=NULL;
    for(unsigned i=0;i<entry_count;i++)if(entries[i].pc==pc){entry=&entries[i];break;}
    if(!entry)return -1;
    unsigned reg=entry->word&31;
    switch(entry->kind) {
    case FRAME_STEPS:cpu->x[8]=rt_frame_steps(load32(cpu->x[8]+cpu->x[9]));break;
    case INTERVAL: {
        cpu->x[9]=rt_interval_choice((uint32_t)cpu->x[9],cpu->x[23],cpu->x[24],cpu->x[19]-cpu->x[21]);
        uint8_t value=(uint8_t)cpu->x[9];memcpy((void*)(uintptr_t)(cpu->x[23]+0x4e1),&value,1);break;
    }
    case DYNRES:cpu->x[8]=rt_game_dynres()?*(const uint8_t*)(uintptr_t)(cpu->x[23]+0x648):0;break;
    case DYNRES_INIT:cpu->x[12]=rt_game_dynres()?load32(cpu->x[12]+cpu->x[13]):0;break;
    case ASPECT:scalar_float(cpu,reg,rt_aspect_site((int)entry->aux));break;
    case CULL:scalar_float(cpu,9,rt_cull_aspect(load_float(cpu->x[27]+0x570)));break;
    case FOVY:scalar_float(cpu,reg,rt_fovy(load_float(cpu->x[entry->aux]+0x564)));break;
    case SKYBOX: {
        float values[4];memcpy(values,(const void*)(uintptr_t)(cpu->x[23]+(entry->aux?0x2e0:0x2d0)),16);
        values[0]+=(entry->aux?1.0f:-1.0f)*rt_skysea_margin();memcpy(&cpu->v[reg],values,16);break;
    }
    case INDOOR:scalar_float(cpu,reg,rt_indoor_aspect());break;
    case TILT:rt_tilt_obj(cpu->x[0]);cpu->sp-=0x90;break;
    case FONT:
        if(!cpu->x[0]){cpu->pc=entry->target;return 2;}
        cpu->x[8]=*(const uint8_t*)(uintptr_t)(cpu->x[0]+0x38);break;
    case DELEGATE:
        if(rt_delegate_null(cpu,(int)entry->aux)){cpu->pc=cpu->x[30];return 2;}
        cpu->x[1]=cpu->x[0];break;
    default:FATAL("unknown native LAS compatibility patch\n");
    }
    return 1;
}
