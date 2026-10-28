/* Derived from LAS Recomp Android at cca791a3e7218e1dac95e7d4a11b8f97dba3ea51.
 * Copyright (c) 2026 Peiikai. MIT: native/runtime/LICENSE-LASRecomp.
 * Shared runtime: LAS-specific features run only for the selected LAS adapter.
 * Touch/virtual gyro derived from Let's Go Recomp at
 * 96894fd8e9ea20c1b38cdea53105ec2a335df620, Copyright (c) 2026 raulzito12345rrs.
 * MIT: native/runtime/LICENSE-LetsGo. Physical device motion is not sampled. */
// Controller input: SDL keyboard/gamepad -> nn::hid Npad state.
//
// Both go through the player's bindings (runtime/hle/bindings.c, defaults there: keyboard WASD left stick, arrows
// d-pad, K/J/L/I = A/B/X/Y, Q/E = L/R, 1/3 = ZL/ZR, Enter / Backspace = Plus / Minus, numpad 8/4/5/6 right stick;
// controller by position, Switch A = east).
#include <SDL3/SDL.h>
#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <pthread.h>
#include "npad_history.h"
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../common_session.h"
#include "../nvn/nvn.h"
#include "../nvn/renderer.h"
#include "../settings.h"
#include "../input_bind.h"
#include "../rumble.h"
#include "../led.h"
#include "../ui/menu.h"
extern bool menu_input_captured(void);

enum {
    BTN_A = 0, BTN_B, BTN_X, BTN_Y, BTN_LSTICK, BTN_RSTICK, BTN_L, BTN_R, BTN_ZL, BTN_ZR, BTN_PLUS, BTN_MINUS,
    BTN_LEFT, BTN_UP, BTN_RIGHT, BTN_DOWN
};

_Static_assert(BTN_DOWN == BIND_DOWN && BTN_ZL == BIND_ZL, "bindings follow the button bits");

static _Atomic uint64_t key_buttons, pad_buttons, mouse_buttons;
static _Atomic int32_t key_lx, key_ly, key_rx, key_ry, pad_lx, pad_ly, pad_rx, pad_ry;
static uint8_t keys_down[SDL_SCANCODE_COUNT];
static SDL_Gamepad *pad;
static _Atomic uint64_t sampling;
static pthread_mutex_t npad_history_lock = PTHREAD_MUTEX_INITIALIZER;
static NpadHistoryBank npad_history_bank;
static uint64_t npad_history_epoch;
static void npad_forget_history(bool reboot) {
    pthread_mutex_lock(&npad_history_lock);
    npad_history_reset(&npad_history_bank);
    ++npad_history_epoch;
    if (reboot) atomic_store(&sampling, 0); // reset only after guest threads stop
    pthread_mutex_unlock(&npad_history_lock);
}
static bool host_test;  // scripted runs must not vibrate the user's controller

// Guest IPC threads publish values; only the UI thread touches SDL's pad.
// The lock also makes paired value snapshots consistent with permission/reset.
static SDL_SpinLock rumble_lock;
static RumbleValue rumble_values[2];
static bool rumble_allowed = true;
static _Atomic uint64_t rumble_frame_ms;
static uint16_t rumble_last_lo, rumble_last_hi;
static uint64_t rumble_last_ms;
static bool rumble_warned;

static void rumble_clear(void) {
    SDL_LockSpinlock(&rumble_lock);
    memset(rumble_values, 0, sizeof rumble_values);
    SDL_UnlockSpinlock(&rumble_lock);
}
void input_rumble_send(uint32_t h, const RumbleValue *v) {
    int side = rumble_side(h);
    SDL_LockSpinlock(&rumble_lock);
    if (side < 0) { SDL_UnlockSpinlock(&rumble_lock); return; }
    if (rumble_allowed) rumble_values[side] = *v;
    SDL_UnlockSpinlock(&rumble_lock);
}
RumbleValue input_rumble_get(uint32_t h) {
    RumbleValue v = {0, 160, 0, 320};
    int side = rumble_side(h);
    SDL_LockSpinlock(&rumble_lock);
    if (side >= 0 && rumble_allowed) v = rumble_values[side];
    SDL_UnlockSpinlock(&rumble_lock);
    return v;  // requested value, not physical motor feedback
}
void input_rumble_permit(bool allowed) {
    SDL_LockSpinlock(&rumble_lock);
    rumble_allowed = allowed;
    if (!allowed) memset(rumble_values, 0, sizeof rumble_values);
    SDL_UnlockSpinlock(&rumble_lock);
}
bool input_rumble_permitted(void) {
    SDL_LockSpinlock(&rumble_lock);
    bool allowed = rumble_allowed;
    SDL_UnlockSpinlock(&rumble_lock);
    return allowed;
}
void input_rumble_poll(void) {
    uint64_t now = SDL_GetTicks();
    uint64_t last_frame = atomic_load(&rumble_frame_ms);
    uint16_t lo = 0, hi = 0;
    // Switch values are latched until changed: they contain no duration.
    // Renew SDL's 250 ms effect every 100 ms while frames are progressing.
    // A stalled render/UI thread therefore cannot leave an infinite effect.
    // This watchdog is a host policy, not a duration supplied by the game.
    bool suppress = (now > last_frame && now - last_frame > 250) || !pad || host_test || !g_settings.rumble || menu_is_open() ||
                    !SDL_GetKeyboardFocus();
    SDL_LockSpinlock(&rumble_lock);
    if (suppress) memset(rumble_values, 0, sizeof rumble_values);
    else if (rumble_allowed) {
        rumble_mix(rumble_values, &lo, &hi);
        int pct = g_settings.rumble_strength;  // (Controls tab: Intensity)
        if (pct > 0 && pct < 100) lo = (uint16_t)(lo * pct / 100), hi = (uint16_t)(hi * pct / 100);
    }
    SDL_UnlockSpinlock(&rumble_lock);
    if (pad && (lo != rumble_last_lo || hi != rumble_last_hi ||
                ((lo || hi) && now - rumble_last_ms >= 100))) {
        if (!SDL_RumbleGamepad(pad, lo, hi, (lo || hi) ? 250 : 0) && !rumble_warned) {
            LOG("[rumble] unavailable: %s\n", SDL_GetError());
            rumble_warned = true;  // keyboard/non-rumble pads are still usable
        }
        rumble_last_ms = now;
    }
    rumble_last_lo = lo; rumble_last_hi = hi;
}
void input_rumble_stop(void) {  // UI thread: also called before normal app exit
    rumble_clear();
    atomic_store(&rumble_frame_ms, 0);
    if (pad) SDL_RumbleGamepad(pad, 0, 0, 0);
    rumble_last_lo = rumble_last_hi = 0;
}
RT_ON_RESET(input_rumble_reset) {
    input_rumble_stop();
    input_rumble_permit(true);
}

// Controller light (runtime/led.h): the light bar of the game's controller shows Link's health. UI thread only.
// While the option is off (or nothing is in play) SDL_SetGamepadLED is never called: the one call that hands the
// light back is made when we stop owning it.
int game_in_play(void);                // hle/warp.c
int cheats_link_health(int *max);      // ui/cheats.mm
static SDL_JoystickID led_pad_id;      // the pad the state below is about
static bool led_capable, led_lit;      // it has an RGB light / we set its colour
static LedColor led_last;
static uint64_t led_last_ms, led_log_ms, led_hurt_ms;
static int led_prev_hp = -1, led_prev_max;

static void led_forget(void) { led_lit = false, led_prev_hp = -1, led_pad_id = 0; }
static void led_restore(const char *why) {
    if (led_lit && pad) {
        LedColor c = led_default_color(SDL_GetGamepadType(pad) == SDL_GAMEPAD_TYPE_PS4, SDL_GetGamepadPlayerIndex(pad));
        SDL_SetGamepadLED(pad, c.r, c.g, c.b);
        LOG("[led] %s: controller colour restored\n", why);
    }
    led_lit = false, led_prev_hp = -1;
}
void input_led_stop(void) { led_restore("quit"); }  // before exit / teardown
static void led_atexit(void) { led_restore("exit"); }
RT_ON_RESET(input_led_reset) { led_restore("reset"); }

void input_led_poll(void) {
    if (!shared_title_las()) { led_restore("title adapter inactive"); return; }
    if (!pad || host_test) { led_forget(); return; }
    SDL_JoystickID id = SDL_GetGamepadID(pad);
    if (id != led_pad_id) {
        led_forget();
        led_pad_id = id;
        led_capable = SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad), SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false);
        if (led_capable) LOG("[led] %s has a light\n", SDL_GetGamepadName(pad));
    }
    if (!led_capable) return;
    if (!g_settings.controller_light) { led_restore("off"); return; }
    int max = 0, hp = game_in_play() ? cheats_link_health(&max) : -1;
    if (hp < 0 || max <= 0) { led_restore("not in play"); return; }
    uint64_t now = SDL_GetTicks();
    if (led_prev_hp >= 0 && hp < led_prev_hp && max == led_prev_max) led_hurt_ms = now;
    if (led_prev_hp < 0) led_hurt_ms = 0;  // (entering play: no flash)
    led_prev_hp = hp, led_prev_max = max;
    LedColor c = led_health_color(hp, max, now, led_hurt_ms ? now - led_hurt_ms : UINT64_MAX);
    if (led_lit && (led_equal(c, led_last) || now - led_last_ms < LED_MIN_INTERVAL_MS)) return;
    if (!SDL_SetGamepadLED(pad, c.r, c.g, c.b)) {
        LOG("[led] unavailable: %s\n", SDL_GetError());
        led_capable = false;
        return;
    }
    static bool registered;
    if (!registered) registered = true, atexit(led_atexit);
    if (!led_lit || now - led_log_ms >= 1000) {  // (the pulse changes it all the time: one line a second at most)
        LOG("[led] %d,%d,%d (health %d/%d quarter-hearts%s)\n", c.r, c.g, c.b, hp, max,
            led_hurt_ms && now - led_hurt_ms < LED_FLASH_MS ? ", hurt" : "");
        led_log_ms = now;
    }
    led_lit = true, led_last = c, led_last_ms = now;
}

// Host keys / buttons held by a test script (LARECOMP_HOST_INPUT, input_frame): they go through the bindings
static uint8_t test_keys[SDL_SCANCODE_COUNT];
static _Atomic uint32_t test_pad;
static _Atomic int32_t test_lx, test_ly;  // (a resting stick's drift: "lstick" in LARECOMP_HOST_INPUT)

static int held_key(int sc) { return sc > 0 && sc < SDL_SCANCODE_COUNT && (keys_down[sc] || test_keys[sc]); }

// Through the bindings (runtime/hle/bindings.c: g_bind_key), read on every key event, so a rebind applies at once.
static void recompute_keyboard(void) {
    uint64_t b = 0;
    for (int i = 0; i < BIND_PAD_COUNT; i++)
        if (held_key(g_bind_key[i])) b |= 1ull << i;
    key_buttons = b;
    key_lx = (held_key(g_bind_key[BIND_LS_RIGHT]) - held_key(g_bind_key[BIND_LS_LEFT])) * 32767;
    key_ly = (held_key(g_bind_key[BIND_LS_UP]) - held_key(g_bind_key[BIND_LS_DOWN])) * 32767;
    key_rx = (held_key(g_bind_key[BIND_RS_RIGHT]) - held_key(g_bind_key[BIND_RS_LEFT])) * 32767;
    key_ry = (held_key(g_bind_key[BIND_RS_UP]) - held_key(g_bind_key[BIND_RS_DOWN])) * 32767;
}

