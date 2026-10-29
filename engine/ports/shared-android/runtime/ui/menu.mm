#include "keyboard_text.h"
/* Derived from LAS Recomp Android at cca791a3e7218e1dac95e7d4a11b8f97dba3ea51.
 * Copyright (c) 2026 Peiikai. MIT: native/runtime/LICENSE-LASRecomp.
 * Shared runtime: LAS-specific features run only for the selected LAS adapter. */
// In-game overlay menu built with Dear ImGui (Metal renderer on macOS, Vulkan elsewhere; input fed from SDL events).
//
// Threads: SDL events arrive on the main thread and are queued; the menu is laid out and drawn on the
// render thread inside backend_present, after the game image, into the same drawable.
#ifdef __APPLE__
#import <Metal/Metal.h>
#endif
#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <atomic>
#include <string>
#include <vector>
#include "touch_ui.h"
#ifdef __ANDROID__
#include <string>
#endif

#include "imgui.h"
#include "i18n.h"
#include "option_rows.h"
#include "imgui_internal.h"  // (NavCursorVisible)
#include "osk.h"
#include "pad_nav.h"
#ifdef __APPLE__
#include "imgui_impl_metal.h"
#else
#include "imgui_impl_vulkan.h"
extern "C" bool vk_imgui_backend_init(void);  // runtime/nvn/vulkan/imgui_glue.cpp
extern "C" void rec_drain(void);             // runtime/nvn/vulkan/record.c
#endif

extern "C" {
#include "../hle.h"
#include "../common_session.h"
#include "../settings.h"
#include "menu.h"
void input_reset(void);
void save_editor_draw(float scale);  // save_editor.mm
extern "C" int game_in_play(void);  // hle/warp.c
void save_editor_on_open(void);
void rando_tab_draw(float scale);
bool warp_tab_draw(float scale);  // warp_tab.mm: true when a warp started
extern "C" void cheats_draw(float scale);  // cheats.mm
extern "C" void difficulty_draw(float scale);  // difficulty.mm
void controls_tab_draw(float scale);  // controls_ui.mm
bool controls_ui_took_back(void);
bool rando_tab_took_back(void);  // rando_tab.mm: B went back from the Triforce hunt's count
#include "../input_bind.h"
#include "../paths.h"
#ifdef __ANDROID__
#include "../platform/android.h"
bool vk_gpu_driver_custom(const char **failed);  // nvn/vulkan/device.c
#endif
}

#ifndef __APPLE__
extern "C" {
SDL_Window *ui_window(void);
bool vk_frame_begin(const float clear[4]);
void vk_frame_end(const char *shot_path);
void *vk_frame_cmd(void);
void vk_headstart_prepare(void (*progress)(int done, int total), bool (*cancelled)(void));
}
#endif

// ------------------------------------------------------------------ state shared with the main thread
static std::atomic<bool> g_open{false};
static std::atomic<bool> g_text_busy{false};  // a text field / the on-screen keyboard has the keys (render thread: ui_text_busy)
static std::atomic<bool> g_want_fullscreen_apply{false};

struct QEvent {
    enum { KEY, ANALOG, MOUSE_POS, MOUSE_BUTTON, WHEEL, FINGER, TEXT } kind;
    SDL_Event finger;  // (FINGER: the event itself - touch_ui.h runs on the render thread)
    ImGuiKey key;
    bool down;
    float value, x, y;
    int button;
    char text[32];
};
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static std::vector<QEvent> q_events;
static float g_pixel_density = 1;
static int g_win_px_w = 1, g_win_px_h = 1;  // (touch positions come normalized)

static void push(const QEvent &e) {
    pthread_mutex_lock(&q_lock);
    q_events.push_back(e);
    pthread_mutex_unlock(&q_lock);
}

#ifndef __APPLE__
extern "C" double vk_shown_fps(void);  // (vulkan/device.c: every picture shown, generated ones too: the FPS counter)
#endif
bool menu_is_open(void) { return g_open.load(); }
static void set_open(bool open);
extern "C" void menu_toggle(void) { set_open(!g_open.load()); }  // (the touch controls' menu button)
extern "C" void menu_test_open(void) {
    set_open(true);
    if (getenv("LARECOMP_MENU_TAB")) { extern int g_tab_request_ext; g_tab_request_ext = atoi(getenv("LARECOMP_MENU_TAB")); }
}

void menu_tabs_focus(bool first_tab);
extern "C" void touch_reset(void);  // touch_controls.mm
extern "C" bool touch_edit_back(void);
static void set_open(bool open) {
    if (open == g_open.load()) return;
    g_open.store(open);
    input_reset();  // nothing held while the menu had focus leaks into the game
    touch_reset();  // (nor touches: the menu takes them, their lifts included)
    bind_capture_cancel();
    if (open) {
        if (shared_title_las()) save_editor_on_open();
        static bool opened;  // the controller's cursor: the first tab (Controls) the first time this session, later the
        menu_tabs_focus(!opened);  // selected one
        opened = true;
    }
    else { settings_save(); osk_cancel(); g_text_busy = false; }
}

static ImGuiKey key_from_scancode(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
    case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
    case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
    case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
    case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
    case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
    case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;
    case SDL_SCANCODE_HOME: return ImGuiKey_Home;
    case SDL_SCANCODE_END: return ImGuiKey_End;
    case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
    case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
    case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
    case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
    case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
    case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
    case SDL_SCANCODE_LGUI: return ImGuiKey_LeftSuper;
    case SDL_SCANCODE_RGUI: return ImGuiKey_RightSuper;
    case SDL_SCANCODE_A: return ImGuiKey_A;
    case SDL_SCANCODE_C: return ImGuiKey_C;
    case SDL_SCANCODE_V: return ImGuiKey_V;
    case SDL_SCANCODE_X: return ImGuiKey_X;
    case SDL_SCANCODE_Z: return ImGuiKey_Z;
    case SDL_SCANCODE_Q: return ImGuiKey_Q;  // previous tab
    case SDL_SCANCODE_E: return ImGuiKey_E;  // next tab
    default: return ImGuiKey_None;
    }
}


// ---- software keyboard (runtime/hle/swkbd.c: the game's name entry etc.) --------------------------------------
// The game thread waits in menu_keyboard_run (re-presenting the last frame); this dialog takes the text: typed
// directly when the player last used a keyboard, an on-screen keyboard (d-pad + confirm, back deletes, Start / + OK; buttons as in pad_nav.h) when
// they last used a controller.
static std::atomic<int> g_kbd{0};  // 1 open, 2 confirmed, 3 cancelled
static std::atomic<bool> g_last_pad{false};
static char g_kbd_text[96];
static int g_kbd_max = 8;
static bool capturing() { return g_open.load() || g_kbd.load() == 1; }
// The game gets no input while this is true (runtime/hle/input.c): menu or name entry up.
extern "C" bool menu_input_captured(void) { return capturing(); }

bool menu_handle_event(const SDL_Event *e) {
    static bool back_down, start_down;
    if (g_open.load() && bind_capture_event(e)) return true;  // Controls tab waiting for a new binding: it takes the press
    if (e->type == SDL_EVENT_KEY_DOWN) g_last_pad = false;
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) g_last_pad = true;
    if (g_kbd.load() == 1 && e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {  // Enter confirms, Esc cancels
        if (e->key.scancode == SDL_SCANCODE_RETURN || e->key.scancode == SDL_SCANCODE_KP_ENTER) { g_kbd = 2; return true; }
        if (e->key.scancode == SDL_SCANCODE_ESCAPE) { g_kbd = 3; return true; }
    }
    if (g_kbd.load() == 1 && e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e->gbutton.button == SDL_GAMEPAD_BUTTON_START) {
        g_kbd = 2;
        return true;
    }
    // typing (a text field, the on-screen keyboard, the name entry): keys are text, not menu shortcuts
    bool typing = g_kbd.load() == 1 || (g_open.load() && g_text_busy.load());
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        // Android's back button: cancels the touch layout editor, opens the menu, and in it backs out as Esc does
        if (e->key.scancode == SDL_SCANCODE_AC_BACK) {
            if (e->key.repeat) return true;
            if (touch_edit_back()) return true;
            if (!g_open.load()) { if (!typing) set_open(true); return true; }
        }
        if (!e->key.repeat && (e->key.scancode == SDL_SCANCODE_F1 ||
                               (e->key.scancode == SDL_SCANCODE_ESCAPE && !g_open.load()))) {
            if (!typing) set_open(!g_open.load());  // (Esc with the menu open goes to ImGui below: it cancels a field)
            return true;
        }
        if (!e->key.repeat && (e->key.scancode == SDL_SCANCODE_F11 ||
                               (e->key.scancode == SDL_SCANCODE_RETURN && (e->key.mod & (SDL_KMOD_ALT | SDL_KMOD_GUI))) ||
                               (e->key.scancode == SDL_SCANCODE_F && (e->key.mod & SDL_KMOD_GUI) && (e->key.mod & SDL_KMOD_CTRL)))) {
            g_settings.fullscreen = !g_settings.fullscreen;
            g_want_fullscreen_apply.store(true);
            settings_save();
            return true;
        }
        /* fallthrough */
    case SDL_EVENT_KEY_UP: {
        if (!capturing()) return false;
        bool down = e->type == SDL_EVENT_KEY_DOWN;
        if (e->key.scancode == SDL_SCANCODE_AC_BACK && !down) return true;
        if (down && (e->key.scancode == SDL_SCANCODE_ESCAPE || e->key.scancode == SDL_SCANCODE_AC_BACK)) {  // Esc: back out of popups, else close
            QEvent q = {};
            q.kind = QEvent::KEY; q.key = ImGuiKey_Escape; q.down = true; push(q);
            q.down = false; push(q);
            return true;
        }
        ImGuiKey k = key_from_scancode(e->key.scancode);
        if (k != ImGuiKey_None) { QEvent q = {}; q.kind = QEvent::KEY; q.key = k; q.down = down; push(q); }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::TEXT; strlcpy(q.text, e->text.text, sizeof q.text); push(q); }
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        bool down = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_BACK) back_down = down;
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_START) start_down = down;
        if (down && !typing && ((back_down && start_down) || e->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE)) {
            set_open(!g_open.load());
            return true;
        }
        if (!capturing()) return false;
        ImGuiKey k = ui_pad_key(e->gbutton.which, e->gbutton.button, down);  // (confirm / back: pad_nav.h)
        if (k != ImGuiKey_None) { QEvent q = {}; q.kind = QEvent::KEY; q.key = k; q.down = down; push(q); }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (!capturing()) return false;
        float v = e->gaxis.value / 32767.0f;
        QEvent q = {};
        q.kind = QEvent::ANALOG;
        switch (e->gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX:
            q.key = ImGuiKey_GamepadLStickLeft; q.value = v < -0.3f ? -v : 0; push(q);
            q.key = ImGuiKey_GamepadLStickRight; q.value = v > 0.3f ? v : 0; push(q);
            break;
        case SDL_GAMEPAD_AXIS_LEFTY:
            q.key = ImGuiKey_GamepadLStickUp; q.value = v < -0.3f ? -v : 0; push(q);
            q.key = ImGuiKey_GamepadLStickDown; q.value = v > 0.3f ? v : 0; push(q);
            break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: q.key = ImGuiKey_GamepadL2; q.value = v; push(q); break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: q.key = ImGuiKey_GamepadR2; q.value = v; push(q); break;
        default: break;
        }
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::MOUSE_POS; q.x = e->motion.x * g_pixel_density; q.y = e->motion.y * g_pixel_density; push(q); }
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        if (!capturing()) return false;
        {
            QEvent q = {};
            q.kind = QEvent::MOUSE_BUTTON;
            q.button = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1 : 2;
            q.down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            push(q);
        }
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::WHEEL; q.x = e->wheel.x; q.y = e->wheel.y; push(q); }
        return true;
