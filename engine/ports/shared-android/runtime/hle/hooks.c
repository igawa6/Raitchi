/* LAS title adapter, derived from pinned LAS Android hle/hooks.c.
 * Copyright (c) 2026 Peiikai. MIT: native/runtime/LICENSE-LASRecomp. */
// Hook points: instructions of the game's code that call into the runtime (tools/patches.py HOOKS). The generated
// code at a hook site runs `rt_hook(c, site)` first; what a hook does lives here, so changing it rebuilds the app,
// not the game code.
//
// rt_hook returns 0: run the site's own instruction, 1: the hook did it all, 2..: jump to the site's targets
// (HOOKS[site][1], in order).
#include "../game_addresses.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../cpu.h"
#include "../rt.h"
#include "runtime/aot_hooks.h"

void rt_mb_seen(uint64_t o);  // miniboss.c
int rt_shadow_link_appear(uint64_t o);
int rt_shadow_link_bgm(void);
int rt_gohma_free(void);
uint32_t rt_gohma_heading(uint64_t gohma, uint32_t track);
int rt_crashout_skip(uint64_t o);
void rt_dig_hole(Cpu *c);  // digs.c
uint64_t rt_hole_cap(void);
void rt_frame_job_kickoff(uint64_t owner);  // delegate_null.c
void rt_frame_job_finish(uint64_t job);
void rt_owner_call_notify(Cpu *c, uint64_t obj);

// The hook sites (game_addresses.h): which one a site is, by the selected game version's table
#define HOOK_SITES(X) \
    X(MB_SEEN_SITE) \
    X(SHADOW_LINK_APPEAR_SITE) \
    X(SHADOW_LINK_BGM_SITE) \
    X(ACTOR_EVENT_SITE) \
    X(CRASHOUT_SITE) \
    X(GOHMA_HEADING_SITES_0) \
    X(GOHMA_HEADING_SITES_1) \
    X(GOHMA_ARRIVED_SITE) \
    X(MSTALFOS_BODY_SITE) \
    X(MSTALFOS_JUMP_SITE) \
    X(MCHIEF_STORY_SITE) \
    X(DIG_HOLE_SITE) \
    X(HOLE_CAP_SITES_0) \
    X(HOLE_CAP_SITES_1) \
    X(EMPTY_HANDLER_SITE) \
    X(CHOICE_COUNT_SITE) \
    X(FRAME_JOB_KICKOFF_SITES_0) \
    X(FRAME_JOB_KICKOFF_SITES_1) \
    X(FRAME_JOB_KICKOFF_SITES_2) \
    X(FRAME_JOB_KICKOFF_SITES_3) \
    X(OWNER_CALL_NOTIFY_SITE) \
    X(FRAME_JOB_FINISH_SITE) \
    X(SHAPE_SIM_SITES_0) \
    X(SHAPE_SIM_SITES_1)
enum {
#define X(name) H_##name,
    HOOK_SITES(X)
#undef X
};
static const size_t hook_field[] = {
#define X(name) offsetof(GameAddrs, name),
    HOOK_SITES(X)
#undef X
};

static int hook_id(uint64_t site) {
    for (size_t i = 0; i < sizeof hook_field / sizeof hook_field[0]; i++)
        if (*(const uint64_t *)((const char *)g_ga + hook_field[i]) - GUEST_SHIFT == site) return (int)i;
    return -1;
}