// Controller buttons (and the triggers as buttons) -> Switch buttons through g_bind_pad; the sticks stay the sticks.
static void recompute_pad(void) {
    uint32_t host = test_pad;
    if (pad && !host_test) {
        for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
            if (SDL_GetGamepadButton(pad, (SDL_GamepadButton)i)) host |= 1u << i;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) host |= 1u << PADB_LTRIGGER;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) host |= 1u << PADB_RTRIGGER;
    }
    uint64_t b = 0;
    for (int i = 0; i < BIND_PAD_COUNT; i++)
        if (g_bind_pad[i] >= 0 && g_bind_pad[i] < PADB_HOST_COUNT && (host >> g_bind_pad[i] & 1)) b |= 1ull << i;
    pad_buttons = b;
    if (!pad || host_test) { pad_lx = test_lx, pad_ly = test_ly, pad_rx = pad_ry = 0; return; }
    pad_lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
    pad_ly = -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    pad_rx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
    pad_ry = -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
}

bool input_gamepad_connected(void) { return pad != NULL; }
// touch screens: runtime/ui/touch_controls.mm
bool touch_handle_event(const SDL_Event *e);
uint64_t touch_buttons(void);
void touch_stick(int32_t *lx, int32_t *ly);
void touch_pad_used(void);
// Every press reaches the game, however short. recompute_pad reads the pad as it is now, so a quick tap whose press
// and release SDL hands over together (a Bluetooth pad's reports often come batched), or that fell between two of the
// game's polls, never showed: the game's menus took a second press now and then. A press the game hasn't polled yet
// that is released is still given to the game's next polls (fill_state). UI thread: note_press; game thread: the rest.
static _Atomic uint64_t press_unseen;  // pressed (events), not yet in a state the game polled
static _Atomic uint64_t tap_buttons;   // released while unseen: the game's next poll gets them
static uint64_t pad_bits(int host) {   // the game buttons bound to host button `host` (SDL button / PADB_*TRIGGER)
    uint64_t b = 0;
    for (int i = 0; i < BIND_PAD_COUNT; i++)
        if (g_bind_pad[i] == host) b |= 1ull << i;
    return b;
}
static void note_press(uint64_t down, uint64_t up) {
    if (down) press_unseen |= down;
    if (up) {
        uint64_t u = atomic_fetch_and(&press_unseen, ~up) & up;
        if (u) tap_buttons |= u;
    }
}

// The mouse: held by the game window (relative mode) or free
static SDL_Window *mouse_win;
static _Atomic int gyro_aiming;  // the game has the motion sensor started (aiming): the mouse turns the gyro
static _Atomic int64_t mouse_acc_x, mouse_acc_y;  // motion not yet applied to the virtual gyro, in 1/1000 pixel
static _Atomic int touch_down;
static SDL_FingerID raw_finger;
static bool raw_finger_held;
static void mouse_release(void) {
    if (mouse_win) SDL_SetWindowRelativeMouseMode(mouse_win, false);
    mouse_win = NULL;
    mouse_buttons = 0;
    touch_down = 0;
    raw_finger_held = false;
}

// Touch screen: the free mouse over the game image. The game is told it is docked (runtime/services/am.c: a 1920 x
// 1080 display, wider with LARECOMP_DISPLAY_W) and takes touches in that display's pixels - not the handheld panel's
// 1280 x 720: in those, the berry button sat 2/3 of the way to where it was drawn and never opened.
// g_present_rect: where the image is drawn in the window, in pixels (runtime/nvn/vulkan/device.c).
static double g_present_rect[4] = {0, 0, 1280, 720};
static SDL_SpinLock present_rect_lock;
void input_set_present_rect(double x, double y, double w, double h) {
    SDL_LockSpinlock(&present_rect_lock);
    g_present_rect[0] = x; g_present_rect[1] = y;
    g_present_rect[2] = w; g_present_rect[3] = h;
    SDL_UnlockSpinlock(&present_rect_lock);
}
static void input_present_rect(double rect[4]) {
    SDL_LockSpinlock(&present_rect_lock);
    memcpy(rect, g_present_rect, sizeof g_present_rect);
    SDL_UnlockSpinlock(&present_rect_lock);
}
static double touch_w(void) {
    static double w = 0;
    if (!w) w = getenv("LARECOMP_DISPLAY_W") ? atoi(getenv("LARECOMP_DISPLAY_W")) : 1920;
    return w > 0 ? w : 1920;
}
#define TOUCH_H 1080.0
static _Atomic int32_t touch_x = 960, touch_y = 540;
static void touch_at(SDL_Window *w, float mx, float my) {
    float d = w ? SDL_GetWindowPixelDensity(w) : 1;
    if (d <= 0) d = 1;
    double rect[4]; input_present_rect(rect);
    double r0 = rect[0], r1 = rect[1], r2 = rect[2], r3 = rect[3];
    if (r2 <= 0 || r3 <= 0) return;
    double x = (mx * d - r0) / r2 * touch_w(), y = (my * d - r1) / r3 * TOUCH_H;
    touch_x = (int32_t)(x < 0 ? 0 : x > touch_w() - 1 ? touch_w() - 1 : x);
    touch_y = (int32_t)(y < 0 ? 0 : y > TOUCH_H - 1 ? TOUCH_H - 1 : y);
}


// Forget held keys (the menu took focus); the gamepad is re-read on its next event.
void input_reset(void) {
    press_unseen = 0, tap_buttons = 0;
    rumble_clear();
    mouse_release();
    memset(keys_down, 0, sizeof keys_down);
    recompute_keyboard();
    // Invalidate after clearing sources: an in-flight old capture is rejected,
    // and a poll starting after this epoch can only see cleared reset inputs.
    npad_forget_history(false);
}

volatile int g_debug_frame_request;  // F9: dump the next frame's passes (build/pass_*.png) + draw log

// Events a test pushes (LARECOMP_HOST_INPUT type: / sdlkey: / sdlpad: / mouse:) carry this device id: they go the whole way, menu first.
#define TEST_DEVICE 0x7E57u
static bool test_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: return e->key.which == TEST_DEVICE;
    case SDL_EVENT_TEXT_INPUT: return e->text.windowID == TEST_DEVICE;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: return e->gbutton.which == TEST_DEVICE;
    case SDL_EVENT_MOUSE_MOTION: return e->motion.which == TEST_DEVICE;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: return e->button.which == TEST_DEVICE;
    default: return false;
    }
}

// Scripted test runs ignore the user's own input (a key or stick touched while a test drives Link spoils it):
// LARECOMP_NO_USER_INPUT=1, on by default whenever LARECOMP_INPUT / LARECOMP_DOORJUMP_TEST / LARECOMP_PLACE is set
// (or LARECOMP_HOST_INPUT / LARECOMP_BIND_TEST; LARECOMP_NO_USER_INPUT=0 lets it through again).
bool input_user_blocked(const SDL_Event *e) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("LARECOMP_NO_USER_INPUT");
        on = v ? atoi(v) != 0 : (getenv("LARECOMP_INPUT") || getenv("LARECOMP_DOORJUMP_TEST") || getenv("LARECOMP_PLACE") ||
                                 getenv("LARECOMP_HOST_INPUT") || getenv("LARECOMP_BIND_TEST"));
        if (on) LOG("[input] scripted run: keyboard, mouse and controllers ignored\n");
    }
    if (!on) return false;
    if (test_event(e)) return false;  // (LARECOMP_HOST_INPUT type: / sdlkey: / sdlpad: - the test's own presses)
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_GAMEPAD_ADDED: case SDL_EVENT_GAMEPAD_AXIS_MOTION: case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: case SDL_EVENT_JOYSTICK_ADDED: case SDL_EVENT_JOYSTICK_AXIS_MOTION:
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN: case SDL_EVENT_JOYSTICK_BUTTON_UP:
        return true;
    default:
        return false;
    }
}

// The mouse cursor during play: hidden as soon as a controller (a button, a stick or trigger past a small deadzone) or
// a key is used, shown again only when the mouse moves (a few points, so a bumped desk doesn't count), clicks or
// scrolls. Every window mode and platform: SDL hides it over our window only. Main thread (the event loops: the
// game's ui_run and the start screen).
static bool cursor_hidden;  // (the start screen keeps ImGui's backend from showing it again meanwhile)
bool input_cursor_hidden(void) { return cursor_hidden; }
void input_cursor_event(const SDL_Event *e) {
    static float moved;  // (points the mouse moved since the cursor was hidden)
    bool hide = false, show = false;
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:  // (a modifier alone - Cmd / Alt for switching apps - isn't play)
        hide = !e->key.repeat && !(e->key.scancode >= SDL_SCANCODE_LCTRL && e->key.scancode <= SDL_SCANCODE_RGUI);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: hide = true; break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: hide = abs(e->gaxis.value) > 12000; break;  // (stick drift stays below)
    case SDL_EVENT_MOUSE_MOTION:
        moved += fabsf(e->motion.xrel) + fabsf(e->motion.yrel);
        show = moved > 6;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_WHEEL: show = true; break;
    default: break;
    }
    if (hide && !cursor_hidden) {
        SDL_HideCursor();
        cursor_hidden = true, moved = 0;
        LOG("[cursor] hidden (%s): SDL cursor visible=%d\n", e->type == SDL_EVENT_KEY_DOWN ? "key" : "controller",
            SDL_CursorVisible());
    } else if (show && cursor_hidden) {
        SDL_ShowCursor();
        cursor_hidden = false;
        LOG("[cursor] shown (mouse %s): SDL cursor visible=%d\n",
            e->type == SDL_EVENT_MOUSE_MOTION ? "moved" : e->type == SDL_EVENT_MOUSE_WHEEL ? "wheel" : "button",
            SDL_CursorVisible());
    }
}

// "Allow background inputs" (Controls tab, off by default): controllers keep working while the window isn't in front.
// SDL drops controller events without keyboard focus unless this hint is set; it reads the hint live. (On macOS the
// hint makes the system ask for Input Monitoring - reading HID input for a background app - so it was taken out as a
// default there; the player opts in here.)
SDL_JoystickID input_active_pad(void) { return pad ? SDL_GetGamepadID(pad) : 0; }
void input_apply_background(void) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, g_settings.bg_input ? "1" : "0");
}

// A controller SDL has no mapping for (on macOS e.g. a generic Bluetooth HID pad, such as a phone app posing as one)
// is only a joystick to SDL, and the game reads gamepads only. gamecontrollerdb.txt in the data root holds the
// player's own mappings, in SDL's format (lines for other platforms are skipped, so SDL_GameControllerDB's file works
// as is). Called right after SDL_Init: a pad already connected becomes a gamepad then (SDL sends GAMEPAD_ADDED).
void input_load_user_mappings(void) {
    if (!SDL_GetPathInfo("gamecontrollerdb.txt", NULL)) return;
    int n = SDL_AddGamepadMappingsFromFile("gamecontrollerdb.txt");
    if (n < 0) LOG("[input] gamecontrollerdb.txt: %s\n", SDL_GetError());
    else LOG("[input] gamecontrollerdb.txt: %d controller mapping(s) added\n", n);
}