#ifdef __ANDROID__
    case SDL_EVENT_FINGER_DOWN: case SDL_EVENT_FINGER_UP: case SDL_EVENT_FINGER_MOTION: case SDL_EVENT_FINGER_CANCELED:
        if (!capturing()) return false;
        {   // touch screens (touch_ui.h, on the render thread: drain_events)
            QEvent q = {};
            q.kind = QEvent::FINGER, q.finger = *e;
            push(q);
        }
        return true;
#endif
    default:
        return false;
    }
}

extern "C" uint32_t g_window_resize_request;  // (test scripts: runtime/hle/input.c "resize")
void menu_apply_window_requests(void *win) {
    SDL_Window *w = (SDL_Window *)win;
    g_pixel_density = SDL_GetWindowPixelDensity(w);
    SDL_GetWindowSizeInPixels(w, &g_win_px_w, &g_win_px_h);
    static bool text_input;
#ifdef __ANDROID__
    // phones: text input shows the on-screen keyboard - only while a text field has the focus, not the whole time
    // the menu is open (opening Settings brought the keyboard up)
    bool want_text = capturing() && g_text_busy.load();
#else
    bool want_text = capturing();
#endif
    if (want_text != text_input) {
        if (want_text) SDL_StartTextInput(w);
        else SDL_StopTextInput(w);
        text_input = want_text;
    }
    static bool first = true;
    if (first || g_want_fullscreen_apply.exchange(false)) {
        first = false;
        SDL_SetWindowFullscreen(w, g_settings.fullscreen);
    }
    if (uint32_t r = __atomic_exchange_n(&g_window_resize_request, 0u, __ATOMIC_RELAXED))
        SDL_SetWindowSize(w, (int)(r >> 16), (int)(r & 0xffff));
}

// ------------------------------------------------------------------ render thread
static bool g_inited;
static ImGuiContext *g_ctx;  // ours: the start screen (back to it after a reboot) has its own context
static ImFont *g_font, *g_font_big;
static ImFont *g_menu_font, *g_menu_font_big;  // the menu window (Baloo 2); everything else keeps g_font
static int g_tab, g_tab_request = -2;  // (-2: no request; -1 is the Prelaunch tab)
static int g_focus_tab = -99;  // put the nav cursor on this tab next frame (-100: the selected one; -99: none)
static int g_focus_frames;

// The start screen's Settings opening: the controller's cursor starts on the first tab the first time in a session,
// later on the tab that's selected
static bool g_page_fresh_seen;  // (focus_check: the menu opened)
static bool g_page_fresh;  // the page shown hasn't been entered yet: the first Down goes to its first enabled row
static bool g_page_focus_next;  // (that page has no option rows: its first control takes the cursor next frame)
static int g_shown_tab = -1000;  // the page drawn last (its id)
void menu_tabs_focus(bool first_tab) {
    g_focus_tab = first_tab ? -1 : -100, g_focus_frames = 0, g_page_fresh = g_page_fresh_seen = true;
}
int g_tab_request_ext = -1;

// Phones: the menu a third larger than on a 1080-line monitor (a 6.5" screen held at arm's length: rows to touch, text
// to read), its fonts built at that size (sharp, not scaled up)
#ifdef __ANDROID__
static const float kUi = 1.35f;
#else
static const float kUi = 1.0f;
#endif

static void style_init(void) {
    ImGuiStyle &s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 14; s.FrameRounding = 8; s.GrabRounding = 8; s.TabRounding = 8; s.ChildRounding = 10;
    s.WindowPadding = ImVec2(28, 24); s.FramePadding = ImVec2(14, 8); s.ItemSpacing = ImVec2(14, 12);
    s.WindowBorderSize = 0; s.ScrollbarSize = 18;
    ImVec4 *c = s.Colors;
    ImVec4 gold(0.93f, 0.76f, 0.33f, 1), gold_dim(0.62f, 0.49f, 0.20f, 1), green(0.15f, 0.36f, 0.25f, 1);
    c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.09f, 0.08f, 0.94f);
    c[ImGuiCol_ChildBg] = ImVec4(0.09f, 0.13f, 0.11f, 0.85f);
    c[ImGuiCol_PopupBg] = ImVec4(0.07f, 0.10f, 0.09f, 0.98f);
    c[ImGuiCol_Text] = ImVec4(0.96f, 0.94f, 0.88f, 1);
    c[ImGuiCol_TextDisabled] = ImVec4(0.62f, 0.64f, 0.58f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.13f, 0.19f, 0.16f, 1);
    c[ImGuiCol_FrameBgHovered] = green;
    c[ImGuiCol_FrameBgActive] = ImVec4(0.19f, 0.45f, 0.31f, 1);
    c[ImGuiCol_Button] = ImVec4(0.14f, 0.22f, 0.18f, 1);
    c[ImGuiCol_ButtonHovered] = green;
    c[ImGuiCol_ButtonActive] = ImVec4(0.21f, 0.50f, 0.34f, 1);
    c[ImGuiCol_Header] = green;
    c[ImGuiCol_HeaderHovered] = ImVec4(0.19f, 0.45f, 0.31f, 1);
    c[ImGuiCol_HeaderActive] = ImVec4(0.21f, 0.50f, 0.34f, 1);
    c[ImGuiCol_Tab] = ImVec4(0.12f, 0.18f, 0.15f, 1);
    c[ImGuiCol_TabHovered] = green;
    c[ImGuiCol_TabSelected] = ImVec4(0.21f, 0.44f, 0.31f, 1);
    c[ImGuiCol_TabSelectedOverline] = gold;
    c[ImGuiCol_CheckMark] = gold;
    c[ImGuiCol_SliderGrab] = gold_dim;
    c[ImGuiCol_SliderGrabActive] = gold;
    c[ImGuiCol_NavCursor] = gold;
    c[ImGuiCol_Separator] = ImVec4(0.30f, 0.36f, 0.30f, 1);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_WindowBg];
}


