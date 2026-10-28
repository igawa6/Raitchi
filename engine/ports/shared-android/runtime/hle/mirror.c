/* Derived from LAS Recomp Android at cca791a3e7218e1dac95e7d4a11b8f97dba3ea51.
 * Copyright (c) 2026 Peiikai. MIT: native/runtime/LICENSE-LASRecomp.
 * Shared runtime: LAS-specific features run only for the selected LAS adapter. */
// Mirror mode (Gameplay tab, g_settings.mirror_mode): the world is shown mirrored left to right, the HUD and menus
// aren't. Each frame the game composites its finished scene into the window image with one unblended full-screen
// draw that samples the scene image (mirror_scene_size), then draws the HUD on top (blended); the renderers (runtime/nvn/renderer.m,
// vulkan/exec.c) mirror that composite (ShaderKey flip_x). While the mirrored world is on screen the stick and d-pad
// swap left and right (runtime/hle/input.c); the pause menu shows no composite (its backdrop is a small blurred copy),
// so there they don't.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "../settings.h"
#include "../common_session.h"

uint64_t renderer_present_count(void);

static uint64_t g_world_frame = ~0ull;  // the mirrored scene was drawn
static uint64_t g_scene_frame = ~0ull;  // the scene composite was drawn (mirrored or not)

// Could an image of sw x sh be the scene, composited into a w x h window image? The game's dynamic resolution rendered
// busy scenes smaller (1536x864, 1280x720 into a 1920x1080 window image: the outdoors, D8; off now - tools/patches.py
// GAME_DYNRES_SITE - but LARECOMP_GAME_DYNRES=1 brings it back), so any size from half the
// window's width up, at its aspect ratio; the scratch draws into the window images sample a quarter of it or less.
// (Requiring the window's own size left those frames unmirrored: the world flipped back and forth, issue #66.)
bool mirror_scene_size(uint32_t sw, uint32_t sh, uint32_t w, uint32_t h) {
    if (!shared_title_las()) return false;
    uint64_t a = (uint64_t)sw * h, b = (uint64_t)sh * w;
    if (sw * 2 >= w && sw <= w && sh <= h && (a > b ? a - b : b - a) * 100 <= (uint64_t)w * h) return true;
    // interiors at the wide render resolution (runtime/hle/widescreen.c): the scene has the window's aspect, not the
    // composite target's 16:9 (frame generation and mirror mode missed their composite there)
    extern float widescreen_factor(void);
    float f = widescreen_factor(), sa = sh ? (float)sw / (float)sh : 0, want = 16.0f / 9.0f * f;
    return f > 1.01f && sh * 2 >= h && sh <= h && fabsf(sa - want) < 0.01f * want;
}

// LARECOMP_LOG_SCENE=<n>: every n presented frames, log the size of the scene image the game composited into the window
// image (guest sizes, as the game allocated them: its main pass resolution) - checks the game's dynamic resolution.
#include <stdlib.h>
#include "../rt.h"
void scene_size_log(bool composite, uint32_t sw, uint32_t sh, uint32_t w, uint32_t h) {
    if (!shared_title_las()) return;
    static long every = -1;
    static uint64_t last = ~0ull;
    if (every < 0) every = getenv("LARECOMP_LOG_SCENE") ? atol(getenv("LARECOMP_LOG_SCENE")) : 0;
    if (every <= 0 || !composite) return;
    uint64_t f = renderer_present_count();
    if (f / every == last) return;
    last = f / every;
    LOG("[scene] frame %llu scene %ux%u window image %ux%u\n", (unsigned long long)f, sw, sh, w, h);
}

// The renderer found the scene composite (composite): mirror it?
bool mirror_draw(bool composite) {
    if (!shared_title_las()) return false;
    if (composite) g_scene_frame = renderer_present_count();
    if (!g_settings.mirror_mode || !composite) return false;
    g_world_frame = renderer_present_count();
    return true;
}

// The world is on screen (its composite drawn this frame or the last three: the render thread can be a frame or two
// behind the game's) - not the pause menu, title or file select. (Frame pacing: runtime/nvn/pace.c.)
bool world_on_screen(void) {
    if (!shared_title_las()) return false;
    uint64_t f = g_scene_frame;
    return f != ~0ull && renderer_present_count() - f <= 3;
}

// The stick / d-pad swap left and right: mirror mode and the mirrored world on screen (this frame or the last two).
bool mirror_input(void) {
    return shared_title_las() && g_settings.mirror_mode && g_world_frame != ~0ull && renderer_present_count() - g_world_frame <= 2;
}


// The in-world action prompt ("Lift", "Read", "Talk"... - Cm_HUD's L_Interact_00) sits at its target's spot on screen.
// tools/patches.py PROMPT_POS_SITE: the HUD writes the prompt pane's translation (x right of the centre) each time it
// places it; in mirror mode, with the mirrored world on screen, x is mirrored - the prompt itself reads as ever.
#include <string.h>
#include "../hle.h"
float rt_prompt_x(uint64_t pane, float x) {
    if (!mirror_input() || memcmp((const char *)(uintptr_t)(pane + 0xB0), "L_Interact_00", 14)) return x;
    return -x;
}