// Controller family for the button prompts (runtime/nvn/texpatch.c) and our menus' layout: 0 Nintendo / none, 1 Xbox
// (and every other pad SDL labels like one: generic / third-party pads with A on the bottom), 2 PlayStation.
// LARECOMP_PAD_TYPE=xbox|playstation|nintendo pretends one is attached (tests).
// By SDL's own face-button labels for the type: the bottom button reads B on Nintendo pads, A on Xbox-style ones,
// Cross on PlayStation ones.
int input_family_of_type(int type) {
    switch (SDL_GetGamepadButtonLabelForType((SDL_GamepadType)type, SDL_GAMEPAD_BUTTON_SOUTH)) {
    case SDL_GAMEPAD_BUTTON_LABEL_B: return 0;
    case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return 2;
    default: return 1;
    }
}
static _Atomic int pad_family;
static void note_family(void) {
    int f = pad ? input_family_of_type(SDL_GetGamepadType(pad)) : 0;
    if (f != pad_family) LOG("[input] button prompts: %s controller\n", f == 1 ? "Xbox" : f == 2 ? "PlayStation" : "Nintendo");
    pad_family = f;
}
int input_pad_family(void) {
    static int env = -2;
    if (env == -2) {
        const char *e = getenv("LARECOMP_PAD_TYPE");
        env = !e ? -1 : !strcmp(e, "xbox") ? 1 : !strcmp(e, "playstation") || !strcmp(e, "ps") ? 2 : 0;
    }
    return env >= 0 ? env : pad_family;
}

// Our menus' face buttons: the family whose button names they show, and (Confirm button = Controller convention, #172)
// whose layout they use - Nintendo (0): the right button confirms, the bottom one goes back; Xbox / PlayStation
// (1 / 2): the bottom one confirms, the right one goes back (runtime/ui/pad_nav.mm). "Button icons"
// forced to a style wins (icons and menus agree); Auto: the controller `which` that pressed (0, or not a connected
// gamepad - a test's pushed press: the game's controller / the first connected one, else Nintendo).
int input_menu_family(SDL_JoystickID which) {
    switch (g_settings.prompts) {
    case PROMPTS_NINTENDO: return 0;
    case PROMPTS_XBOX: return 1;
    case PROMPTS_PLAYSTATION: return 2;
    default: break;
    }
    if (getenv("LARECOMP_PAD_TYPE")) return input_pad_family();
    if (which && SDL_IsGamepad(which)) return input_family_of_type(SDL_GetGamepadTypeForID(which));
    return bind_label_family();
}

// Which connected controller the game reads. Android lists other input devices as gamepads too (a wireless keyboard /
// mouse receiver's media keys: "Logitech USB Receiver Mouse"), and the first one listed used to be taken for good: a
// DualSense connected beside it was never read. Now a pad SDL knows (PlayStation, Xbox, Switch...) beats one with sticks
// and face buttons, which beats anything else; and the controller whose button is pressed becomes the one read.
static int pad_rank(SDL_JoystickID id) {
    SDL_GamepadType t = SDL_GetGamepadTypeForID(id);
    if (t != SDL_GAMEPAD_TYPE_UNKNOWN && t != SDL_GAMEPAD_TYPE_STANDARD) return 3;
    char *m = SDL_GetGamepadMappingForID(id);
    int r = m && strstr(m, "leftx:") && strstr(m, ",a:") && strstr(m, ",b:") ? 2 : 1;
    SDL_free(m);
    return r;
}
static void use_pad(SDL_JoystickID id, const char *why) {
    if (pad && SDL_GetGamepadID(pad) == id) return;
    SDL_Gamepad *p = SDL_OpenGamepad(id);
    if (!p) { LOG("[input] gamepad %u: %s\n", (unsigned)id, SDL_GetError()); return; }
    if (pad) {
        SDL_RumbleGamepad(pad, 0, 0, 0);
        led_restore("switched");
        SDL_CloseGamepad(pad);
    }
    pad = p;
    led_forget();
    rumble_last_lo = rumble_last_hi = 0, rumble_warned = false;
    LOG("[input] gamepad: %s (%s)\n", SDL_GetGamepadName(pad), why);
    note_family();
}
static void pick_pad(const char *why) {  // the best connected one, unless the one read is as good
    int n = 0, best = -1, cur = pad ? pad_rank(SDL_GetGamepadID(pad)) : 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n), pick = 0;
    for (int i = 0; i < n; i++) {
        int r = pad_rank(ids[i]);
        LOG("[input] connected: %s (rank %d)\n", SDL_GetGamepadNameForID(ids[i]), r);
        if (r > best) best = r, pick = ids[i];
    }
    SDL_free(ids);
    if (pick && best > cur) use_pad(pick, why);
}

void input_handle_event(const SDL_Event *e) {
    static bool scanned;  // pads present at startup: their ADDED event may have gone to the start screen's loop
    if (!scanned) {
        scanned = true;
        pick_pad("connected");
    }
    if (touch_handle_event(e)) return;
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
        if (e->type == SDL_EVENT_KEY_DOWN && e->key.scancode == SDL_SCANCODE_F9 && !e->key.repeat) g_debug_frame_request = 1;
        if (e->key.scancode < SDL_SCANCODE_COUNT) keys_down[e->key.scancode] = e->type == SDL_EVENT_KEY_DOWN;
        uint64_t before = key_buttons;
        recompute_keyboard();
        uint64_t now = key_buttons;
        note_press(now & ~before, before & ~now);
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: case SDL_EVENT_MOUSE_MOTION: {
        if (shared_title_las()) break;
        if ((e->type == SDL_EVENT_MOUSE_MOTION ? e->motion.which : e->button.which) == SDL_TOUCH_MOUSEID) break;
        bool motion = e->type == SDL_EVENT_MOUSE_MOTION;
        SDL_Window *w = SDL_GetWindowFromID(motion ? e->motion.windowID : e->button.windowID);
        // aiming: the window holds the mouse (the gyro); else it is free (the touch screen)
        if (gyro_aiming && !mouse_win && w) {
            mouse_win = w;
            SDL_SetWindowRelativeMouseMode(w, true);
            touch_down = 0;
        } else if (!gyro_aiming && mouse_win) {
            mouse_release();
        }
        if (motion) {
            if (mouse_win) {
                mouse_acc_x += (int64_t)(e->motion.xrel * 1000);
                mouse_acc_y += (int64_t)(e->motion.yrel * 1000);
            } else {
                touch_at(w, e->motion.x, e->motion.y);
            }
            break;
        }
        bool down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        if (e->button.button == SDL_BUTTON_RIGHT) {
            mouse_buttons = down ? mouse_buttons | 1ull << BTN_B : mouse_buttons & ~(1ull << BTN_B);
        } else if (e->button.button == SDL_BUTTON_LEFT) {
            if (mouse_win) mouse_buttons = down ? mouse_buttons | 1ull << BTN_A : mouse_buttons & ~(1ull << BTN_A);
            else { touch_at(w, e->button.x, e->button.y); touch_down = down; }
        }
        break;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        mouse_release();
        break;
    case SDL_EVENT_FINGER_DOWN: case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP: case SDL_EVENT_FINGER_CANCELED: {
        if (shared_title_las() || menu_input_captured()) break;
        bool down = e->type == SDL_EVENT_FINGER_DOWN;
        if (down && !raw_finger_held) raw_finger = e->tfinger.fingerID, raw_finger_held = true;
        if (!raw_finger_held || raw_finger != e->tfinger.fingerID) break;
        SDL_Window *w = SDL_GetWindowFromID(e->tfinger.windowID);
        int width = 0, height = 0;
        if (w) SDL_GetWindowSize(w, &width, &height);
        if (width > 0 && height > 0) touch_at(w, e->tfinger.x * width, e->tfinger.y * height);
        if (e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_FINGER_CANCELED)
            touch_down = 0, raw_finger_held = false;
        else touch_down = 1;
        break;
    }
    case SDL_EVENT_GAMEPAD_ADDED:
        pick_pad("connected");
        if (pad && SDL_GetGamepadID(pad) == e->gdevice.which) touch_pad_used();  // (the touch controls hide: touch_controls.mm)
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (pad && SDL_GetGamepadID(pad) == e->gdevice.which) {
            SDL_RumbleGamepad(pad, 0, 0, 0);
            SDL_CloseGamepad(pad); pad = NULL; note_family();
            led_forget();  // (gone: it shows its own colour when it comes back)
            rumble_clear(); rumble_last_lo = rumble_last_hi = 0; rumble_warned = false;
            pick_pad("another one left");  // (a controller still connected takes over)
        }
        recompute_pad();
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && !host_test && e->gbutton.which != TEST_DEVICE) {
            use_pad(e->gbutton.which, "pressed");  // (the controller in use is the one read)
            touch_pad_used();
        }
        if (!host_test || e->gbutton.which == TEST_DEVICE) {  // (the press as the event has it, before reading the pad)
            uint64_t b = pad_bits(e->gbutton.button);
            note_press(e->gbutton.down ? b : 0, e->gbutton.down ? 0 : b);
        }
        recompute_pad();
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (!host_test && abs(e->gaxis.value) > 20000 && (!pad || SDL_GetGamepadID(pad) != e->gaxis.which) &&
            pad_rank(e->gaxis.which) >= (pad ? pad_rank(SDL_GetGamepadID(pad)) : 1))
            use_pad(e->gaxis.which, "stick moved");  // (a stick pushed well off centre, on a pad at least as good)
        if (!host_test && pad && SDL_GetGamepadID(pad) != e->gaxis.which) break;  // (another device's axes: not the game's)
        if ((e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) &&
            !host_test) {  // (the triggers as buttons: past half way)
            static bool trig[2];
            int t = e->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
            bool on = e->gaxis.value > 16000;
            if (on != trig[t]) {
                uint64_t b = pad_bits(PADB_LTRIGGER + t);
                note_press(on ? b : 0, on ? 0 : b);
                trig[t] = on;
            }
        }
        recompute_pad();
        // the controller in use (a stick or trigger well off centre - not drift; a press: above): the touch controls hide
        if (abs(e->gaxis.value) > 12000) touch_pad_used();
        break;
    }
}

// Scripted input for testing: LARECOMP_INPUT="frame:BUTTON:frames,..." (BUTTON = A,B,X,Y,L,R,ZL,ZR,PLUS,
// MINUS,LEFT,UP,RIGHT,DOWN, or STICK_UP/DOWN/LEFT/RIGHT, RSTICK_UP/DOWN/LEFT/RIGHT, STICK_<degrees>: full tilt at
// that angle, 0 = right, 90 = up).
static _Atomic uint64_t script_buttons;
static _Atomic int32_t script_lx, script_ly, script_rx, script_ry;
// (touch_controls.mm, LARECOMP_TOUCH_SHOW_INPUT: the scripted presses drawn on the on-screen controls)
void input_script_state(uint64_t *buttons, int32_t *lx, int32_t *ly) { *buttons = script_buttons, *lx = script_lx, *ly = script_ly; }