#ifdef __APPLE__
static void init(id<MTLDevice> dev) {
#else
static void init(void) {
#endif
    IMGUI_CHECKVERSION();
    g_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.IniFilename = NULL;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    const char *paths[] = {"/System/Library/Fonts/Avenir Next.ttc", "/System/Library/Fonts/Helvetica.ttc",
                           "C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                           "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf"};
    // (Android has none of those: the DejaVu Sans the APK carries, copied out with the app's resources - else the
    // name entry's text, the FPS counter and the overlays came out in ImGui's 13 px default font)
    char dejavu[1024];
    if (rt_resource("DejaVuSans.ttf", NULL, dejavu, sizeof dejavu)) {
        g_font = io.Fonts->AddFontFromFileTTF(dejavu, 34, &cfg);
        ImFontConfig b = cfg;
        g_font_big = io.Fonts->AddFontFromFileTTF(dejavu, 56, &b);
    }
    for (const char *p : paths) {
        if (FILE *f = fopen(p, "rb")) fclose(f);  // a missing file is an assertion in ImGui, not a NULL return
        else continue;
        if (!g_font) g_font = io.Fonts->AddFontFromFileTTF(p, 34, &cfg);
        if (g_font && !g_font_big) { ImFontConfig b = cfg; b.FontNo = 0; g_font_big = io.Fonts->AddFontFromFileTTF(p, 56, &b); }
    }
    // the menu window itself: Baloo 2 SemiBold, as on the start screen (shipped, OFL), at its Settings window's sizes; the
    // FPS counter, overlays and on-screen keyboard keep the fonts above
    char baloo[1024];
    if (const char *f = getenv("LARECOMP_MENU_FONT")) strlcpy(baloo, f, sizeof baloo);  // (test) another weight / face
    if (getenv("LARECOMP_MENU_FONT") || rt_resource("Baloo2-SemiBold.ttf", "third_party/fonts/Baloo2/Baloo2-SemiBold.ttf", baloo, sizeof baloo)) {
        g_menu_font = io.Fonts->AddFontFromFileTTF(baloo, 39 * kUi, &cfg);
        g_menu_font_big = io.Fonts->AddFontFromFileTTF(baloo, 72 * kUi, &cfg);
    }
    if (!g_font) g_font = io.Fonts->AddFontDefault();
    if (!g_font_big) g_font_big = g_font;
    if (!g_menu_font || !g_menu_font_big) g_menu_font = g_font, g_menu_font_big = g_font_big;
    style_init();
#ifdef __APPLE__
    ImGui_ImplMetal_Init(dev);
#else
    vk_imgui_backend_init();
#endif
    g_inited = true;
}

#ifndef __APPLE__
static std::atomic<int> g_prep_done{-1}, g_prep_total{};
static std::atomic<bool> g_prep_finished{};
static std::atomic<bool> g_prep_cancel{};
extern "C" int rt_reboot_requested(void);
extern "C" void input_handle_event(const SDL_Event *event);

enum StartupPrepAction { PREP_WAIT, PREP_CONTINUE, PREP_QUIT };
struct StartupPrepTouch { SDL_FingerID id = 0; bool active = false; };

static void startup_prep_progress(int done, int total) {
    g_prep_done.store(done, std::memory_order_relaxed);
    g_prep_total.store(total, std::memory_order_relaxed);
}

static bool startup_prep_cancelled(void) {
    return g_prep_cancel.load(std::memory_order_relaxed);
}

static void *startup_prep_worker(void *) {
    vk_headstart_prepare(startup_prep_progress, startup_prep_cancelled);
    g_prep_finished.store(true, std::memory_order_release);
    return NULL;
}

static void startup_prep_clear_input(void) {
    if (!g_inited) return;
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.ClearEventsQueue();
    io.ClearInputKeys();
    io.ClearInputMouse();
    ImGui::ClearActiveID();
}

// This loop owns SDL/ImGui before guest threads start; do not run in-game menu shortcuts.
static void startup_prep_input(const SDL_Event *event, StartupPrepTouch &touch) {
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO &io = ImGui::GetIO();
    bool down = event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    switch (event->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
        ImGuiKey key = key_from_scancode(event->key.scancode);
        if (key != ImGuiKey_None) io.AddKeyEvent(key, down);
        break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        ImGuiKey key = ui_pad_key(event->gbutton.which, event->gbutton.button, down);
        if (key != ImGuiKey_None) io.AddKeyEvent(key, down);
        break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        float value = event->gaxis.value / 32767.0f;
        if (event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, value < -0.3f, value < -0.3f ? -value : 0);
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, value > 0.3f, value > 0.3f ? value : 0);
        } else if (event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, value < -0.3f, value < -0.3f ? -value : 0);
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, value > 0.3f, value > 0.3f ? value : 0);
        }
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (event->motion.which != SDL_TOUCH_MOUSEID) {
            io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
            io.AddMousePosEvent(event->motion.x * SDL_GetWindowPixelDensity(ui_window()),
                                event->motion.y * SDL_GetWindowPixelDensity(ui_window()));
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event->button.which != SDL_TOUCH_MOUSEID && event->button.button == SDL_BUTTON_LEFT) {
            io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
            io.AddMousePosEvent(event->button.x * SDL_GetWindowPixelDensity(ui_window()),
                                event->button.y * SDL_GetWindowPixelDensity(ui_window()));
            io.AddMouseButtonEvent(0, event->type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        break;
    case SDL_EVENT_FINGER_DOWN: case SDL_EVENT_FINGER_MOTION: case SDL_EVENT_FINGER_UP: case SDL_EVENT_FINGER_CANCELED: {
        if (event->type == SDL_EVENT_FINGER_DOWN) {
            if (touch.active) break;
            touch.id = event->tfinger.fingerID, touch.active = true;
        }
        if (!touch.active || event->tfinger.fingerID != touch.id) break;
        if (event->type == SDL_EVENT_FINGER_CANCELED) {
            touch.active = false;
            startup_prep_clear_input();
            break;
        }
        int width = 1, height = 1;
        SDL_GetWindowSizeInPixels(ui_window(), &width, &height);
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(event->tfinger.x * width, event->tfinger.y * height);
        if (event->type == SDL_EVENT_FINGER_DOWN) io.AddMouseButtonEvent(0, true);
        if (event->type == SDL_EVENT_FINGER_UP) { io.AddMouseButtonEvent(0, false); touch.active = false; }
        break;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        touch.active = false;
        startup_prep_clear_input();
        io.AddFocusEvent(false);
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        io.AddFocusEvent(true);
        break;
    default: break;
    }
}

static StartupPrepAction startup_prep_draw(int done, int total, StartupPrepAction action) {
    if (!g_inited) init();
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO &io = ImGui::GetIO();
    int width = 1, height = 1;
    SDL_GetWindowSizeInPixels(ui_window(), &width, &height);
    io.DisplaySize = ImVec2((float)width, (float)height);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    io.DeltaTime = 1.0f / 60.0f;
    float scale = fmaxf(0.45f, height / 1080.0f);
    io.FontGlobalScale = scale;
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(width * 0.5f, height * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(fminf(width * 0.86f, 760.0f * scale), 0), ImGuiCond_Always);
    ImGui::Begin("##shader-prep", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushFont(g_menu_font_big);
    const ImVec4 accent(0.84f, 0.29f, 0.43f, 1.0f);
    ImGui::TextColored(accent, "%s", action == PREP_QUIT ? "Closing game" :
                        action == PREP_CONTINUE ? "Starting game" : "Preparing shaders");
    ImGui::PopFont();
    if (action == PREP_CONTINUE) {
        ImGui::TextWrapped("Shader builds can continue in the background.");
    } else if (action == PREP_QUIT) {
        ImGui::TextWrapped("Finishing shader work before closing.");
    } else if (total > 0) {
        int completed = done < 0 ? 0 : done > total ? total : done;
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, accent);
        ImGui::ProgressBar((float)completed / (float)total, ImVec2(-1, 0), "");
        ImGui::PopStyleColor();
        ImGui::TextDisabled("%d of %d known shader records finished", completed, total);
    } else {
        const float phase = (float)(SDL_GetTicks() % 900) / 900.0f;
        ImGui::ProgressBar(phase, ImVec2(-1, 0), "");
        ImGui::TextDisabled("Checking the shader cache...");
    }
    if (action == PREP_WAIT) {
        ImGui::Spacing();
        float button_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        float button_height = fmaxf(56 * scale, 48 * SDL_GetWindowPixelDensity(ui_window()));
        if (ImGui::Button("Continue", ImVec2(button_width, button_height))) action = PREP_CONTINUE;
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("Cancel game", ImVec2(button_width, button_height))) action = PREP_QUIT;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) action = PREP_QUIT;
        ImGui::TextWrapped("Continue skips the wait. Cancel closes the game.");
    }
    ImGui::End();
    ImGui::Render();
    static const float clear[4] = {0.035f, 0.045f, 0.055f, 1.0f};
    if (vk_frame_begin(clear)) {
        rec_drain();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)vk_frame_cmd());
        vk_frame_end(NULL);
    }
    return action;
}

extern "C" bool menu_shader_prep_startup(void) {
    g_prep_done = -1;
    g_prep_total = 0;
    g_prep_finished = false;
    g_prep_cancel.store(false, std::memory_order_relaxed);
    if (rt_reboot_requested()) return false;
    pthread_t worker;
    if (pthread_create(&worker, NULL, startup_prep_worker, NULL)) return !rt_reboot_requested();

    const uint64_t started = SDL_GetTicksNS();
    StartupPrepAction action = PREP_WAIT;
    StartupPrepTouch touch;
    bool shown = false, backgrounded = false;
    unsigned completed_frames = 0;
    for (;;) {
        if (rt_reboot_requested()) action = PREP_QUIT;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) action = PREP_QUIT;
            if (event.type == SDL_EVENT_GAMEPAD_ADDED || event.type == SDL_EVENT_GAMEPAD_REMOVED) {
                input_handle_event(&event);
                if (event.type == SDL_EVENT_GAMEPAD_REMOVED) startup_prep_clear_input();
            }
            if (event.type == SDL_EVENT_WINDOW_HIDDEN || event.type == SDL_EVENT_WINDOW_MINIMIZED ||
                event.type == SDL_EVENT_WILL_ENTER_BACKGROUND) {
                backgrounded = true;
                touch.active = false;
                startup_prep_clear_input();
            }
            if (event.type == SDL_EVENT_WINDOW_SHOWN || event.type == SDL_EVENT_WINDOW_RESTORED ||
                event.type == SDL_EVENT_DID_ENTER_FOREGROUND)
                backgrounded = false;
            if (action != PREP_QUIT && ((event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                 (event.key.scancode == SDL_SCANCODE_AC_BACK || event.key.scancode == SDL_SCANCODE_ESCAPE)) ||
                (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && event.gbutton.button == SDL_GAMEPAD_BUTTON_BACK)))
                action = PREP_CONTINUE;
            if (shown && g_inited && !backgrounded && action == PREP_WAIT) startup_prep_input(&event, touch);
        }
        if (action != PREP_WAIT) g_prep_cancel.store(true, std::memory_order_relaxed);
        bool finished = g_prep_finished.load(std::memory_order_acquire);
        if (!shown && !finished && SDL_GetTicksNS() - started >= 150000000ull) shown = true;
        if (shown && !backgrounded) {
            action = startup_prep_draw(g_prep_done.load(std::memory_order_relaxed),
                                      g_prep_total.load(std::memory_order_relaxed), action);
            if (action != PREP_WAIT) g_prep_cancel.store(true, std::memory_order_relaxed);
        }
        if (finished) {
            // A queued Cancel release still belongs to this screen when builds
            // finish. ImGui may trickle a tap over several frames; settle its
            // existing queue before launch, without an unbounded completion wait.
            bool pending = shown && !backgrounded && action == PREP_WAIT && g_inited &&
                           g_ctx->InputEventsQueue.Size != 0;
            if (!pending || ++completed_frames >= 4) break;
        }
        SDL_Delay(backgrounded ? 100 : 16);
    }
    pthread_join(worker, NULL);
    if (rt_reboot_requested()) action = PREP_QUIT;
    // Do not draw a completed bar for a skipped wait. Terminal cleanup drains the
    // headstart worker in main.c before destroying its Vulkan device.
    if (shown && !backgrounded && action != PREP_WAIT)
        startup_prep_draw(g_prep_done.load(std::memory_order_relaxed),
                          g_prep_total.load(std::memory_order_relaxed), action);
    startup_prep_clear_input();
    input_reset();
    return action != PREP_QUIT;
}
#else
extern "C" bool menu_shader_prep_startup(void) { return true; }
#endif