static int las_instruction_hook(Cpu *c, uint64_t site) {
    switch (hook_id(site)) {
    case H_MB_SEEN_SITE:  // MB_SEEN_SITE - ldr x8, [x21]: an actor seen
        rt_mb_seen(c->x[21]);
        return 0;
    case H_SHADOW_LINK_APPEAR_SITE:  // SHADOW_LINK_APPEAR_SITE - ldr x9, [x0, #0x230]: his Appear event, none on the Field
        return rt_shadow_link_appear(c->x[0]) ? 2 : 0;
    case H_SHADOW_LINK_BGM_SITE:  // SHADOW_LINK_BGM_SITE - bl main+0xce8ce0: his chamber music, only in the chamber
        return rt_shadow_link_bgm();
    case H_ACTOR_EVENT_SITE: {  // ACTOR_EVENT_SITE - bl main+0x80c290: an actor's event request (quick pickups)
        extern int rt_actor_event(Cpu *c);
        return rt_actor_event(c);
    }
    case H_CRASHOUT_SITE:  // CRASHOUT_SITE - str wzr, [x0, #0xb8]: Blaino's knock-out, no crash-out for copies
        if (!rt_crashout_skip(c->x[0])) return 0;
        ST32(c->x[0] + 0xb8, 0);
        return 2;
    case H_GOHMA_HEADING_SITES_0:  // GOHMA_HEADING_SITES - str w8, [x19, #0x15ec]: heading at Link on the Field
    case H_GOHMA_HEADING_SITES_1:
        c->x[8] = rt_gohma_heading(c->x[19], (uint32_t)c->x[8]);
        return 0;
    case H_GOHMA_ARRIVED_SITE:  // GOHMA_ARRIVED_SITE - ldr q0, [x19, #0x1600]: no track end on the Field
        if (!rt_gohma_free()) return 0;
        c->x[0] = 0;
        return 2;
    case H_MSTALFOS_BODY_SITE:  // MSTALFOS_BODY_SITE - mov w8, #1: body collision off on the Field
        c->x[8] = rt_gohma_free() ? 0 : 1;
        return 1;
    case H_MSTALFOS_JUMP_SITE:  // MSTALFOS_JUMP_SITE - fsub s0, s0, s1: jump arc from his takeoff height on the Field
        if (rt_gohma_free()) {
            float s0;
            memcpy(&s0, &c->v[0], 4);
            s0 += *(const float *)(uintptr_t)(LD64(c->x[19] + 0x1160) + 0x28c);
            __uint128_t r = 0;
            memcpy(&r, &s0, 4);
            c->v[0] = r;
        }
        return 0;
    case H_MCHIEF_STORY_SITE:  // MCHIEF_STORY_SITE - tbnz w8, #7: no story check on the Field
        return rt_gohma_free() ? 2 : 0;
    case H_DIG_HOLE_SITE:  // DIG_HOLE_SITE - ldr x22, [sp, #8]: a hole was dug
        rt_dig_hole(c);
        return 0;
    case H_HOLE_CAP_SITES_0:  // HOLE_CAP_SITES - cmp x8, #0x51 / cmp x8, #0x50: dug-hole limit
        (void)subs64(c, c->x[8], rt_hole_cap());
        return 1;
    case H_HOLE_CAP_SITES_1:
        (void)subs64(c, c->x[8], rt_hole_cap() - 1);
        return 1;
    case H_EMPTY_HANDLER_SITE:  // EMPTY_HANDLER_SITE - bl main+0x108dfc0: an empty std::function would throw (issue #41)
        if (c->x[0]) return 0;
        LOG("[event] empty handler skipped (issue #41)\n");
        return 2;  // (the dispatcher's catch: returns 0)
    case H_CHOICE_COUNT_SITE: {  // CHOICE_COUNT_SITE - str w8, [x19, #0x1cc4]: a question's answer count (issue #183)
        int32_t n = (int32_t)c->x[8];
        if (n >= 2 && n <= 4) return 0;
        FATAL("a question's answer count is %d: the game's text doesn't fit its language (issue #183)\n", n);
        return 0;
    }
    case H_FRAME_JOB_KICKOFF_SITES_0:  // FRAME_JOB_KICKOFF_SITES - a frame job's rebuild waits for its last finish (#19)
    case H_FRAME_JOB_KICKOFF_SITES_1:
    case H_FRAME_JOB_KICKOFF_SITES_2:
        rt_frame_job_kickoff(c->x[22]);
        return 0;
    case H_FRAME_JOB_KICKOFF_SITES_3:
        rt_frame_job_kickoff(c->x[21]);
        return 0;
    case H_OWNER_CALL_NOTIFY_SITE:  // OWNER_CALL_NOTIFY_SITE - add x0, x21, #0x38: notify only once the caller waits
        rt_owner_call_notify(c, c->x[21]);
        return 0;
    case H_FRAME_JOB_FINISH_SITE:  // FRAME_JOB_FINISH_SITE - ldr x8, [x0]: a frame job's finish (debug delay)
        rt_frame_job_finish(c->x[0]);
        return 0;
    case H_SHAPE_SIM_SITES_0:  // SHAPE_SIM_SITES - a PhysX shape change: the shape's simulation object (#314)
    case H_SHAPE_SIM_SITES_1: {
        if (c->x[0] != 1) return 0;  // (PhysX's getSimForShape: 1 = the simulation holds no object for the shape)
        static int logged;
        if (logged++ < 8) {  // the shape, and who changed it (frame records from x29)
            char where[160] = "";
            int n = 0;
            uint64_t fp = c->x[29];
            for (int i = 0; i < 6 && n < (int)sizeof where - 24 && fp >= c->sp && fp < c->sp + 0x100000 && !(fp & 7); i++) {
                n += snprintf(where + n, sizeof where - n, " %#llx", (unsigned long long)LD64(fp + 8));
                fp = LD64(fp);
            }
            LOG("[physx] shape %#llx changed (flags %#x) without a simulation object: update skipped (issue #314), from%s\n",
                (unsigned long long)c->x[1], site == GA(SHAPE_SIM_SITES_0) ? (unsigned)LD32(c->x[19]) : 0u, where);
        }
        return 2;
    }
    }
    return 0;
}

void shared_las_hooks_register(void) {
    if (!shared_title_las()) return;
    for (size_t i = 0; i < sizeof hook_field / sizeof hook_field[0]; ++i) {
        uint64_t canonical = *(const uint64_t *)((const char *)g_ga + hook_field[i]);
        uint64_t actual = GUEST(canonical);
        if (rt_register_instruction_hook(actual, las_instruction_hook) != AOT_OK)
            FATAL("LAS instruction site outside generated text: %llx\n", actual);
    }
}