// Tests of the bindings, through them like a real player:
// LARECOMP_HOST_INPUT="frame:key:<SDL key name>:frames,frame:pad:<button>:frames,..." holds a host key / controller
//   button (names as in larecomp.ini's bind_* lines: "Z", "Keypad 0", "south", "ltrigger"...);
//   "frame:lstick:<x>/<y>:frames" holds the controller's left stick there (raw SDL units, y up: a drifting stick).
// "frame:type:<text>" types <text> as real key events (down, text, up per character), through the overlay menu / name
//   entry first like a player's typing; "{Escape}" (any SDL key name) presses that key. No ',' or ':' in <text>.
// "frame:sdlpad:<button>:frames" / "frame:sdlkey:<key>:frames" hold a controller button / key as real events (down
//   at frame, up `frames` later): they reach menus and the on-screen keyboard first, as a player's would.
// "frame:mouse:<dx>/<dy>" moves the mouse by that many points, "frame:mouse:click" clicks it (real events).
// "frame:shot:<name>" saves the window (game + menus) to build/<name>.png.
// "frame:resize:<W>x<H>" resizes the window to W x H points (menu_apply_window_requests: the main thread).
// LARECOMP_BIND_TEST="frame:pad|key:<input>:<host>,..." starts a rebind capture of <input> (bind_* name: a, zl,
//   ls_up...) at that frame (the Controls tab shows it waiting) and presses <host> 90 frames later ("esc" cancels,
//   "wait" lets it time out); "frame:reset:pad|key" resets to the defaults.
static void push_key(SDL_Scancode sc, SDL_Keymod mod, bool down) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.which = TEST_DEVICE, e.key.scancode = sc, e.key.mod = mod, e.key.down = down;
    e.key.key = SDL_GetKeyFromScancode(sc, mod, false);
    SDL_PushEvent(&e);
}
uint32_t g_window_resize_request;  // (W << 16 | H: menu_apply_window_requests applies it)
static void test_push(uint64_t frame, unsigned long start, unsigned long len, const char *dev, const char *arg) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    if (!strcmp(dev, "resize")) {
        unsigned w = 0, h = 0;
        if (frame == start && sscanf(arg, "%ux%u", &w, &h) == 2 && w && h && w < 65536 && h < 65536) {
            LOG("[input] test: window to %ux%u\n", w, h);
            __atomic_store_n(&g_window_resize_request, w << 16 | h, __ATOMIC_RELAXED);
        }
        return;
    }
    if (!strcmp(dev, "shot")) {
        extern char g_window_shot_request[256];  // runtime/hle/hookshot.c
        if (frame == start) snprintf(g_window_shot_request, sizeof g_window_shot_request, "build/%s.png", arg);
        return;
    }
    if (!strcmp(dev, "mouse")) {
        if (frame != start) return;
        float mx, my, dx = 0, dy = 0;
        SDL_GetMouseState(&mx, &my);
        if (!strcmp(arg, "click")) {
            e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
            e.button.which = TEST_DEVICE, e.button.button = SDL_BUTTON_LEFT, e.button.down = true, e.button.clicks = 1;
            e.button.x = mx, e.button.y = my;
            SDL_PushEvent(&e);
            e.type = SDL_EVENT_MOUSE_BUTTON_UP, e.button.down = false;
        } else {
            sscanf(arg, "%f/%f", &dx, &dy);
            e.type = SDL_EVENT_MOUSE_MOTION;
            e.motion.which = TEST_DEVICE, e.motion.x = mx + dx, e.motion.y = my + dy, e.motion.xrel = dx, e.motion.yrel = dy;
        }
        SDL_PushEvent(&e);
        return;
    }
    if (!strcmp(dev, "sdlkey")) {
        if (frame == start || frame == start + len) push_key(SDL_GetScancodeFromName(arg), SDL_KMOD_NONE, frame == start);
        return;
    }
    if (!strcmp(dev, "sdlpad")) {
        int b = bind_pad_from_name(arg);
        if (b < 0 || b >= SDL_GAMEPAD_BUTTON_COUNT || (frame != start && frame != start + len)) return;
        e.type = frame == start ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
        e.gbutton.which = TEST_DEVICE, e.gbutton.button = (Uint8)b, e.gbutton.down = frame == start;
        SDL_PushEvent(&e);
        return;
    }
    if (frame != start) return;
    LOG("[input] test: typing \"%s\"\n", arg);
    static char ascii[128][2];  // (a text event keeps a pointer to its text: these live on)
    for (const char *p = arg; *p; p++) {
        if (*p == '{' && strchr(p, '}')) {  // a named key
            char nm[32];
            size_t n = (size_t)(strchr(p, '}') - p - 1);
            snprintf(nm, sizeof nm, "%.*s", (int)(n < sizeof nm ? n : sizeof nm - 1), p + 1);
            SDL_Scancode sc = SDL_GetScancodeFromName(nm);
            push_key(sc, SDL_KMOD_NONE, true), push_key(sc, SDL_KMOD_NONE, false);
            p += n + 1;
            continue;
        }
        SDL_Keymod mod = SDL_KMOD_NONE;
        SDL_Scancode sc = SDL_GetScancodeFromKey((SDL_Keycode)tolower((unsigned char)*p), &mod);
        if (isupper((unsigned char)*p)) mod |= SDL_KMOD_LSHIFT;
        push_key(sc, mod, true);
        int c = *p & 0x7f;
        ascii[c][0] = (char)c;
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_TEXT_INPUT, e.text.windowID = TEST_DEVICE, e.text.text = ascii[c];
        SDL_PushEvent(&e);
        push_key(sc, mod, false);
    }
}

static void host_test_frame(uint64_t frame) {
    static const char *hspec, *bspec;
    static int init;
    if (!init) { init = 1; hspec = getenv("LARECOMP_HOST_INPUT"); bspec = getenv("LARECOMP_BIND_TEST"); host_test = hspec; }
    static char *buf;  // (a copy to cut up; as long as the longest spec: a long scripted run is thousands of steps)
    static size_t cap;
    size_t need = 2048;
    if (hspec && strlen(hspec) + 1 > need) need = strlen(hspec) + 1;
    if (bspec && strlen(bspec) + 1 > need) need = strlen(bspec) + 1;
    if (need > cap) { free(buf); buf = malloc(need); cap = need; }
    if (hspec) {
        uint8_t keys[SDL_SCANCODE_COUNT] = {0};
        uint32_t padm = 0;
        int32_t sx = 0, sy = 0;
        snprintf(buf, cap, "%s", hspec);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            unsigned long start = 0, len = 10;
            char dev[8] = "", name[400] = "";
            if (sscanf(tok, "%lu:%7[a-z]:%399[^:]:%lu", &start, dev, name, &len) < 3) continue;
            if (!strcmp(dev, "type") || !strcmp(dev, "sdlpad") || !strcmp(dev, "sdlkey") || !strcmp(dev, "shot") ||
                !strcmp(dev, "resize") || !strcmp(dev, "mouse")) {
                test_push(frame, start, len, dev, name);
                continue;
            }
            if (frame < start || frame >= start + len) continue;
            if (!strcmp(dev, "key")) { SDL_Scancode sc = SDL_GetScancodeFromName(name); if (sc) keys[sc] = 1; }
            else if (!strcmp(dev, "lstick")) sscanf(name, "%d/%d", &sx, &sy);
            else { int h = bind_pad_from_name(name); if (h >= 0) padm |= 1u << h; }
        }
        bool kch = memcmp(keys, test_keys, sizeof keys) != 0, pch = padm != test_pad || sx != test_lx || sy != test_ly;
        memcpy(test_keys, keys, sizeof keys);
        test_pad = padm;
        test_lx = sx, test_ly = sy;
        if (kch) recompute_keyboard();
        if (pch) recompute_pad();
    }
    if (bspec) {
        snprintf(buf, cap, "%s", bspec);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            unsigned long start = 0;
            char dev[8] = "", in[16] = "", host[40] = "";
            if (sscanf(tok, "%lu:%7[a-z]:%15[a-z_]:%39[^:]", &start, dev, in, host) < 3) continue;
            if (!strcmp(dev, "reset")) { if (frame == start) bind_reset(!strcmp(in, "key") ? BIND_DEV_KEY : BIND_DEV_PAD); continue; }
            int d = !strcmp(dev, "key") ? BIND_DEV_KEY : BIND_DEV_PAD;
            if (frame == start) {
                LOG("[input] test: capture %s %s\n", dev, in);
                bind_capture_begin(d, bind_slot_from_name(in));
            }
            if (frame == start + 90 && strcmp(host, "wait")) {
                SDL_Event e;
                memset(&e, 0, sizeof e);
                if (!strcmp(host, "esc") || d == BIND_DEV_KEY) {
                    e.type = SDL_EVENT_KEY_DOWN;
                    e.key.scancode = !strcmp(host, "esc") ? SDL_SCANCODE_ESCAPE : SDL_GetScancodeFromName(host);
                } else {
                    int h = bind_pad_from_name(host);
                    if (h >= PADB_LTRIGGER) {
                        e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
                        e.gaxis.axis = h == PADB_LTRIGGER ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
                        e.gaxis.value = 32767;
                    } else {
                        e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                        e.gbutton.button = (Uint8)h;
                    }
                }
                bind_capture_event(&e);
            }
        }
    }
}

static uint64_t g_pad_obj;
volatile int g_inject_plus;  // frames to hold Plus for the game (movie.c: A/B skip the opening on rando seeds)
RT_ON_RESET(input_reboot_reset) { g_pad_obj = 0; g_inject_plus = 0; npad_forget_history(true); }  // (reboot: the controller object was guest memory)
// LARECOMP_ROUTE=<step>;<step>;... with LARECOMP_ROUTE_AT=<frame> (videos, tests): an autopilot after that frame,
// steering by Link's real place (runtime/hle/doorjump.c doorjump_link_pos) - x,z walks there (screen right = +x, down =
// +z; x,z,r: within r, default 0.6; no closer for 45 frames: a sword swing, twice, then on to the next step); A / B / X / Y / L / R / PLUS / MINUS
// / UP / DOWN / LEFT / RIGHT presses it; hB:N holds it N frames; sDEG:N the stick at that angle (0 right, 90 up) N
// frames; wN waits; @Level waits until that level is loaded (a door, a loading zone), @!Level until it is left, then
// 40 frames more.
static void route_tick(uint64_t frame, uint64_t *b, int32_t *lx, int32_t *ly) {
    static const char *spec;
    static long at = -1;
    static int step;
    static uint64_t step_frame, best_frame, swing_until;
    static int swings;
    static float best;
    static bool done;
    if (at < 0) {
        spec = getenv("LARECOMP_ROUTE");
        at = getenv("LARECOMP_ROUTE_AT") ? atol(getenv("LARECOMP_ROUTE_AT")) : 0;
        if (!spec) done = true;
    }
    if (done || (long)frame < at) return;
    extern bool doorjump_link_pos(float p[3]);
    extern const char *game_level_name(void);
    char tok[96] = "";
    {  // step `step` of the list
        const char *q = spec;
        for (int i = 0; i < step && q; i++) { q = strchr(q, ';'); if (q) q++; }
        if (!q || !*q) { done = true; LOG("[route] done at frame %llu\n", (unsigned long long)frame); return; }
        size_t n = strcspn(q, ";");
        if (n >= sizeof tok) n = sizeof tok - 1;
        memcpy(tok, q, n);
        tok[n] = 0;
    }
    if (!step_frame) { step_frame = frame, best = 1e9f, best_frame = frame, swings = 0; LOG("[route] step %d: %s (frame %llu)\n", step, tok, (unsigned long long)frame); }
    uint64_t t = frame - step_frame;
    bool next = false;
    static const char *const bn[] = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "LEFT", "UP", "RIGHT", "DOWN"};
    float x, z, r = 0.6f;
    int deg, n;
    char nm[32];
    if (tok[0] == '@') {
        const char *lv = game_level_name();
        static uint64_t seen;
        bool neg = tok[1] == '!', ok = lv && (neg ? strcmp(lv, tok + 2) && *lv : !strcmp(lv, tok + 1));  // (@!Level: until it's left)
        if (ok) { if (!seen) seen = frame; if (frame - seen > 40) next = true, seen = 0; }
        if (t > 900) next = true, seen = 0;
    } else if (tok[0] == 'w' && sscanf(tok + 1, "%d", &n) == 1) {
        next = t >= (uint64_t)n;
    } else if (tok[0] == 's' && sscanf(tok + 1, "%d:%d", &deg, &n) == 2) {
        double a = deg * 3.14159265358979 / 180;
        *lx = (int32_t)lround(cos(a) * 32767), *ly = (int32_t)lround(sin(a) * 32767);
        next = t >= (uint64_t)n;
    } else if (tok[0] == 'h' && sscanf(tok + 1, "%31[A-Z]:%d", nm, &n) == 2) {
        for (int i = 0; i < 16; i++) if (!strcmp(nm, bn[i])) *b |= 1ull << i;
        next = t >= (uint64_t)n;
    } else if (sscanf(tok, "%f,%f,%f", &x, &z, &r) >= 2) {
        float p[3];
        if (!doorjump_link_pos(p) || (p[0] == 0 && p[2] == 0)) { next = t > 300; best_frame = frame; }  // (a level loading)
        else {
            float dx = x - p[0], dz = z - p[2], d = sqrtf(dx * dx + dz * dz);
            if (d < best - 0.15f) best = d, best_frame = frame;
            if (d <= r) next = true;
            else if (frame - best_frame > 45 && swings < 2) {  // (stuck: a bush or grass in the way - the sword, as a player)
                swings++, best_frame = frame, swing_until = frame + 8;
            } else if (frame - best_frame > 45) { next = true; LOG("[route] step %d stuck %.2f from %.1f,%.1f at %.1f,%.1f\n", step, d, x, z, p[0], p[2]); }
            if (!next) {
                float k = 32767.0f / d;
                *lx = (int32_t)lroundf(dx * k), *ly = (int32_t)lroundf(-dz * k);
                if (frame < swing_until) *b |= 1ull << 1;  // (B)
            }
        }
    } else {
        for (int i = 0; i < 16; i++) if (!strcmp(tok, bn[i]) && t < 8) *b |= 1ull << i;
        next = t >= 16;
    }
    if (next) step++, step_frame = 0;
}