static TouchUi g_touch;  // (render thread)
static void drain_events(ImGuiIO &io) {
    pthread_mutex_lock(&q_lock);
    std::vector<QEvent> ev;
    ev.swap(q_events);
    pthread_mutex_unlock(&q_lock);
    for (const QEvent &q : ev) {
        switch (q.kind) {
        case QEvent::KEY: io.AddKeyEvent(q.key, q.down); break;
        case QEvent::ANALOG: io.AddKeyAnalogEvent(q.key, q.value > 0.1f, q.value); break;
        case QEvent::MOUSE_POS: io.AddMousePosEvent(q.x, q.y); break;
        case QEvent::MOUSE_BUTTON: io.AddMouseButtonEvent(q.button, q.down); break;
        case QEvent::WHEEL: io.AddMouseWheelEvent(q.x, q.y); break;
        case QEvent::FINGER:
            g_touch.event(&q.finger, (float)g_win_px_w, (float)g_win_px_h, [&](float x, float y) { io.AddMousePosEvent(x, y); },
                          [&](bool down) { io.AddMouseButtonEvent(0, down); });
            break;
        case QEvent::TEXT: io.AddInputCharactersUTF8(q.text); break;
        }
    }
}

// Section header in the big font.
static void header(const char *t) {
    ImGui::PushFont(g_font_big);
    ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "%s", T(t));
    ImGui::PopFont();
    ImGui::Separator();
    ImGui::Spacing();
}

static void hint(const char *t) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", T(t));
    ImGui::PopTextWrapPos();
}

// A row with a label on the left and a choice of options on the right (dpad left/right cycles).
// reserve: room kept free right of the combo (a button on the same line). Label and names are shown through T().
static bool choice(const char *label, int *value, const char *const *names, int n, float reserve = 0) {
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T(label));
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.45f);
    bool changed = false;
    ImGui::SetNextItemWidth(reserve > 0 ? -reserve : -1);
    if (ImGui::BeginCombo("##c", T(names[*value]))) {
        for (int i = 0; i < n; i++) {
            ImGui::PushID(i);
            if (ImGui::Selectable(T(names[i]), i == *value)) { *value = i; changed = true; }
            ImGui::PopID();
            if (i == *value) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemFocused() && !ImGui::IsPopupOpen("##c")) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
            *value = (*value + 1) % n; changed = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
            *value = (*value + n - 1) % n; changed = true;
        }
    }
    ImGui::PopID();
    return changed;
}

static bool toggle(const char *label, bool *v) {
    const char *names[] = {"Off", "On"};
    int i = *v;
    bool ch = choice(label, &i, names, 2);
    *v = i;
    return ch;
}

#ifdef __ANDROID__
// ---- the GPU driver (any GPU): the system's or one the player installed (GpuDrivers.java). The Vulkan driver is chosen
// before anything is drawn, so a change applies at the next start. The Java side is asked once, and after a change.
static std::atomic<bool> g_driver_reread{true};
static pthread_mutex_t g_driver_lock = PTHREAD_MUTEX_INITIALIZER;
static std::string g_driver_msg;  // the last install's result

static void driver_installed(const char *err) {  // (LasActivity's install thread)
    if (err && !err[0]) return;  // (cancelled)
    pthread_mutex_lock(&g_driver_lock);
    g_driver_msg = err ? std::string(T("Not installed: ")) + err : T("Installed and selected: it's used from the next start.");
    pthread_mutex_unlock(&g_driver_lock);
    g_driver_reread = true;
}

static void gpu_driver_rows(void) {
    static bool supported, use;
    static std::string name;
    if (g_driver_reread.exchange(false)) {
        supported = android_gpu_driver_supported();
        const char *n = android_gpu_driver_name();
        name = n ? n : "";
        use = android_gpu_driver_selected();
    }
    opt_section("GPU driver");
    if (!supported) {
        opt_disable_next();
        opt_action("GPU driver", "System", "Installed GPU drivers (such as Mesa Turnip) work on Adreno GPUs only.");
        return;
    }
    const char *failed;
    bool active = vk_gpu_driver_custom(&failed);
    std::string d = T("The Vulkan driver the game renders with. System: your device's own. An installed driver (such as "
                      "Mesa Turnip for Adreno or Mesa PanVK for Mali) can be faster or fix glitches - or be slower; one "
                      "that doesn't start is replaced by System at the next start.\n\nChanges apply at the next start of "
                      "the app. Running now: ");
    d += active ? (name.empty() ? T("Turnip (built in)") : name) : T("System");  // (no installed one: the APK's own runs)
    if (failed) d += std::string(".\n\n") + failed + T(" didn't start: System runs instead");
    d += ".";
    std::string names[2] = {T("System"), name};
    if (use != active) names[use] += T(" (next start)");
    const char *np[2] = {names[0].c_str(), names[1].c_str()};
    int v = use;
    if (name.empty()) opt_disable_next(), opt_action("GPU driver", active ? "Turnip (built in)" : "System", d.c_str());
    else if (opt_choice("GPU driver", &v, np, 2, 0, d.c_str())) android_gpu_driver_select(use = v);

    pthread_mutex_lock(&g_driver_lock);
    std::string id = T("Installs a driver package from your files: a .zip as other emulators use them (meta.json and the "
                       "driver's .so), replacing the one installed before, and selects it. It's used from the next start "
                       "of the app.");
    if (!g_driver_msg.empty()) id += "\n\n" + g_driver_msg;
    pthread_mutex_unlock(&g_driver_lock);
    if (opt_action("Install driver...", "", id.c_str())) android_gpu_driver_install(driver_installed);
}
#endif

extern "C" void renderer_get_scale(float *sx, float *sy);
extern "C" int input_pad_family(void);  // runtime/hle/input.c
extern "C" double output_scale_dynamic_factor(void);

static void tab_display(void) {
    float scale = ImGui::GetFontSize() / 34;
    opt_begin("##video", scale);
    opt_section("Display");
    if (shared_title_las()) {
    const char *aspects[] = {"Unlocked", "Original 16:9"};
    opt_choice("Aspect ratio", &g_settings.aspect, aspects, 2, ASPECT_UNLOCKED,
               "Unlocked: the view fills the window, so wider (or taller) windows show more of the island - up to "
               "ultrawide; houses and dungeons keep their framing. Original 16:9: the game's own framing, with bars.");
    }
    bool fs = g_settings.fullscreen;
    if (opt_toggle("Fullscreen", &fs, false, "Fills the screen. Also: F11, Alt+Enter or Cmd+Ctrl+F.")) {
        g_settings.fullscreen = fs;
        g_want_fullscreen_apply.store(true);
    }
    opt_toggle("Show FPS", &g_settings.show_fps, false, "Shows the frame rate in the top right corner.");

    opt_section("Rendering");
    // display order -> RES_* (the ini / enum order predates the menu order)
    // (up to 4x: 7680 x 4320, 8K - more only ran out of GPU memory, and sat next to 75% in the list)
    // (Match display in the middle: left from it 75% / 50% / 25%, right Dynamic then 1x-4x; user 2026-10-11)
    static const char *res[] = {"25%", "50%", "75%", "Match display", "Dynamic", "1x", "1.25x", "1.5x", "2x", "3x", "4x"};
    static const int res_mode[] = {RES_Q25, RES_Q50, RES_Q75, RES_MATCH, RES_DYNAMIC, 1, RES_X125, RES_X150, 2, 3, 4};
    enum { RES_MATCH_AT = 3 };
    enum { NRES = sizeof res_mode / sizeof res_mode[0] };
    {
        int ri = RES_MATCH_AT, rdef = RES_MATCH_AT;  // (rdef: the GPU's default - Dynamic on an integrated GPU)
        for (int i = 0; i < NRES; i++) {
            if (res_mode[i] == g_settings.res_mode) ri = i;
            if (res_mode[i] == g_default_res_mode) rdef = i;
        }
        float sx, sy;
        renderer_get_scale(&sx, &sy);
        char d[400];
        int n = snprintf(d, sizeof d, T("How sharp the 3D image is. Match display: your screen's own resolution. Dynamic: "
                                        "as sharp as your GPU keeps at full speed. 1x-4x: the Switch's 1080p times that%s. "
                                        "75%% / 50%% / 25%%: a share of the display, for slower machines.\n\nRendering at "
                                        "%.0f x %.0f."), g_gpu_integrated ? T(" (1x: 720p on this GPU)") : "", 1920.0 * sx, 1080.0 * sy);
        if (g_settings.res_mode == RES_DYNAMIC)
            snprintf(d + n, sizeof d - n, T(" Dynamic: %.0f%% of the display right now."), output_scale_dynamic_factor() * 100);
        if (opt_choice("Internal resolution", &ri, res, NRES, rdef, d, false))
            g_settings.res_mode = res_mode[ri], g_settings.res_auto = false;
    }
    {   // each OS's own upscaler next to bilinear (UPSCALE_SHARP: MetalFX on macOS, AMD FSR 1 on Windows)
#ifdef __APPLE__
        static const char *ups[] = {"Bilinear", "MetalFX"};
        const char *ud = "How the image is scaled up to the window when it renders at a lower resolution. MetalFX: Apple's "
                         "upscaler, sharper. Bilinear: plain and soft.";
#elif defined(__ANDROID__)
        static const char *ups[] = {"Bilinear", "Snapdragon GSR", "AMD FSR 1"};
        const char *ud = "How the image is scaled up to the screen when it renders at a lower resolution. Snapdragon GSR: "
                         "edge-aware upscaling and sharpening in one pass, made for phones. AMD FSR 1: similar, two passes "
                         "(slower on phones). Bilinear: plain and soft.";
        static const int upv[] = {UPSCALE_BILINEAR, UPSCALE_SGSR, UPSCALE_FSR1};
#else
        static const char *ups[] = {"Bilinear", "AMD FSR 1"};
        const char *ud = "How the image is scaled up to the window when it renders at a lower resolution. AMD FSR 1: "
                         "edge-aware upscaling and sharpening, sharper. Bilinear: plain and soft.";
#endif
#ifdef __ANDROID__
        int ui = g_settings.upscaler == UPSCALE_SGSR ? 1 : g_settings.upscaler == UPSCALE_FSR1 ? 2 : 0;
#else
        int ui = g_settings.upscaler == UPSCALE_BILINEAR ? 0 : 1;
#endif
        char udm[512];
        if (g_settings.res_mode == RES_MATCH) {  // (renders at the window's size: nothing to scale up)
            snprintf(udm, sizeof udm, T("%s\n\nNot used at Match display: the game already renders at your display's resolution."), T(ud));
            ud = udm;
            opt_disable_next();
        }
#ifdef __ANDROID__
        int udef = g_default_upscaler == UPSCALE_FSR1 ? 2 : g_default_upscaler == UPSCALE_BILINEAR ? 0 : 1;
        if (opt_choice("Upscaler", &ui, ups, 3, udef, ud)) g_settings.upscaler = upv[ui], g_settings.upscaler_auto = false;
#else
        if (opt_choice("Upscaler", &ui, ups, 2, g_default_upscaler == UPSCALE_BILINEAR ? 0 : 1, ud))
            g_settings.upscaler = ui ? UPSCALE_SHARP : UPSCALE_BILINEAR, g_settings.upscaler_auto = false;
#endif
    }
    static const char *aa[] = {"Off", "SMAA"};
    opt_choice("Anti-aliasing", &g_settings.antialias, aa, 2, AA_OFF,
               "Smooths jagged edges of the game image before it is scaled to the window. SMAA: finds the edges and "
               "blends only along them, so textures stay sharp. Most useful at a lower internal resolution.");
#ifndef __APPLE__
    if (shared_title_las()) {
    static const char *fgn[] = {"Off", "60 fps (generated)"};
    opt_choice("Frame generation", &g_settings.frame_gen, fgn, 2, 0,
               "60 fps (generated): while the game runs at its own 30 frames a second, an in-between frame is made "
               "after each one from the picture, the scene's depth, the camera's movement and what moves on its own, "
               "and shown a refresh later. It pauses by itself when the game can't hold its 30. Takes effect at the "
               "next start of the app.");
    }
#endif
#ifndef __APPLE__
    {
        static const char *barn[] = {"Blurred picture", "Black"};
        int v = g_settings.black_bars ? 1 : 0;
        if (opt_choice("Side bars", &v, barn, 2, 0,
                       "What fills the screen around a picture narrower (or taller) than it - movies, menus, the file "
                       "select, Authentic 16:9: the picture's own edges, blurred and dimmed, or plain black."))
            g_settings.black_bars = v != 0;
    }
#endif
    if (shared_title_las()) {
    static const char *frn[] = {"Auto", "60 fps (native)", "30 fps"};
    opt_choice("Frame rate", &g_settings.frame_rate, frn, 3, FRAME_RATE_AUTO,
               "The game's own frame rate. Auto: 60 while your device keeps up, else the game's 30 (with frame "
               "generation, 60 on the screen). 60 fps (native): always the game's 60 fps mode - smoothest on a fast "
               "device; one that can't keep up plays in slow motion, then lower the resolution. 30 fps: always the "
               "game's 30 - cooler and easier on the battery.");
    }
#ifdef __ANDROID__
    static const char *pwr[] = {"Balanced", "Maximum performance", "GPU performance"};
    opt_choice("Power", &g_settings.power_mode, pwr, 3, 0,
               "Rooted phones: Balanced keeps the GPU a step under its top clock and leaves the phone's CPU and "
               "temperature limits alone - cooler, easier on the battery and safe on a worn one; the resolution "
               "adjusts. GPU performance holds only the GPU at its top clock (the CPU's limits stay): the sharpest "
               "picture for a warm phone, while the battery stays under 45 C. Maximum performance holds every clock at "
               "the top, past those limits: a hot phone, and a worn battery may not keep up (the phone restarts).");
#endif

    if (shared_title_las()) {
    opt_section("Game");
    opt_toggle("Remove blur", &g_settings.blur_removal, false,
               "Turns off the tilt-shift blur at the top and bottom of the screen.");
    opt_toggle("Hide Hero Mode icon", &g_settings.hide_hero_icon, false,
               "Hides the Hero Mode emblem in the corner of the screen while playing a Hero Mode file.");
    }
#ifdef __ANDROID__
    gpu_driver_rows();
#endif
    opt_end();
}

