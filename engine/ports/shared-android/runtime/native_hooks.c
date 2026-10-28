/* LAS hook branch choices mirror pinned tools/patches.py HOOKS.
 * Copyright (c) 2026 Peiikai. MIT: native/runtime/LICENSE-LASRecomp. */
#include "native_hooks.h"
#include "native_las_patches.h"
#include "game_addresses.h"
#include "rt.h"
#include "runtime/aot_hooks.h"
#include <stddef.h>

typedef struct NativeHookBranch { size_t site, target; } NativeHookBranch;
/* Only field names belong here. Address selection/versioning stays in the
 * existing LAS adapter, and every selected target must be current module text.
 * All these HOOKS entries currently have exactly one branch selector (2). */
#define LAS_BRANCH(site_,target_) {offsetof(GameAddrs,site_),offsetof(GameAddrs,target_)}
static const NativeHookBranch las_branches[] = {
    LAS_BRANCH(CRASHOUT_SITE,CRASHOUT_SKIP),
    LAS_BRANCH(GOHMA_ARRIVED_SITE,GOHMA_ARRIVED_SKIP),
    LAS_BRANCH(SHADOW_LINK_APPEAR_SITE,SHADOW_LINK_APPEAR_SKIP),
    LAS_BRANCH(ACTOR_EVENT_SITE,ACTOR_EVENT_SKIP),
    LAS_BRANCH(MCHIEF_STORY_SITE,MCHIEF_STORY_SKIP),
    LAS_BRANCH(EMPTY_HANDLER_SITE,EMPTY_HANDLER_CATCH),
    LAS_BRANCH(SHAPE_SIM_SITES_0,SHAPE_SIM_SKIP_0),
    LAS_BRANCH(SHAPE_SIM_SITES_1,SHAPE_SIM_SKIP_1),
};
#undef LAS_BRANCH

int native_hooks_apply(Cpu *cpu,uint64_t pc) {
    if(!aot_instruction_hook_registered(pc)) return 0;
    if(!cpu) FATAL("native instruction hook has no current Cpu\n");
    int patch=native_las_patches_apply(cpu,pc);
    if(patch>=0)return patch;
    int selected=rt_hook(cpu,pc);
    if(selected==0 || selected==1) return selected;
    if(selected==2 && shared_title_las() && g_ga) {
        uint64_t shift=GUEST_SHIFT;
        for(size_t i=0;i<sizeof las_branches/sizeof las_branches[0];i++) {
            const NativeHookBranch *branch=&las_branches[i];
            uint64_t source=*(const uint64_t *)((const unsigned char *)g_ga+branch->site);
            if(source<shift || source-shift!=pc) continue;
            uint64_t target=*(const uint64_t *)((const unsigned char *)g_ga+branch->target);
            if(target<shift || !aot_dispatch_contains_text(target-shift))
                FATAL("native instruction hook branch outside current module text at %llx\n",(unsigned long long)pc);
            cpu->pc=target-shift;
            return 2;
        }
    }
    FATAL("unknown native instruction hook branch %d at %llx\n",selected,(unsigned long long)pc);
    return 0;
}
size_t native_hooks_limit(Cpu *cpu,uint64_t pc,size_t max_words) {
    (void)cpu;
    return aot_instruction_hook_limit(pc,max_words);
}