void input_frame(uint64_t frame) {
    atomic_store(&rumble_frame_ms, SDL_GetTicks());
    static int log_link = -1;  // LARECOMP_LOG_LINK=<frames>: Link's place and the level that often (scripted routes, videos)
    if (log_link < 0) log_link = getenv("LARECOMP_LOG_LINK") ? atoi(getenv("LARECOMP_LOG_LINK")) : 0;
    if (shared_title_las() && log_link > 0 && frame % (uint64_t)log_link == 0) {
        extern bool doorjump_link_pos(float p[3]);
        extern const char *game_level_name(void);
        float p[3];
        if (doorjump_link_pos(p)) LOG("[link] frame %llu %s %.1f %.1f %.1f\n", (unsigned long long)frame, game_level_name(), p[0], p[1], p[2]);
    }
    if (shared_title_las() && g_pad_obj && getenv("LARECOMP_DUMP_PAD") && frame % 600 == 0) {
        const uint8_t *m = (const uint8_t *)(uintptr_t)(g_pad_obj - 0x98);
        LOG("[pad] mgr %llx 4e0=%u 4e4=%u 4e8=%u 548=%#x 54c..=%02x %02x %02x 56c=%02x\n", g_pad_obj - 0x98,
            *(uint32_t *)(m + 0x4e0), *(uint32_t *)(m + 0x4e4), *(uint32_t *)(m + 0x4e8), *(uint32_t *)(m + 0x548),
            m[0x54c], m[0x54d], m[0x54e], m[0x56c]);
        for (int i = 0; i < 9; i++) {
            const uint8_t *o = m + 0x98 + 0x48 * i;
            uint64_t player = *(uint64_t *)(m + 0x10 * i);
            LOG("[pad]  ctl%d id %#x style %#x btn %#x conn %d %d | player %llx [8]=%u\n", i, *(uint32_t *)(o + 8),
                *(uint32_t *)(o + 0xc), *(uint32_t *)(o + 0x14), o[0x44], o[0x45], player,
                player ? *(uint32_t *)(uintptr_t)(player + 8) : 0);
        }
    }
    extern void watch_arm(void);
    watch_arm();
    host_test_frame(frame);
    const char *spec = getenv("LARECOMP_INPUT");
    if (!spec) return;
    static const char *const names[] = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR", "PLUS",
                                        "MINUS", "LEFT", "UP", "RIGHT", "DOWN"};
    uint64_t b = 0;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    static char buf[16384];  // (long scripted sessions: the perf walk is ~4 KB)
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        unsigned long start = 0, len = 10;
        char name[32] = "";
        if (sscanf(tok, "%lu:%31[A-Z_0-9]:%lu", &start, name, &len) < 2) continue;
        if (frame < start || frame >= start + len) continue;
        if (!strcmp(name, "STICK_UP")) ly = 32767;
        else if (!strcmp(name, "STICK_DOWN")) ly = -32767;
        else if (!strcmp(name, "STICK_LEFT")) lx = -32767;
        else if (!strcmp(name, "STICK_RIGHT")) lx = 32767;
        else if (!strcmp(name, "RSTICK_UP")) ry = 32767;
        else if (!strcmp(name, "RSTICK_DOWN")) ry = -32767;
        else if (!strcmp(name, "RSTICK_LEFT")) rx = -32767;
        else if (!strcmp(name, "RSTICK_RIGHT")) rx = 32767;
        else if (!strncmp(name, "STICK_", 6) && name[6] >= '0' && name[6] <= '9') {
            double r = atoi(name + 6) * 3.14159265358979 / 180;
            lx = (int32_t)lround(cos(r) * 32767), ly = (int32_t)lround(sin(r) * 32767);
        }
        for (int i = 0; i < 16; i++) if (!strcmp(name, names[i])) b |= 1ull << i;
    }
    if (shared_title_las()) {  // test hooks steering the stick (runtime/hle/doorjump.c LARECOMP_DOORJUMP_TEST)
        extern int32_t g_test_lx, g_test_ly;
        extern uint64_t g_test_stick_until;
        if (frame < g_test_stick_until) { lx = g_test_lx; ly = g_test_ly; }
        extern uint64_t g_test_buttons, g_test_buttons_until;  // (LARECOMP_PLACE action script)
        if (frame < g_test_buttons_until) b |= g_test_buttons;
    }
    if (shared_title_las()) route_tick(frame, &b, &lx, &ly);
    script_buttons = b;
    script_lx = lx;
    script_ly = ly;
    script_rx = rx;
    script_ry = ry;
}

extern bool menu_input_captured(void);  // runtime/ui/menu.mm: overlay menu or name entry up (it has the keys / pad)

// Buttons held right now (keyboard, gamepad, test script), independent of whether the game polls (movies).
uint64_t input_buttons_now(void) {
    return (menu_input_captured() ? 0 : key_buttons | pad_buttons | mouse_buttons | touch_buttons()) | script_buttons;
}

static int32_t clamp_axis(int32_t v) { return v > 32767 ? 32767 : v < -32767 ? -32767 : v; }

// NpadFullKeyState / NpadHandheldState / NpadJoy*State share this layout (0x28 bytes):
// {int64 sampling, u64 buttons, AnalogStick L {x,y}, AnalogStick R {x,y}, u32 attributes}
uint32_t shared_input_npad(uint32_t *style);