extern "C" bool cosmetics_seed_on(void);  // runtime/cosmetics_seed.c

// Cosmetics: tunic / heart colours, live (runtime/nvn/texpatch.c), and recolours by draw (runtime/nvn/recolour*.c);
// a seed made with "Random cosmetics" fills the rows left on Default (shown as "Seed: <colour>")
static void tab_cosmetics(void) {
    opt_begin("##cosmetics", ImGui::GetFontSize() / 34);
    CosColour *rows[] = {&g_settings.tunic, &g_settings.hearts, &g_settings.hair, &g_settings.skin, &g_settings.shield,
                         &g_settings.leaves, &g_settings.grass, &g_settings.sand};
    if (opt_button("Randomize all", "Every color below gets a random color."))
        for (CosColour *c : rows) c->pick = 1 + rand() % (COL_RANDOM - 1);  // (a named colour: shown as what it is)
    if (opt_button("Default all", "Everything on this page back to the game's own look.")) {
        for (CosColour *c : rows) c->pick = COL_DEFAULT;
        g_settings.enemy_colours = 0;
        g_settings.screen_filter = 0;
    }
    opt_section("Colors");
    opt_colour("Tunic color", &g_settings.tunic, COL_SLOT_TUNIC,
               "Link's tunic color. Default: the game's own (or the randomizer seed's).");
    opt_colour("Heart color", &g_settings.hearts, COL_SLOT_HEARTS, "The color of the hearts on the HUD.");
    opt_colour("Hair color", &g_settings.hair, COL_SLOT_HAIR, "Link's hair color.");
    opt_colour("Body color", &g_settings.skin, COL_SLOT_SKIN, "Link's body color.");  // (larecomp.ini: skin)
    opt_colour("Shield color", &g_settings.shield, COL_SLOT_SHIELD, "The color of Link's shield.");
    opt_section("Overworld");
    {   // Season: fills Leaves, Grass and Sand at once (palette: Orange 2, Gold 3, Pink 9, White 10); "Mixed" when
        // they match none
        static const char *const names[] = {"Spring", "Autumn", "Winter", "Cherry blossom", "Mixed"};
        static const int preset[4][3] = {{COL_DEFAULT, COL_DEFAULT, COL_DEFAULT}, {2, 3, 3}, {10, 10, 10},
                                         {9, COL_DEFAULT, COL_DEFAULT}};
        int cur = 4;  // (as shown: a row on Default shows the seed's colour, if it has its own)
        for (int i = 0; i < 4; i++)
            if (col_pick(&g_settings.leaves, COL_SLOT_LEAVES) == preset[i][0] &&
                col_pick(&g_settings.grass, COL_SLOT_GRASS) == preset[i][1] &&
                col_pick(&g_settings.sand, COL_SLOT_SAND) == preset[i][2])
                cur = i;
        int v = cur;
        if (opt_choice("Season", &v, names, 5, 0, "Sets the leaves, grass and sand colors below.") && v != cur) {
            if (v == 4) v = cur == 0 ? 3 : 0;  // (Mixed is never chosen: past the ends, back round)
            g_settings.leaves.pick = preset[v][0];
            g_settings.grass.pick = preset[v][1];
            g_settings.sand.pick = preset[v][2];
        }
    }
    opt_colour("Leaves color", &g_settings.leaves, COL_SLOT_LEAVES, "The overworld's tree leaves and bushes.");
    opt_colour("Grass color", &g_settings.grass, COL_SLOT_GRASS, "The overworld's grass.");
    opt_colour("Sand color", &g_settings.sand, COL_SLOT_SAND, "The beaches, the desert and the sandy south.");
    opt_section("Enemies");
    {   // (Off in a seed with its own cosmetics: "Seed", its enemies are per seed)
        const char *modes[N_ENEMY_COLOUR_MODES];
        for (int i = 0; i < N_ENEMY_COLOUR_MODES; i++) modes[i] = g_enemy_colour_modes[i];
        if (cosmetics_seed_on()) modes[0] = "Seed";
        opt_choice("Enemy colors", &g_settings.enemy_colours, modes, N_ENEMY_COLOUR_MODES, 0,
                   "Each kind of enemy in another color. Enemies whose color shows their strength or type keep theirs.");
    }
    opt_section("Screen");
    opt_choice("Screen filter", &g_settings.screen_filter, g_screen_filters, N_SCREEN_FILTERS, 0,
               "A filter over the whole picture, the HUD included.");
    opt_end();
}

static void tab_audio(void) {
    opt_begin("##audio", ImGui::GetFontSize() / 34);
    opt_section("Sound");
    float pct = roundf(g_settings.volume * 100);
    if (opt_slider("Volume", &pct, 0, 200, 5, 100, "%g%%",
                   "The game's volume. 100% is the game's own level; above it, louder."))
        g_settings.volume = pct / 100.0f;
    opt_toggle("Room reverb", &g_settings.reverb, true,
               "The game's echo in caves, dungeons and big rooms.");
    opt_toggle("Movie audio", &g_settings.movie_audio, true,
               "Sound in the game's movies (the opening and the ending). Applies the next time a movie starts.");
    opt_end();
}

// Quit: back to the start screen (in game: the game reboots to it) or out to the desktop. Each asks first (progress
// since the last save is lost); B / Esc or Cancel backs out.
extern "C" void rt_request_reboot(int kind);  // runtime/main.c (RT_REBOOT_TITLE = 2: the start screen)
static void tab_quit(float scale, bool in_game) {
    static int ask;  // 0: the two choices; 1: start screen?; 2: desktop?
    if (ask && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
        ask = 0;
    const char *lost = in_game ? T("\n\nProgress since your last save is lost.") : "";
    char d1[300], d2[300];
    snprintf(d1, sizeof d1, T("Leaves the game and goes back to the start screen.%s"), lost);
    snprintf(d2, sizeof d2, T("Closes the app.%s"), lost);
    opt_begin("##quit", scale);
    opt_section("Quit");
    if (!ask) {
        if (!in_game) opt_disable_next();  // (already on it)
        if (opt_action("Quit to Start Screen", "", in_game ? d1 : "You're on the start screen.")) ask = 1;
        if (opt_action("Quit to Desktop", "", d2)) ask = 2;
    } else {
        if (opt_action(ask == 1 ? "Yes, quit to Start Screen" : "Yes, quit to Desktop", "", ask == 1 ? d1 : d2)) {
            settings_save();
            if (ask == 1) {
                rt_request_reboot(2);
                set_open(false);
            } else {
                SDL_Event q = {};
                q.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&q);
            }
            ask = 0;
        }
        if (opt_action("Cancel", "", "Stays where you are.")) ask = 0;
    }
    opt_end();
}

#ifdef __ANDROID__
// ---- the Brazilian Portuguese fan translation: copied in from the folder the player picks (LasActivity's picker; its
// romfs/ into translations/pt-BR/romfs). The answer comes on a Java thread and is handled here.
static std::atomic<int> g_tr_state{0};  // 0 nothing new, 1 installed, 2 failed (g_tr_err)
static char g_tr_err[256];
static void translation_imported(const char *err) {  // (Java thread)
    if (err && !strcmp(err, "nofolder")) return;  // (cancelled)
    if (err) snprintf(g_tr_err, sizeof g_tr_err, "%s", err);
    g_tr_state = err ? 2 : 1;
}

extern "C" void translation_rows(void) {  // (difficulty.mm: under the Language row)
    static std::string msg;
    if (int st = g_tr_state.exchange(0)) {
        if (st == 1) settings_translation_changed(), msg = T("Translation installed - restart the game to see it.");
        else if (!strcmp(g_tr_err, "notfound"))
            msg = T("No translation there: pick the folder extracted from the translation's zip (the one with romfs and "
                    "regionUS inside).");
        else msg = std::string(T("The translation couldn't be copied: ")) + (strncmp(g_tr_err, "copy:", 5) ? g_tr_err : g_tr_err + 5);
    }
    if (g_settings.language != LANG_PT_BR) return;
    bool have = settings_translation_dir() != NULL;
    if (!have) hint("Portuguese (Brazil) needs the fan translation: import the folder extracted from its zip. Until "
                    "then the game's text stays in English.");
    if (ImGui::Button(T(have ? "Reimport translation..." : "Import translation..."))) {
        std::string dest = std::string(rt_data_root()) + "/translations/pt-BR/romfs";
        android_import_translation(dest.c_str(), translation_imported);
    }
    if (!msg.empty()) hint(msg.c_str());
}
#endif

extern "C" bool inventory_overlay_wanted(void);
extern "C" bool tracker_overlay_wanted(void);
extern "C" bool tracker_dungeon_wanted(void);
extern "C" void tracker_dungeon_draw(ImVec2 display, ImFont *font);
extern "C" void tracker_overlay_draw(ImVec2 display, ImFont *font);
extern "C" bool chaos_overlay_wanted(void);
extern "C" void chaos_overlay_draw(ImVec2 display, ImFont *font);
extern "C" void inventory_overlay_draw(ImVec2 display, ImFont *font);
extern "C" bool touch_visible(void);  // touch_controls.mm
extern "C" void touch_draw(ImDrawList *dl, float w, float h, ImFont *font);
extern "C" bool race_overlay_wanted(void);
extern "C" void race_overlay_draw(ImVec2 display, ImFont *font, float scale);
extern "C" bool pickup_flyin_wanted(void);  // (silent pickups: pickup_flyin.mm)
extern "C" void pickup_flyin_draw(ImVec2 display, ImFont *font, float scale);
extern "C" bool race_overlay_key(char *out, size_t cap);

// ---- reusing the overlay's last drawing (LARECOMP_OVERLAY_CACHE=0: off, a new ImGui frame every present as before)
// While only the FPS counter and / or the run timer are up (no menu, no input, nothing interactive or animated), the
// overlay draws the same thing frame after frame: ImGui's NewFrame + layout + Render then run for nothing on the
// render thread. Their drawing is described by a key (the text shown, the window size); while it stays the same, the
// last frame's draw data (ImGui keeps it until the next NewFrame) is drawn again. Only after three frames in a row
// built with that key: an auto-sized window (the FPS counter) settles over the frames after its text changes.
// The FPS counter then shows the same average as ImGui's io.Framerate (the last 60 frame times), sampled twice a
// second instead of every frame.
static int g_ov_cache = -1;
static float g_fps_dt[60];
static int g_fps_n, g_fps_i;
static double g_fps_sum, g_fps_shown, g_fps_at;
static void fps_note(double now, float dt) {
    if (g_fps_n == 60) g_fps_sum -= g_fps_dt[g_fps_i];
    else g_fps_n++;
    g_fps_dt[g_fps_i] = dt;
    g_fps_sum += dt;
    g_fps_i = (g_fps_i + 1) % 60;
    if (now - g_fps_at >= 0.5 || now < g_fps_at) {
        g_fps_at = now;
        g_fps_shown = g_fps_sum > 0 ? g_fps_n / g_fps_sum : 0;
    }
}

// Reboot (runtime/main.c): the game is gone - menu and name dialog close; the ImGui context stays.
RT_ON_RESET(menu_reset) {
    set_open(false);
    g_kbd = 0;
    osk_cancel();
    pthread_mutex_lock(&q_lock);
    q_events.clear();
    pthread_mutex_unlock(&q_lock);
}

static void keyboard_draw(ImGuiIO &io, float scale) {
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 120));
    float w = fminf(io.DisplaySize.x * 0.9f, 1100 * scale);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    ImGui::Begin("##kbd", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushFont(g_font_big);
    ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "%s", T("Enter your name"));
    ImGui::PopFont();
    static bool was_open, was_pad;
    bool pad = g_last_pad.load(), appearing = !was_open || pad != was_pad;  // (switching device: focus the new one)
    was_open = true, was_pad = pad;
#ifdef __ANDROID__
    // a phone without a controller: big keys on the screen to tap (the system keyboard over the game was easy to miss
    // and had no way to confirm: players couldn't get past naming Link); a hardware keyboard still types into them
    if (!pad) {
        int r = osk_keys(g_kbd_text, sizeof g_kbd_text, g_kbd_max, appearing, scale, g_font_big, true, true);
        if (r) g_kbd = r > 0 ? 2 : 3;
    } else
#endif
    if (!pad) {  // keyboard: type straight into the field
        if (appearing) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        ImGui::PushFont(g_font_big);
        ImGui::InputText("##name", g_kbd_text, (size_t)(g_kbd_max * 4 + 1 < (int)sizeof g_kbd_text ? g_kbd_max * 4 + 1 : sizeof g_kbd_text),
                         ImGuiInputTextFlags_CallbackCharFilter,
                         [](ImGuiInputTextCallbackData *d) -> int { return d->EventChar < 0x20 ? 1 : 0; });
        ImGui::PopFont();
        keyboard_text_trim(g_kbd_text, sizeof g_kbd_text, (size_t)g_kbd_max);
        ImGui::TextDisabled(T("Enter  confirm      Esc  cancel      (%d / %d)"), (int)keyboard_text_units(g_kbd_text, sizeof g_kbd_text), g_kbd_max);
    } else {  // controller: on-screen keyboard (osk.mm, shared with our menus' text fields)
        int r = osk_keys(g_kbd_text, sizeof g_kbd_text, g_kbd_max, appearing, scale, g_font_big);
        if (r) g_kbd = r > 0 ? 2 : 3;  // (Start / + confirms in menu_handle_event too)
    }
    ImGui::End();
    if (g_kbd.load() != 1) was_open = false;
}

// Game thread (runtime/hle/swkbd.c): show the dialog, re-present the last frame until it's answered.
// Returns 1 confirmed (out = text, UTF-8), 0 cancelled.
extern "C" int menu_keyboard_run(const char *initial, int max_len, char *out, int cap) {
    extern void backend_represent(void);
    snprintf(g_kbd_text, sizeof g_kbd_text, "%s", initial ? initial : "");
    g_kbd_max = max_len > 0 ? (max_len < (int)sizeof g_kbd_text ? max_len : (int)sizeof g_kbd_text - 1) : (int)sizeof g_kbd_text - 1;
    keyboard_text_trim(g_kbd_text, sizeof g_kbd_text, (size_t)g_kbd_max);
    input_reset();
    g_kbd = 1;
    // (test) LARECOMP_KBD_AUTO=<text>: answer at once
    if (getenv("LARECOMP_KBD_AUTO")) { snprintf(g_kbd_text, sizeof g_kbd_text, "%s", getenv("LARECOMP_KBD_AUTO")); g_kbd = 2; }
    while (g_kbd.load() == 1 && !g_rt_unwind) {  // ~60 Hz (a reboot cancels)
        uint64_t t0 = SDL_GetTicks();
        backend_represent();
        uint64_t dt = SDL_GetTicks() - t0;
        if (dt < 16) SDL_Delay((uint32_t)(16 - dt));
    }
    bool ok = g_kbd.load() == 2 && g_kbd_text[0];
    if (ok && cap > 0) { snprintf(out, cap, "%s", g_kbd_text); keyboard_text_trim(out, (size_t)cap, (size_t)g_kbd_max); }
    g_kbd = 0;
    input_reset();
    return ok;
}

// LARECOMP_MENU_FOCUS_LOG=1 (test; tools/menu_focus_sweep.sh): every Down / Up on a page of option rows is checked a
// few frames later against the rows' order and logged "[menu] focus check ... OK / FAIL": the first Down after a tab
// switch (however it came) or the menu opening, and Down from the tab bar, land on the page's first enabled row; Down
// goes to the next row (the last one stays), Up to the row before, and Up from the top row onto the selected tab.
static void focus_check(bool down, bool up, bool any_down, int focused_tab, int tab, const char *tab_name) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_MENU_FOCUS_LOG") != NULL;
    if (!on) return;
    static int shown = -1000, due_in, want, from, prev_focus = -1;  // (want: a row index, -1: the selected tab)
    static bool first_down_due;
    static const char *what;
    if (tab != shown || g_page_fresh_seen) shown = tab, first_down_due = true, g_page_fresh_seen = false, prev_focus = -1;
    int n = opt_row_count(), first = opt_first_row(), focus = opt_focused_row();
    if (any_down && !down) first_down_due = false;  // (a held Down's repeat: the first Down was checked or skipped)
    if ((down || up) && !due_in && n > 0) {
        from = prev_focus, due_in = 4;
        if (down && (first_down_due || from < 0)) want = first, what = first_down_due ? "first Down" : "Down from the tab bar";
        else if (down) want = from + 1 < n ? from + 1 : from, what = "Down";
        else if (from > 0) want = from - 1, what = "Up";
        else if (from == 0) want = -1, what = "Up from the top row";
        else due_in = 0;  // (Up on the tab bar: not a page move)
        if (down) first_down_due = false;
    } else if (down && n == 0 && first_down_due) {
        first_down_due = false;
        LOG("[menu] focus check: first Down on %s SKIP (no option rows)\n", tab_name);
    }
    prev_focus = focus;
    if (!due_in || --due_in) return;
    bool ok = want >= 0 ? focus == want : focused_tab == tab;
    char got[160];
    if (focus >= 0) snprintf(got, sizeof got, "\"%s\"", opt_row_label(focus));
    else snprintf(got, sizeof got, "tab %d", focused_tab);
    char exp[160];
    if (want >= 0) snprintf(exp, sizeof exp, "\"%s\"", opt_row_label(want));
    else snprintf(exp, sizeof exp, "tab %d", tab);
    LOG("[menu] focus check: %s on %s from %s -> %s (want %s) %s\n", what, tab_name,
        from >= 0 ? opt_row_label(from) : "the tab bar", got, exp, ok ? "OK" : "FAIL");
}