static void fill_state(Cpu *c) {
    uint8_t *destination = (uint8_t *)(uintptr_t)c->x[0];
    if (!destination || !c->x[1]) return;
    uint32_t id; memcpy(&id, (void *)(uintptr_t)c->x[1], sizeof id);
    _Alignas(8) uint8_t state[NPAD_STATE_SIZE];
    uint8_t *s = state;
    memset(s, 0, sizeof state);
    uint64_t serial = atomic_fetch_add(&sampling, UINT64_C(1)) + UINT64_C(1);
    memcpy(s, &serial, sizeof serial);
    static int nlog;
    if (nlog < 6 && (sampling % 600) == 1) { nlog++; LOG("[input] GetNpadState id %#x lr %llx\n", id, c->x[30]); }
    if (id != shared_input_npad(NULL)) { memcpy(destination, state, sizeof state); return; } // selected controller
    if (shared_title_las()) {
    extern void warp_game_tick(Cpu *c);
    warp_game_tick(c);  // game main thread, once per frame: apply a queued warp
    { extern void give_item_tick(Cpu *c, uint64_t frame); extern uint64_t renderer_present_count(void);
      give_item_tick(c, renderer_present_count());
      extern void chamber_game_tick(Cpu *c, uint64_t frame);
      chamber_game_tick(c, renderer_present_count());
      extern void digs_game_tick(Cpu *c, uint64_t frame);
      digs_game_tick(c, renderer_present_count());
      extern void pot_bush_game_tick(Cpu *c, uint64_t frame);
      pot_bush_game_tick(c, renderer_present_count());
      extern void shell_spots_tick(Cpu *c, uint64_t frame);
      shell_spots_tick(c, renderer_present_count());
      extern void qol_tick(void);
      qol_tick();
      extern void run_timer_tick(void);
      run_timer_tick();  // (run timer / LiveSplit: runtime/hle/run_timer.c)
      extern void pets_game_tick(Cpu *c);
      pets_game_tick(c); }
    }
    extern uint64_t g_watch_lo, g_watch_hi;
    if (shared_title_las() && id == 0 && !g_pad_obj) g_pad_obj = c->x[1] - 8;
    if (shared_title_las() && id == 0 && !g_watch_lo && getenv("LARECOMP_WATCH_PAD")) {
        unsigned lo = 0x98 + 0x10, hi = 0x98 + 0x48;  // offsets from the controller manager
        sscanf(getenv("LARECOMP_WATCH_PAD"), "%x,%x", &lo, &hi);
        g_watch_lo = c->x[1] - 8 - 0x98 + lo;
        g_watch_hi = c->x[1] - 8 - 0x98 + hi;
        LOG("[input] watching controller object %llx\n", c->x[1] - 8);
    }
    bool menu = menu_input_captured();  // the overlay menu / name entry has the controls (typing there isn't for the game)
    uint64_t held = key_buttons | pad_buttons | mouse_buttons | touch_buttons();
    atomic_fetch_and(&press_unseen, ~held);  // (the game sees these now)
    static uint64_t tap, tap_frame;  // (pressed and released since the last poll: held for the rest of this frame)
    {
        extern uint64_t renderer_present_count(void);
        uint64_t t = atomic_exchange(&tap_buttons, 0), f = renderer_present_count();
        if (t) {
            tap |= t, tap_frame = f;
            if (getenv("LARECOMP_INPUT")) LOG("[input] short press %#llx\n", (unsigned long long)t);
        } else if (f > tap_frame) tap = 0;
    }
    uint64_t b = menu ? script_buttons : held | tap | script_buttons;
    if (title_adapter_selected()->id == TITLE_EEVEE) {
        // Eevee's controller ZR is its screen finger, as in the working reference.
        // Keyboard/touch-overlay ZR remains a normal button.
        uint64_t zr = 1ull << BTN_ZR;
        b = menu ? script_buttons & ~zr
                 : (key_buttons | mouse_buttons | touch_buttons() | tap | pad_buttons | script_buttons);
        if (!((key_buttons | touch_buttons()) & zr)) b &= ~zr;
    }
    if (shared_title_las()) {   // injected Plus: held for ~10 presented frames (the game polls several times per frame)
        extern uint64_t renderer_present_count(void);
        static uint64_t until;
        if (g_inject_plus > 0) { until = renderer_present_count() + 10; g_inject_plus = 0; }
        if (renderer_present_count() < until) b |= 1ull << BTN_PLUS;
    }
    int32_t tlx, tly;
    touch_stick(&tlx, &tly);
    int32_t lx = clamp_axis((menu ? 0 : key_lx + pad_lx + tlx) + script_lx), ly = clamp_axis((menu ? 0 : key_ly + pad_ly + tly) + script_ly);
    int32_t rx = clamp_axis((menu ? 0 : key_rx + pad_rx) + script_rx), ry = clamp_axis((menu ? 0 : key_ry + pad_ry) + script_ry);
    extern bool mirror_input(void);  // mirror mode, the world on screen (runtime/hle/mirror.c): left and right swap
    bool mirrored = !menu && mirror_input();
    if (mirrored) {
        lx = -lx;
        uint64_t l = (b >> BTN_LEFT) & 1, r = (b >> BTN_RIGHT) & 1;
        b = (b & ~((1ull << BTN_LEFT) | (1ull << BTN_RIGHT))) | (l << BTN_RIGHT) | (r << BTN_LEFT);
    }
    // d-pad / arrow keys walk (setting) while the stick is at rest. "At rest" has a dead zone: a controller's stick
    // never reads exactly 0 (a DualSense rests at a few hundred), which used to turn the d-pad off altogether.
    if (!menu && g_settings.dpad_move && (int64_t)lx * lx + (int64_t)ly * ly < 8000ll * 8000) {
        uint64_t d = key_buttons | pad_buttons | script_buttons;
        int dx = (int)((d >> BTN_RIGHT) & 1) - (int)((d >> BTN_LEFT) & 1);
        if (mirrored) dx = -dx;
        int dy = (int)((d >> BTN_UP) & 1) - (int)((d >> BTN_DOWN) & 1);
        int32_t m = dx && dy ? 23170 : 32767;  // diagonals: same speed (32767 / sqrt 2)
        if (dx || dy) lx = dx * m, ly = dy * m;
    }
    if (shared_title_las()) {   // door jumps stop Link for a moment before moving him (runtime/hle/doorjump.c): stick + d-pad released
        extern int32_t g_stick_lx, g_stick_ly;  // (what the player holds, before that: doorjump.c reads it)
        g_stick_lx = lx, g_stick_ly = ly;
        extern uint64_t g_stick_freeze_until;
        extern uint64_t renderer_present_count(void);
        if (renderer_present_count() < g_stick_freeze_until) {
            lx = ly = 0;
            b &= ~(0xfull << 12);
        }
    }
    // stick directions also appear as buttons (bits 16-23)
    if (lx < -16000) b |= 1ull << 16;
    if (ly > 16000) b |= 1ull << 17;
    if (lx > 16000) b |= 1ull << 18;
    if (ly < -16000) b |= 1ull << 19;
    if (rx < -16000) b |= 1ull << 20;
    if (ry > 16000) b |= 1ull << 21;
    if (rx > 16000) b |= 1ull << 22;
    if (ry < -16000) b |= 1ull << 23;
    if (shared_title_las()) {   // the pause menu's companion slot (runtime/hle/pets.c) takes the d-pad / sticks / A while the cursor is on it
        extern uint64_t pets_menu_input(uint64_t b, int32_t st[4]);
        int32_t st[4] = {lx, ly, rx, ry};
        b = pets_menu_input(b, st);
        // the check tracker drives the Map page in randomizer seeds (runtime/ui/tracker_overlay.mm): its cursor takes
        // the d-pad / left stick / A / Y (and B while one of its panels is open)
        extern uint64_t tracker_map_input(uint64_t b, int32_t st[4]);
        b = tracker_map_input(b, st);
        lx = st[0], ly = st[1], rx = st[2], ry = st[3];
    }
    *(uint64_t *)(s + 8) = b;
    static uint64_t last_b;
    if (b != last_b && getenv("LARECOMP_INPUT")) LOG("[input] id %#x buttons %#llx (sampling %lld, lr %llx)\n", id, (unsigned long long)b, (long long)sampling, c->x[30]);
    last_b = b;
    *(int32_t *)(s + 16) = lx;
    *(int32_t *)(s + 20) = ly;
    if (shared_title_las()) { extern void input_note_rstick(int32_t ry); input_note_rstick(ry); }
    *(int32_t *)(s + 24) = rx;
    *(int32_t *)(s + 28) = ry;
    *(uint32_t *)(s + 32) = 1;  // IsConnected
    memcpy(destination, state, sizeof state);
}

HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", hid_state_fullkey) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", hid_state_dual) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", hid_state_left) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", hid_state_handheld) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", hid_state_right) { fill_state(c); }

// Public plural ABI: (State *out, int count, const uint32_t &id), returns count.
// Providers can call back into guest LAS helpers; never hold the history mutex
// around fill_state or copy a whole Cpu to redirect its two output arguments.
static void fill_states(Cpu *c, uint32_t requested_style) {
    uint64_t output = c->x[0], requested = c->x[1], id_reference = c->x[2];
    int32_t count = (int32_t)(uint32_t)requested;
    size_t capacity = npad_history_count(count);
    if (!output || !capacity || output > UINTPTR_MAX - capacity * NPAD_STATE_SIZE) { c->x[0] = 0; return; }
    if (!id_reference || id_reference > UINTPTR_MAX - sizeof(uint32_t)) {
        memset((void *)(uintptr_t)output, 0, capacity * NPAD_STATE_SIZE); c->x[0] = 0; return;
    }
    uint32_t id, selected_style;
    memcpy(&id, (void *)(uintptr_t)id_reference, sizeof id);
    uint32_t selected_id = shared_input_npad(&selected_style);
    pthread_mutex_lock(&npad_history_lock);
    uint64_t epoch = npad_history_epoch;
    pthread_mutex_unlock(&npad_history_lock);
    NpadHistoryState sample;
    bool captured = npad_history_connected(id, requested_style, selected_id, selected_style);
    if (captured) {
        c->x[0] = (uint64_t)(uintptr_t)&sample; c->x[1] = id_reference;
        fill_state(c);
        c->x[0] = output; c->x[1] = requested; c->x[2] = id_reference;
    }
    // Recheck policy after guest-call-capable capture. A focus/reset during
    // capture invalidates publication, so stale pressed state cannot resurrect.
    selected_id = shared_input_npad(&selected_style);
    pthread_mutex_lock(&npad_history_lock);
    size_t written = 0;
    if (epoch != npad_history_epoch) memset((void *)(uintptr_t)output, 0, capacity * NPAD_STATE_SIZE);
    else written = npad_history_read(&npad_history_bank, id, requested_style, selected_id,
        selected_style, captured ? &sample : NULL, (void *)(uintptr_t)output, count);
    pthread_mutex_unlock(&npad_history_lock);
    c->x[0] = written;
}
HLE_FUNC("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", hid_states_fullkey) { fill_states(c, 1u << 0); }
HLE_FUNC("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", hid_states_handheld) { fill_states(c, 1u << 1); }
HLE_FUNC("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj", hid_states_dual) { fill_states(c, 1u << 2); }
HLE_FUNC("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", hid_states_left) { fill_states(c, 1u << 3); }
HLE_FUNC("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj", hid_states_right) { fill_states(c, 1u << 4); }

// Selected title policy: LAS FullKey/id0; Eevee Handheld/id0x20 by default.
uint32_t shared_input_npad(uint32_t *style) {
    if (shared_title_las()) { if (style) *style = 1; return 0; }
    static uint32_t id = ~0u, st;
    if (id == ~0u) {
        const char *e = getenv("LARECOMP_NPAD");
        if (!e || !strcmp(e, "handheld")) id = 0x20, st = 1u << 1;
        else if (!strcmp(e, "fullkey")) id = 0, st = 1u << 0;
        else if (!strcmp(e, "dual")) id = 0, st = 1u << 2;
        else if (!strcmp(e, "left")) id = 0, st = 1u << 3;
        else if (!strcmp(e, "right")) id = 0, st = 1u << 4;
        else id = 0x20, st = 1u << 1;
        LOG("[input] controller: npad id %#x, style %#x\n", id, st);
    }
    if (style) *style = st;
    return id;
}

HLE_FUNC("_ZN2nn3hid15GetNpadStyleSetERKj", hid_style_set) {
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[0], style;
    static int nlog;
    if (nlog++ < 4) LOG("[input] GetNpadStyleSet id %#x\n", id);
    c->x[0] = id == shared_input_npad(&style) ? style : 0;
}

// BindNpadStyleSetUpdateEvent(id, SystemEventType*, clear mode): signaled once so the game sees a
// controller connected.
#include "../kernel.h"
HLE_FUNC("_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE", hid_bind_style_event) {
    uint32_t w, r;
    KEvent *ev = kernel_create_event_pair(&w, &r);
    pthread_mutex_lock(&g_klock);
    kernel_signal(ev);
    pthread_mutex_unlock(&g_klock);
    uint64_t clear_mode = c->x[2];
    c->x[0] = c->x[1];
    c->x[1] = r;
    c->x[2] = 0;
    c->x[3] = clear_mode;
    guest_call(c, guest_sym("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"));
    LOG("[input] style set update event bound\n");
}