// The tab bar and its pages, into the current window: the in-game menu, and the start screen's Settings (in_game
// false: nothing of the game is loaded yet - the tabs that need it are greyed out; prelaunch: the start screen's own
// first tab, game files and fonts, drawn by launcher.mm).
static void tabs_body(float scale, bool in_game, void (*prelaunch)(float)) {
    // (id: the page; -1 = Prelaunch)
    struct Tab { const char *name; int id; bool on; };
    Tab tabs[11];
    int ntabs = 0;
    if (prelaunch) tabs[ntabs++] = {"Prelaunch", -1, true};
    static const char *names[] = {"Video", "Audio", "Save Editor", "Randomizer", "Warp", "Cheats", "Gameplay",
                                  "Cosmetics", "Controls", "Quit"};
    static const int order[] = {8, 0, 1, 6, 7, 3, 5, 4, 2, 9};  // Controls, Video, Audio, Gameplay, Cosmetics, Randomizer,
                                                                // Cheats, Warp, Save Editor, Quit (ids stay: test hooks)
    for (int i : order)  // (Quit: in game only - the start screen has its own QUIT, and an eleventh tab cut names short)
        if ((shared_title_las() || i == 0 || i == 1 || i == 8 || i == 9) && (i != 9 || !prelaunch))
            tabs[ntabs++] = {names[i], i, in_game || (i != 2 && i != 4 && i != 5)};
    auto available = [&](int id) {
        for (int i = 0; i < ntabs; ++i) if (tabs[i].id == id && tabs[i].on) return true;
        return false;
    };
    // Requests use stable tab IDs, including external debug requests. Hidden LAS
    // pages must not become active via an environment variable or retained focus.
    if (!available(g_tab)) { g_tab = tabs[0].id; g_tab_request = g_tab; }
    if (g_tab_request >= -1 && !available(g_tab_request)) g_tab_request = g_tab;
    if (g_focus_tab >= 0 && !available(g_focus_tab)) g_focus_tab = -100;

    int cur = 0;  // g_tab / g_tab_request hold ids
    for (int i = 0; i < ntabs; i++) if (tabs[i].id == g_tab) cur = i;
    if (!tabs[cur].on) cur = 0;
    // L/R (or Q/E) switch tabs, skipping greyed-out ones (not while typing: Q / E are letters then, and the on-screen
    // keyboard has the controller)
    if (g_tab_request_ext >= 0) {
        if (available(g_tab_request_ext)) g_tab_request = g_tab_request_ext;
        g_tab_request_ext = -1;
    }
    bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), typing = ui_text_busy();
    static int focused_tab = -99;  // the tab the controller's cursor is on (from the frame before; -99: none)
    bool switching = false;  // (L / R this frame: the page changes next frame)
    if (!any_popup && !typing) {
        int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1
                 : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false) ? -1 : 0;
        if (step) {
            int i = cur;
            do i = (i + step + ntabs) % ntabs; while (!tabs[i].on);
            g_tab_request = tabs[i].id;
            // the cursor goes onto the new tab (left behind on the tab bar, A on it switched straight back - user
            // 2026-10-11; left in the old page, it stayed on a row that is gone, and Down went on from where that row
            // had been)
            g_focus_tab = tabs[i].id, g_focus_frames = 0;
            switching = true;
        }
    }
    bool down = !any_popup && !typing && (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown) ||
                                          ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown) ||
                                          ImGui::IsKeyPressed(ImGuiKey_DownArrow));
    bool up = !any_popup && !typing && (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp) ||
                                        ImGui::IsKeyPressed(ImGuiKey_GamepadLStickUp) || ImGui::IsKeyPressed(ImGuiKey_UpArrow));
    // The first Down on a page just switched to (L / R, a click or A on a tab, the menu opening), or Down with the
    // cursor on the tab bar: the page's first enabled row, top to bottom - never ImGui's pick from wherever its cursor
    // was (a row of the page before, the tab bar's x). Option rows take it (opt_focus_first); a page without them gets
    // its first control (g_page_focus_next).
    if (ImGui::GetIO().MouseClicked[0]) g_page_fresh = false;  // (the mouse went into the page)
    bool down_from_tab = down && !switching &&
                         ((focused_tab != -99 && ImGui::GetCurrentContext()->NavCursorVisible) || g_page_fresh);
    if (down_from_tab) {
        ImGui::NavMoveRequestCancel();  // (ImGui's own move this frame)
        g_page_fresh = false;
        g_focus_tab = -99, g_focus_frames = 0;  // (the cursor stops going back to the tab)
    }
    opt_focus_first(down_from_tab);
    // Up / Down on a page of option rows: the next / previous row in order (opt_nav_step), not ImGui's spatial pick
    if ((down || up) && !down_from_tab && !switching && opt_nav_step(down ? 1 : -1)) ImGui::NavMoveRequestCancel();
    opt_frame_reset();
    // The cursor moved out of the page (ImGui's pick, applied this frame for last frame's press - Up from the page's
    // top control): onto the selected tab. ImGui's pick was the tab nearest the control's x (Controls, from most
    // rows), or the close button
    ImGuiContext &g = *ImGui::GetCurrentContext();
    ImGuiWindow *root = ImGui::GetCurrentWindow()->RootWindow;
    static bool nav_in_page;  // (the frame before: the cursor on a control of the page - its child windows)
    if (nav_in_page && g.NavJustMovedToId && g.NavWindow == root && !any_popup) g_focus_tab = -100, g_focus_frames = 0;
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_NoTabListScrollingButtons)) {
        for (int k = 0; k < ntabs; k++) {
            const Tab &t = tabs[k];
            ImGuiTabItemFlags f = g_tab_request == t.id && t.on ? ImGuiTabItemFlags_SetSelected : 0;
            char lbl[96];  // (###: the same tab in either language)
            snprintf(lbl, sizeof lbl, prelaunch ? " %s ###tab%d" : "  %s  ###tab%d", T(t.name), t.id);  // (ten tabs: tighter, so none is cut short)
            ImGui::BeginDisabled(!t.on);  // (out of the game: Save Editor, Warp, Cheats)
            // focus (controller / keyboard): the tab lights up a little with a gold bar along its bottom, instead of
            // ImGui's outline (it sits 4 px outside the tab and the tab bar's clipping could cut its top off on macOS)
            // (focused_tab: from the frame before - colours go in before the tab is drawn)
            bool lit = focused_tab == t.id && ImGui::GetCurrentContext()->NavCursorVisible;
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
            if (lit) {
                ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.19f, 0.27f, 0.22f, 1));
                ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(0.19f, 0.27f, 0.22f, 1));
            }
            if (g_focus_tab == -1 && !prelaunch) g_focus_tab = 8;  // (no Prelaunch in game: Controls, the first tab)
            bool focus_here = g_focus_tab == t.id || (g_focus_tab == -100 && t.id == g_tab);
            bool open = ImGui::BeginTabItem(lbl, NULL, f);
            ImGui::PopStyleColor(lit ? 3 : 1);
            if (focus_here) {  // the controller's cursor starts on this tab (SetKeyboardFocusHere skips tabs: no tab stop)
                ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                ImGui::SetNavCursorVisible(true);
                // a few frames: the page's child window appearing makes ImGui pick its first widget once it's laid out
                if (++g_focus_frames >= 4) g_focus_tab = -99, g_focus_frames = 0;
                else g_focus_tab = t.id;
            }
            if (lit) {
                ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                float h = 4 * scale > 2 ? 4 * scale : 2;
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x + 2, b.y - h), ImVec2(b.x - 2, b.y),
                                                          IM_COL32(236, 194, 84, 255));  // (the menu's gold)
            }
            if (ImGui::IsItemFocused()) focused_tab = t.id;
            else if (focused_tab == t.id) focused_tab = -99;
            ImGui::EndDisabled();
            if (open) {
                if (t.id != g_shown_tab) g_shown_tab = t.id, g_page_fresh = true, g_page_focus_next = false;
                g_tab = t.id;
                ImGui::Spacing();
                ImGui::BeginChild("##page", ImVec2(0, 0), ImGuiChildFlags_None,  // (the rest of the window: no hint line)
                                  ImGuiWindowFlags_NavFlattened);
                if (g_page_focus_next) {  // (a page without option rows: its first control)
                    g_page_focus_next = false;
                    ImGui::SetKeyboardFocusHere();
                    ImGui::SetNavCursorVisible(true);
                }
                switch (t.id) {
                case -1: prelaunch(scale); break;
                case 0: tab_display(); break;
                case 1: tab_audio(); break;
                case 2:
                    if (game_in_play()) save_editor_draw(scale);
                    else ImGui::TextDisabled("%s", T("Load a save file and get into the game to edit it."));
                    break;
                case 3: rando_tab_draw(scale); break;
                case 4: if (warp_tab_draw(scale)) set_open(false); break;
                case 5: cheats_draw(scale); break;
                case 6: difficulty_draw(scale); break;
                case 7: tab_cosmetics(); break;
                case 8: controls_tab_draw(scale); break;
                case 9: tab_quit(scale, in_game); break;
                }
                static const char *scroll_test = getenv("LARECOMP_MENU_SCROLL");  // test hook: page scroll, 0..1
                if (scroll_test) ImGui::SetScrollY(ImGui::GetScrollMaxY() * (float)atof(scroll_test));
                // no enabled option row took the cursor: the page's first control, next frame (SetKeyboardFocusHere:
                // the first item that takes the cursor, in order)
                if (opt_focus_first_left() && down_from_tab) g_page_focus_next = true;
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
        g_tab_request = -2;
        opt_focus_first_left();  // (no page drawn: nothing took it)
        ImGui::EndTabBar();
    }
    nav_in_page = g.NavWindow && g.NavWindow != g.NavWindow->RootWindow && g.NavWindow->RootWindow == root;
    // (checks: fresh presses only - a held key's repeats come faster than a check's few frames)
    bool down1 = down && (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown, false) ||
                          ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown, false) || ImGui::IsKeyPressed(ImGuiKey_DownArrow, false));
    bool up1 = up && (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp, false) ||
                      ImGui::IsKeyPressed(ImGuiKey_GamepadLStickUp, false) || ImGui::IsKeyPressed(ImGuiKey_UpArrow, false));
    for (int k = 0; k < ntabs; k++)
        if (tabs[k].id == g_tab) focus_check(down1, up1, down, focused_tab, g_tab, tabs[k].name);
}