// Touch screen: TouchScreenState<16> {int64 sampling; int32 count; pad; TouchState[16]}, TouchState (0x28) {int64
// delta time; u32 attributes (bit 0 Start, bit 1 End); u32 finger id; u32 x, y; u32 diameter x, y; u32 rotation;
// pad}. One finger: the free mouse (touch_at), the controller's finger (pad_finger), or
// LARECOMP_TOUCH="<from>-<to>[@x,y[,width]]" (frames: a back-and-forth petting stroke `width` wide - default 120,
// 0 = a press in place - around x,y, default the middle of the screen; display pixels, 1920 x 1080) or "<from>-<to>@x,y>x2,y2" (a drag from x,y
// to x2,y2); several segments separated by ';' (consecutive frames: one touch going on); for tests.
static bool pad_finger(int32_t *x, int32_t *y);
static bool touch_now(int32_t *x, int32_t *y) {
    static struct { int from, to, cx, cy, amp, ex, ey; } seg[8];
    static int nseg = -1;
    if (nseg < 0) {
        nseg = 0;
        for (const char *e = getenv("LARECOMP_TOUCH"); e && *e && nseg < 8; e = strchr(e, ';') ? strchr(e, ';') + 1 : NULL) {
            seg[nseg] = (typeof(seg[0])){0, 0, 960, 495, 180, -1, -1};
            sscanf(e, "%d-%d@%d,%d,%d", &seg[nseg].from, &seg[nseg].to, &seg[nseg].cx, &seg[nseg].cy, &seg[nseg].amp);
            const char *gt = strchr(e, '>'), *semi = strchr(e, ';');
            if (gt && (!semi || gt < semi)) sscanf(gt + 1, "%d,%d", &seg[nseg].ex, &seg[nseg].ey);
            if (seg[nseg].from > 0) nseg++;
        }
    }
    extern uint64_t renderer_present_count(void);
    uint64_t f = renderer_present_count();
    for (int i = 0; i < nseg; i++) {
        if (f < (uint64_t)seg[i].from || f > (uint64_t)seg[i].to) continue;
        double t = (double)(f - seg[i].from), k = seg[i].to > seg[i].from ? t / (seg[i].to - seg[i].from) : 1;
        *x = (int32_t)(seg[i].ex >= 0 ? seg[i].cx + (seg[i].ex - seg[i].cx) * k : seg[i].cx + seg[i].amp * sin(t * 0.25));
        *y = (int32_t)(seg[i].ex >= 0 ? seg[i].cy + (seg[i].ey - seg[i].cy) * k : seg[i].cy + seg[i].amp / 6 * sin(t * 0.5));
        return true;
    }
    if (pad_finger(x, y)) return true;
    *x = touch_x, *y = touch_y;
    return touch_down && !menu_input_captured();
}
// The controller as a finger (a controller has no touch screen, and Play with Eevee pets with one): the right stick
// moves it over the screen - the overlay draws it (runtime/ui/menu.mm) - and ZR held touches there. ZR then doesn't
// reach the game (fill_state; there it only repeats A).
static double finger_x = 960, finger_y = 540;
static _Atomic uint64_t finger_seen_ns;
static _Atomic int finger_down;
static bool pad_finger(int32_t *x, int32_t *y) {
    if (title_adapter_selected()->id != TITLE_EEVEE) return false;
    static uint64_t last;
    uint64_t now = SDL_GetTicksNS();
    double dt = last ? (now - last) / 1e9 : 0;
    if (dt > 0.1) dt = 0.1;
    last = now;
    int32_t rx = pad_rx + script_rx, ry = pad_ry + script_ry;
    double ax = abs(rx) > 6000 ? rx / 32767.0 : 0, ay = abs(ry) > 6000 ? ry / 32767.0 : 0;
    if (ax != 0 || ay != 0) {
        finger_x += ax * 1000 * dt;  // (display pixels per second at full tilt: about 2 s across)
        finger_y -= ay * 1000 * dt;
        finger_x = finger_x < 0 ? 0 : finger_x > touch_w() - 1 ? touch_w() - 1 : finger_x;
        finger_y = finger_y < 0 ? 0 : finger_y > TOUCH_H - 1 ? TOUCH_H - 1 : finger_y;
        finger_seen_ns = now;
    }
    bool down = ((pad_buttons | script_buttons) >> BTN_ZR & 1) && !menu_input_captured();
    if (down) finger_seen_ns = now;
    finger_down = down;
    *x = (int32_t)finger_x, *y = (int32_t)finger_y;
    return down;
}
// Overlay (runtime/ui/menu.mm): where to draw the controller's finger, in window pixels, for 3 s after it last moved.
bool input_finger_cursor(float *x, float *y, bool *down) {
    uint64_t seen = finger_seen_ns;
    if (!seen || SDL_GetTicksNS() - seen > 3000000000ull) return false;
    double rect[4]; input_present_rect(rect);
    *x = (float)(rect[0] + finger_x / touch_w() * rect[2]);
    *y = (float)(rect[1] + finger_y / TOUCH_H * rect[3]);
    *down = finger_down;
    return true;
}

static void touch_caller_log(const char *what, uint64_t lr) {  // (LARECOMP_TOUCH runs: who reads touches / gestures)
    static uint64_t seen[32];
    static int n, on = -1;
    if (on < 0) on = getenv("LARECOMP_TOUCH") != NULL;
    if (!on) return;
    for (int i = 0; i < n; i++) if (seen[i] == lr) return;
    if (n < 32) seen[n++] = lr;
    extern uint64_t renderer_present_count(void);
    LOG("[touch] %s read from lr %llx (frame %llu)\n", what, (unsigned long long)lr, (unsigned long long)renderer_present_count());
}
HLE_FUNC("_ZN2nn3hid20GetTouchScreenStatesILm16EEEiPNS0_16TouchScreenStateIXT_EEEi", hid_touch_states) {
    touch_caller_log("touch screen", c->x[30]);
    uint8_t *s = (uint8_t *)(uintptr_t)c->x[0];
    int count = (int)c->x[1];
    static int64_t sampling;
    static bool was_down;
    static int32_t last_x, last_y;
    static int nlog;
    if (count <= 0) { c->x[0] = 0; return; }
    int32_t x, y;
    bool down = touch_now(&x, &y);
    // The release sample keeps the finger where it was: the game checks every sample's position (finger id -> x, y),
    // and a release reported elsewhere (the test's default, the mouse while the controller's finger let go) took the
    // touch off an on-screen button before it ended - the berry button never opened.
    if (down) last_x = x, last_y = y;
    else x = last_x, y = last_y;
    memset(s, 0, 0x290);
    *(int64_t *)s = ++sampling;
    if (down || was_down) {  // (the release is one more sample, flagged End)
        uint8_t *t = s + 0x10;
        *(int32_t *)(s + 8) = 1;
        *(int64_t *)t = 16666667;
        *(uint32_t *)(t + 8) = down && !was_down ? 1 : !down ? 2 : 0;
        *(uint32_t *)(t + 0x10) = (uint32_t)x;
        *(uint32_t *)(t + 0x14) = (uint32_t)y;
        *(uint32_t *)(t + 0x18) = *(uint32_t *)(t + 0x1c) = 15;
        if (nlog < 12 && down != was_down) {
            nlog++;
            LOG("[touch] %s at %d,%d (lr %llx)\n", down ? "down" : "up", x, y, (unsigned long long)c->x[30]);
        }
    }
    was_down = down;
    c->x[0] = 1;
}
// Gestures (Let's Go reads them at start-up only - its screens go by the raw touches above - but they are cheap to
// keep right), recognized from the same finger as the system does: Touch (new touch) -> Press (held still 0.5 s) / Pan (moved, per frame) -> Tap
// (short, still) or Swipe (fast) -> Complete. GestureState (0x60): int64 event number, int64 context number, u32 type
// (0 idle, 1 complete, 2 cancel, 3 touch, 4 press, 5 tap, 6 pan, 7 swipe), u32 direction (0 none, 1 left, 2 up,
// 3 right, 4 down), u32 x, y, s32 delta x, y, f32 velocity x, y, u32 attributes (bit 4 new touch), f32 scale,
// f32 rotation, s32 point count, points[4] {u32 x, y}. The game reads the newest first and keeps the event number.
typedef struct { int64_t num, ctx; uint32_t type, dir, x, y; int32_t dx, dy; float vx, vy; uint32_t attr; float scale, rot;
                 int32_t npoints; uint32_t pts[8]; } Gesture;
_Static_assert(sizeof(Gesture) == 0x60, "nn::hid::GestureState");
static Gesture g_gestures[16];
static int64_t g_gesture_num, g_gesture_ctx;
static void gesture_emit(uint32_t type, uint32_t dir, int32_t x, int32_t y, int32_t dx, int32_t dy, float vx, float vy,
                         uint32_t attr, bool point) {
    if (getenv("LARECOMP_TOUCH")) LOG("[touch] gesture %u at %d,%d\n", type, x, y);
    memmove(g_gestures + 1, g_gestures, sizeof g_gestures - sizeof g_gestures[0]);
    Gesture *g = &g_gestures[0];
    memset(g, 0, sizeof *g);
    g->num = ++g_gesture_num;
    g->ctx = g_gesture_ctx;
    g->type = type, g->dir = dir, g->x = (uint32_t)x, g->y = (uint32_t)y, g->dx = dx, g->dy = dy, g->vx = vx, g->vy = vy;
    g->attr = attr, g->scale = 1;
    if (point) { g->npoints = 1; g->pts[0] = (uint32_t)x; g->pts[1] = (uint32_t)y; }
}
// Once per presented frame (the game may poll several times): the finger's new sample -> gesture events.
static void gesture_update(void) {
    extern uint64_t renderer_present_count(void);
    static uint64_t frame = ~0ull, start_ns, last_ns;
    static bool was, moved, pressed;
    static int32_t sx, sy, lx, ly;
    uint64_t f = renderer_present_count();
    if (f == frame) return;
    frame = f;
    int32_t x, y;
    bool down = touch_now(&x, &y);
    uint64_t now = SDL_GetTicksNS();
    double dt = last_ns ? (now - last_ns) / 1e9 : 1 / 30.0;
    if (dt <= 0) dt = 1 / 30.0;
    last_ns = now;
    if (down && !was) {
        g_gesture_ctx++;
        sx = lx = x, sy = ly = y, start_ns = now, moved = pressed = false;
        gesture_emit(3, 0, x, y, 0, 0, 0, 0, 1u << 4, true);  // Touch, new touch
    } else if (down) {
        if (!moved && (abs(x - sx) > 16 || abs(y - sy) > 16)) moved = true;
        if (moved && (x != lx || y != ly)) {
            gesture_emit(6, 0, x, y, x - lx, y - ly, (float)((x - lx) / dt), (float)((y - ly) / dt), 0, true);  // Pan
        } else if (!moved && !pressed && now - start_ns > 500000000ull) {
            pressed = true;
            gesture_emit(4, 0, x, y, 0, 0, 0, 0, 0, true);  // Press
        }
        lx = x, ly = y;
    } else if (was) {
        double t = (now - start_ns) / 1e9, vx = (lx - sx) / (t > 0 ? t : 1), vy = (ly - sy) / (t > 0 ? t : 1);
        if (!moved && !pressed) gesture_emit(5, 0, sx, sy, 0, 0, 0, 0, 0, true);  // Tap
        else if (moved && t < 0.4 && fabs(vx) + fabs(vy) > 800) {  // Swipe
            uint32_t dir = fabs(vx) > fabs(vy) ? (vx < 0 ? 1 : 3) : (vy < 0 ? 2 : 4);
            gesture_emit(7, dir, lx, ly, lx - sx, ly - sy, (float)vx, (float)vy, 0, true);
        }
        gesture_emit(1, 0, lx, ly, 0, 0, 0, 0, 0, false);  // Complete
    }
    was = down;
}
HLE_FUNC("_ZN2nn3hid16GetGestureStatesEPNS0_12GestureStateEi", hid_gesture_states) {
    touch_caller_log("gestures", c->x[30]);
    gesture_update();
    int count = (int)c->x[1], n = 0;
    Gesture *out = (Gesture *)(uintptr_t)c->x[0];
    for (; n < count && n < 16 && g_gestures[n].num; n++) out[n] = g_gestures[n];
    c->x[0] = (uint64_t)n;
}