void menu_tabs_draw(float scale, ImFont *font, ImFont *font_big, bool in_game, void (*prelaunch)(float)) {
    ImFont *f0 = g_font, *f1 = g_font_big;
    g_font = font, g_font_big = font_big;
    ImGuiStyle saved = ImGui::GetStyle();  // the menu's look (the start screen has its own style)
    style_init();
    ImGui::GetStyle().ScaleAllSizes(scale);
    tabs_body(scale, in_game, prelaunch);
    ImGui::GetStyle() = saved;
    g_font = f0, g_font_big = f1;
}

static bool g_menu_drawn;  // menu_render drew overlays this frame (menu_redraw)
// Whether menu_render draws anything: the menu, the keyboard, an overlay (FPS, inventory, tracker, chaos, race, a
// pickup card). Asked by the Vulkan backend before menu_render, which records into the window image's command buffer
// itself: its recorder thread (runtime/nvn/vulkan/record.c) has to be caught up first, and only when there is something
// to draw.
static bool render_needed(bool open, bool kbd, bool inv, bool trk, bool trkd, bool chaos, bool race, bool flyin, bool touch) {
    return open || kbd || g_settings.show_fps || inv || trk || trkd || chaos || race || flyin || touch;
}

#ifdef __APPLE__
void menu_render(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> target) {
    const double tw = target.width, th = target.height;
#else
void menu_render(void *cmd, uint32_t target_w, uint32_t target_h) {
    const double tw = target_w, th = target_h;
#endif
    bool open = g_open.load();
    const bool las = shared_title_las();
    bool inv = las && !open && inventory_overlay_wanted();
    bool trk = las && !open && tracker_overlay_wanted();
    bool trkd = las && !open && tracker_dungeon_wanted();
    bool chaos = las && !open && chaos_overlay_wanted();
    bool kbd = g_kbd.load() == 1;
    bool touch = !open && !kbd && touch_visible();
    bool race = las && race_overlay_wanted();  // (run timer, race seed hash: race_overlay.mm)
    g_menu_drawn = false;
    bool flyin = las && !open && pickup_flyin_wanted();  // (a silent pickup's card)
    if (!render_needed(open, kbd, inv, trk, trkd, chaos, race, flyin, touch)) {
        if (g_inited) { pthread_mutex_lock(&q_lock); q_events.clear(); pthread_mutex_unlock(&q_lock); }
        return;
    }
#ifdef __APPLE__
    if (!g_inited) init(dev);
#else
    if (!g_inited) init();
#endif
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)tw, (float)th);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    static double last;
    double now = SDL_GetTicksNS() / 1e9;
    io.DeltaTime = last > 0 && now > last ? (float)(now - last) : 1.0f / 60;
    last = now;
    if (g_ov_cache < 0) g_ov_cache = !getenv("LARECOMP_OVERLAY_CACHE") || atoi(getenv("LARECOMP_OVERLAY_CACHE")) != 0;
    // DrawData borrows ImGui window buffers. Keep its lifetime within this UI
    // frame. Cross-present reuse bypasses NewFrame/Render ownership boundaries;
    // rebuild before each backend upload. FPS smoothing is separate.
    if (g_ov_cache) fps_note(now, io.DeltaTime);
    drain_events(io);
    // fonts are built at 34/56 px for a ~1080 px tall output; scale to the window
    float scale = (float)(th / 1080.0);
    if (scale < 0.45f) scale = 0.45f;
    io.FontGlobalScale = scale;
    // a drag / flick: this frame's scroll, applied after NewFrame (touch_scroll_apply)
    float touch_px = g_touch.frame(io.DeltaTime, [&](float x, float y) { io.AddMousePosEvent(x, y); });

#ifdef __APPLE__
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
#endif
    // spacing/rounding follow the output size too (style tuned for 1080 p)
    static ImGuiStyle base;
    static float style_scale = -1;
    if (style_scale < 0) base = ImGui::GetStyle();
    if (fabsf(style_scale - scale) > 0.01f) {
        ImGui::GetStyle() = base;
        ImGui::GetStyle().ScaleAllSizes(scale * kUi);
        style_scale = scale;
    }
#ifdef __APPLE__
    ImGui_ImplMetal_NewFrame(rp);
#else
    ImGui_ImplVulkan_NewFrame();
#endif
    ImGui::NewFrame();
    touch_scroll_apply(touch_px);

    if (g_settings.show_fps) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 16 * scale, 16 * scale), 0, ImVec2(1, 0));
        ImGui::SetNextWindowBgAlpha(0.5f);
        ImGui::Begin("##fps", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
#ifdef __APPLE__
        ImGui::Text("%.0f fps", g_ov_cache ? g_fps_shown : io.Framerate);
#else
        ImGui::Text("%.0f fps", vk_shown_fps() > 0 ? vk_shown_fps() : g_ov_cache ? g_fps_shown : io.Framerate);
#endif
        ImGui::End();
    }

    if (race) race_overlay_draw(io.DisplaySize, g_menu_font_big, scale);
    if (touch) touch_draw(ImGui::GetBackgroundDrawList(), io.DisplaySize.x, io.DisplaySize.y, g_font_big ? g_font_big : ImGui::GetFont());
    if (flyin) pickup_flyin_draw(io.DisplaySize, g_font_big ? g_font_big : ImGui::GetFont(), scale);
    if (trk) tracker_overlay_draw(io.DisplaySize, g_font_big ? g_font_big : ImGui::GetFont());
    if (chaos) chaos_overlay_draw(io.DisplaySize, g_font_big ? g_font_big : ImGui::GetFont());
    if (trkd) tracker_dungeon_draw(io.DisplaySize, g_font_big ? g_font_big : ImGui::GetFont());
    if (inv) {
        inventory_overlay_draw(io.DisplaySize, g_font_big ? g_font_big : ImGui::GetFont());
    }

    if (open) {
        // dim the game behind the menu
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 110));
        float w = fminf(io.DisplaySize.x * 0.9f, 1500 * scale), h = io.DisplaySize.y - 2 * 18 * scale;  // (all the height: a slim margin)
#ifdef __ANDROID__
        w = io.DisplaySize.x - 2 * 24 * scale;  // (phones: the whole screen, as the WWHD port's settings)
#endif
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
        ImGui::Begin("##menu", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImFont *keep_big = g_font_big;  // (the window in the menu's font; header() uses g_font_big)
        g_font_big = g_menu_font_big;
        ImGui::PushFont(g_menu_font);
        // header as on the start screen's Settings: the title, and a close X in the corner (B / Esc too). ("Title
        // screen" - rt_request_reboot(RT_REBOOT_TITLE) - is to find a new home; user 2026-10-11.)
        ImGui::PushFont(g_font_big);
        ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "%s", T("Settings"));
        ImGui::PopFont();
        {
            float bx = 44 * scale;
            ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - bx);
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
            bool close = ImGui::InvisibleButton("##close", ImVec2(bx, bx), ImGuiButtonFlags_EnableNav);
            ImGui::PopStyleColor();
            bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
            ImU32 col = hot ? IM_COL32(236, 194, 84, 255) : IM_COL32(220, 216, 205, 255);
            float m = bx * 0.3f, t = (hot ? 3.0f : 2.4f) * scale * 1.5f;
            ImDrawList *dl = ImGui::GetWindowDrawList();
            if (hot) dl->AddRectFilled(p, ImVec2(p.x + bx, p.y + bx), IM_COL32(255, 255, 255, 20), 8 * scale);
            dl->AddLine(ImVec2(p.x + m, p.y + m), ImVec2(p.x + bx - m, p.y + bx - m), col, t);
            dl->AddLine(ImVec2(p.x + bx - m, p.y + m), ImVec2(p.x + m, p.y + bx - m), col, t);
            if (close) set_open(false);
        }

        bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), typing = ui_text_busy();  // (before the
        tabs_body(scale, true, NULL);                                                                          //  pages run)
        // B with nothing focused-deeper closes the menu (not the Esc / B that ends typing: typing is from the frame
        // before, so it's still true the frame a field lets go)
        if (!any_popup && !typing && !ui_text_busy() && !controls_ui_took_back() && (!las || !rando_tab_took_back()) &&
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
            set_open(false);
        ImGui::PopFont();
        g_font_big = keep_big;
        ImGui::End();
        osk_draw(g_font_big, scale);  // (a text field activated with the controller)
    }

    if (kbd) keyboard_draw(io, scale);
    ImGui::Render();
    g_text_busy = open && ui_text_busy();
#ifdef __APPLE__
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
    [enc endEncoding];
#else
    // ImGui records into cmd itself: the recorder thread (record.c) must have recorded everything queued so far, and
    // be idle, first (one thread at a time on a command buffer and its pool). Drained here, right before, rather than
    // on an earlier guess in device.c of whether anything is drawn: that read the overlays' state a moment before the
    // check at the top of this function read it again, so an overlay turning on in between (game_in_play() at the
    // first frame of play, a pickup card) recorded into cmd while the recorder was still recording it. Uploads
    // queued above (overlay icons) are recorded by the drain too.
    rec_drain();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)cmd);  // inside the window image's rendering
    g_menu_drawn = true;
#endif
}

#ifndef __APPLE__
// The last menu_render's overlays drawn again as they were (the in-between frame of frame generation): no new UI
// frame - a second menu_render per game frame fed the touch controls twice and the stick snapped back to centre
void menu_redraw(void *cmd) {
    if (!g_menu_drawn || !ImGui::GetDrawData()) return;
    rec_drain();  // (ImGui records into cmd itself: menu_render)
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)cmd);
}
#endif