// The SDK vibration node graph stays recompiled; HID IPC 201/206 sends its
// output to rumble.h / SDL using the native-layout handles below.
HLE_FUNC("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", hid_init_vibration) { (void)c; }
HLE_FUNC("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", hid_vibration_handles) {
    uint32_t *out = (uint32_t *)(uintptr_t)c->x[0];
    // (as in hid_six_axis_handles below) a negative requested count must clamp to 0, not pass
    // through: it would otherwise skip the write but still report a huge count back as unsigned.
    int n = (int)c->x[1] < 0 ? 0 : (int)c->x[1] < 2 ? (int)c->x[1] : 2;
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[2];
    // Native wire layout is style, player, device index, padding (one byte each).
    // The old synthetic handles placed the style byte in padding.
    for (int i = 0; i < n; i++) out[i] = 3u | ((id & 0xFF) << 8) | ((uint32_t)i << 16);
    c->x[0] = (uint64_t)n;
}
HLE_FUNC("_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE", hid_vibration_info) {
    uint32_t *info = (uint32_t *)(uintptr_t)c->x[0];
    uint32_t h = *(uint32_t *)(uintptr_t)c->x[1];
    info[0] = 1;        // VibrationDeviceType_LinearResonantActuator
    int side = rumble_side(h);
    info[1] = side < 0 ? 0 : (uint32_t)side + 1;  // None=0, Left=1, Right=2
}

// Six-axis (motion) sensor. Let's Go aims Poke Ball throws with the gyro in handheld mode; with no sensor behind it
// the SDK read garbage and the aim went into the sky. The rest pose is a controller lying level (identity orientation,
// gravity -z), which keeps the aim centred. The mouse turns this virtual controller (yaw / pitch, input_handle_event
// accumulates the motion), with every field consistent: orientation, angle, angular velocity and gravity.
// SixAxisSensorState (0x60): +0 delta time ns, +8 sampling number, +0x10 acceleration (G), +0x1c angular velocity
// (turns / s), +0x28 angle (turns), +0x34 direction (3x3: x, y, z axes), +0x58 attributes (bit 0 IsConnected).
// LARECOMP_GYRO_TEST=x|y|z sweeps the yaw axis ±15 degrees (2 s period) to see which way the aim follows.
static pthread_mutex_t motion_lock = PTHREAD_MUTEX_INITIALIZER;
static double gyro_yaw, gyro_pitch;               // radians
static _Atomic uint64_t gyro_start_ns;            // when the game last started the sensor (aiming begins)
static int gyro_yaw_axis = 2, gyro_pitch_axis = 0;
static void mat_rot(double m[3][3], int axis, double a) {
    int i = (axis + 1) % 3, j = (axis + 2) % 3;
    memset(m, 0, 9 * sizeof(double));
    m[axis][axis] = 1;
    m[i][i] = cos(a); m[i][j] = -sin(a);
    m[j][i] = sin(a); m[j][j] = cos(a);
}
static void mat_mul(double o[3][3], double a[3][3], double b[3][3]) {
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) o[r][k] = a[r][0] * b[0][k] + a[r][1] * b[1][k] + a[r][2] * b[2][k];
}
static void six_axis_fill(uint8_t *s, int64_t sampling) {
    pthread_mutex_lock(&motion_lock);
    static int test = -1, test_fixed;
    static double mouse_sensitivity = 0.1; // pinned Let's Go degrees per pixel
    if (test < 0) {
        const char *configured = getenv("LARECOMP_MOUSE_SENS");
        if (configured) {
            double requested = strtod(configured, NULL);
            if (isfinite(requested) && requested > 0 && requested <= 10) mouse_sensitivity = requested;
        }
        const char *e = getenv("LARECOMP_GYRO_TEST");
        test = e && *e >= 'x' && *e <= 'z' ? 1 + (*e - 'x') : 0;
        if (test && (e[1] == '+' || e[1] == '-')) test_fixed = e[1] == '+' ? 1 : -1;
        const char *ax = getenv("LARECOMP_GYRO_AXES");  // "<yaw><pitch>", e.g. "zx" (the default)
        if (ax && ax[0] >= 'x' && ax[0] <= 'z' && ax[1] >= 'x' && ax[1] <= 'z') gyro_yaw_axis = ax[0] - 'x', gyro_pitch_axis = ax[1] - 'x';
    }
    static uint64_t last_ns;
    uint64_t now = SDL_GetTicksNS();
    double dt = last_ns ? (now - last_ns) / 1e9 : 1 / 60.0;
    if (dt < 1e-4) dt = 1e-4;
    if (dt > 0.1) dt = 0.1;
    last_ns = now;
    double py = gyro_yaw, pp = gyro_pitch;
    int yaw_axis = gyro_yaw_axis;
    if (test) {
        yaw_axis = test - 1;
        // x+ / z-...: 1 s into the aim, a turn of 10 degrees at 20 degrees/s (a sudden jump is ignored as noise)
        double ts = gyro_start_ns ? (now - gyro_start_ns) / 1e9 - 1 : 0, ramp = ts < 0 ? 0 : ts > 0.5 ? 0.5 : ts;
        gyro_yaw = test_fixed ? test_fixed * 20 * ramp * M_PI / 180 : 15 * M_PI / 180 * sin(now / 1e9 * M_PI);
        gyro_pitch = 0;
    } else {
        double k = mouse_sensitivity * M_PI / 180 / 1000;  // degrees per pixel
        gyro_yaw -= atomic_exchange(&mouse_acc_x, 0) * k;
        gyro_pitch -= atomic_exchange(&mouse_acc_y, 0) * k;
        double lim = 80 * M_PI / 180;
        if (gyro_pitch > lim) gyro_pitch = lim;
        if (gyro_pitch < -lim) gyro_pitch = -lim;
    }
    double ry[3][3], rp[3][3], r[3][3];
    mat_rot(ry, yaw_axis, gyro_yaw);
    mat_rot(rp, gyro_pitch_axis, gyro_pitch);
    mat_mul(r, ry, rp);
    memset(s, 0, 0x60);
    *(int64_t *)(s + 0x00) = (int64_t)(dt * 1e9);
    *(int64_t *)(s + 0x08) = sampling;
    float *acc = (float *)(s + 0x10), *gyr = (float *)(s + 0x1c), *ang = (float *)(s + 0x28), *dir = (float *)(s + 0x34);
    for (int i = 0; i < 3; i++) acc[i] = (float)-r[2][i];  // gravity (world -z) in the controller's frame: R^T (0,0,-1)
    gyr[yaw_axis] += (float)((gyro_yaw - py) / dt / (2 * M_PI));
    gyr[gyro_pitch_axis] += (float)((gyro_pitch - pp) / dt / (2 * M_PI));
    ang[yaw_axis] += (float)(gyro_yaw / (2 * M_PI));
    ang[gyro_pitch_axis] += (float)(gyro_pitch / (2 * M_PI));
    for (int a = 0; a < 3; a++)  // direction: the controller's x, y, z axes in world coordinates (columns of R)
        for (int i = 0; i < 3; i++) dir[a * 3 + i] = (float)r[i][a];
    *(uint32_t *)(s + 0x58) = 1;  // IsConnected to this virtual sensor, not a physical Thor gyro
    pthread_mutex_unlock(&motion_lock);
}
static _Atomic int64_t six_axis_sampling;
HLE_FUNC("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", hid_six_axis_handles) {
    uint32_t *out = (uint32_t *)(uintptr_t)c->x[0];
    int n = (int)c->x[1] < 0 ? 0 : (int)c->x[1] < 2 ? (int)c->x[1] : 2;
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[2];
    for (int i = 0; i < n; i++) out[i] = (id & 0xFF) << 8 | (uint32_t)i | 0x04000000u;  // (opaque to the game)
    c->x[0] = (uint64_t)n;
}
HLE_FUNC("_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", hid_six_axis_start) {
    // aiming begins (the game measures the turn from here): the virtual controller starts level again, and the
    // mouse becomes the gyro at its next move (input_handle_event)
    gyro_aiming = 1;
    gyro_start_ns = SDL_GetTicksNS();
    pthread_mutex_lock(&motion_lock);
    gyro_yaw = gyro_pitch = 0;
    pthread_mutex_unlock(&motion_lock);
    atomic_store(&mouse_acc_x, 0);
    atomic_store(&mouse_acc_y, 0);
    extern uint64_t renderer_present_count(void);
    LOG("[input] six-axis sensor started (frame %llu)\n", (unsigned long long)renderer_present_count());
}
HLE_FUNC("_ZN2nn3hid17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE", hid_six_axis_stop) {
    gyro_aiming = 0;
    extern uint64_t renderer_present_count(void);
    if (getenv("LARECOMP_GYRO_TEST")) LOG("[input] six-axis sensor stopped (frame %llu)\n", (unsigned long long)renderer_present_count());
}
static void six_axis_log(Cpu *c) {  // (LARECOMP_GYRO_TEST: when the game reads it, and from where)
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_GYRO_TEST") != NULL;
    extern uint64_t renderer_present_count(void);
    static uint64_t last, n;
    uint64_t f = renderer_present_count();
    n++;
    if (on && (n <= 8 || f / 30 != last / 30))
        LOG("[gyro] read #%llu at frame %llu (lr %llx)\n", (unsigned long long)n, (unsigned long long)f, (unsigned long long)c->x[30]);
    last = f;
}
HLE_FUNC("_ZN2nn3hid21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE", hid_six_axis_state) {
    six_axis_log(c);
    six_axis_fill((uint8_t *)(uintptr_t)c->x[0], ++six_axis_sampling);
}
HLE_FUNC("_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", hid_six_axis_states) {
    six_axis_log(c);
    int n = (int)c->x[1];
    if (n > 0) six_axis_fill((uint8_t *)(uintptr_t)c->x[0], ++six_axis_sampling);  // the latest sample only
    c->x[0] = n > 0 ? 1 : 0;
}
HLE_FUNC("_ZN2nn3hid21IsSixAxisSensorAtRestERKNS0_19SixAxisSensorHandleE", hid_six_axis_at_rest) {
    static int n;
    if (getenv("LARECOMP_GYRO_TEST") && n++ < 8) LOG("[gyro] IsSixAxisSensorAtRest (lr %llx)\n", (unsigned long long)c->x[30]);
    c->x[0] = 0;  // never at rest: at rest, the game stopped reading the sensor (a hand-held console never is)
}
static _Atomic int fusion_enabled = 1, gyro_drift = 1;
static _Atomic float fusion_power = 0.03f, fusion_range = 0.4f;
HLE_FUNC("_ZN2nn3hid25EnableSixAxisSensorFusionERKNS0_19SixAxisSensorHandleEb", hid_six_axis_fusion) { fusion_enabled = c->x[1] != 0; }
HLE_FUNC("_ZN2nn3hid28IsSixAxisSensorFusionEnabledERKNS0_19SixAxisSensorHandleE", hid_six_axis_fusion_on) { c->x[0] = fusion_enabled; }
HLE_FUNC("_ZN2nn3hid32SetSixAxisSensorFusionParametersERKNS0_19SixAxisSensorHandleEff", hid_six_axis_set_params) {
    float power, range;
    memcpy(&power, &c->v[0], sizeof power);
    memcpy(&range, &c->v[1], sizeof range);
    fusion_power = power; fusion_range = range;
}
HLE_FUNC("_ZN2nn3hid32GetSixAxisSensorFusionParametersEPfS1_RKNS0_19SixAxisSensorHandleE", hid_six_axis_get_params) {
    if (c->x[0]) *(float *)(uintptr_t)c->x[0] = fusion_power;
    if (c->x[1]) *(float *)(uintptr_t)c->x[1] = fusion_range;
}
HLE_FUNC("_ZN2nn3hid25SetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleERKNS0_22GyroscopeZeroDriftModeE", hid_gyro_drift_set) { gyro_drift = *(const uint32_t *)(uintptr_t)c->x[1]; }
HLE_FUNC("_ZN2nn3hid25GetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleE", hid_gyro_drift_get) { c->x[0] = gyro_drift; }
