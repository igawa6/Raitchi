// Live boot preparation UI with real ImGui core, controlled SDL events and workers.
#include <SDL3/SDL.h>
#include "imgui.h"
#include "imgui_internal.h"
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <unistd.h>
#include <vector>
static unsigned checks;
#define CHECK(x) do { ++checks; assert(x); } while (0)
static ImGuiContext *g_ctx;
static bool g_inited;
static ImFont *g_menu_font_big;
static std::atomic<unsigned> loops{0};
static std::atomic<uint64_t> clock_ns{1000000000};
static unsigned stop_loop = ~0u, create_calls, joins, resets, hotplugs, bars;
static std::atomic<unsigned> complete_loop{30};
static bool ready_cancel;
static unsigned ready_cancel_stage;
static bool test_gate_ready();
static bool stop_before, create_fails, synchronous_worker;
static unsigned stop_checks, stop_at_check = ~0u;
static std::atomic<unsigned> worker_done{0};
static float last_fraction;
static ImRect continue_rect, cancel_rect;
struct Scheduled { unsigned loop; SDL_Event event; };
static std::vector<Scheduled> events;
static size_t event_at;
static void init() {
    g_ctx = ImGui::CreateContext(); ImGui::SetCurrentContext(g_ctx);
    auto &io = ImGui::GetIO(); io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    g_menu_font_big = io.Fonts->AddFontDefault();
    unsigned char *pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height); g_inited = true;
}
extern "C" SDL_Window *ui_window() { return reinterpret_cast<SDL_Window *>(1); }
extern "C" bool SDL_GetWindowSizeInPixels(SDL_Window *, int *w, int *h) { *w = 1280; *h = 720; return true; }
extern "C" float SDL_GetWindowPixelDensity(SDL_Window *) { return 2; }
extern "C" Uint64 SDL_GetTicksNS() { return clock_ns.load(); }
extern "C" Uint64 SDL_GetTicks() { return clock_ns.load() / 1000000; }
extern "C" void SDL_Delay(Uint32 ms) { clock_ns.fetch_add((uint64_t)ms * 1000000); loops++; usleep(1000); }
extern "C" bool SDL_PollEvent(SDL_Event *e) {
    if (ready_cancel && ready_cancel_stage < 2 && loops >= 12 + ready_cancel_stage) {
        *e = {};
        e->type = ready_cancel_stage ? SDL_EVENT_MOUSE_BUTTON_UP : SDL_EVENT_MOUSE_BUTTON_DOWN;
        e->button.which = 1; e->button.button = SDL_BUTTON_LEFT;
        e->button.x = cancel_rect.GetCenter().x / 2; e->button.y = cancel_rect.GetCenter().y / 2;
        if (ready_cancel_stage++) {
            complete_loop = 0;
            while (!test_gate_ready()) usleep(100);
        }
        return true;
    }
    if (event_at == events.size() || events[event_at].loop > loops.load()) return false;
    *e = events[event_at++].event; return true;
}
extern "C" int rt_reboot_requested() { return ++stop_checks >= stop_at_check || stop_before || loops.load() >= stop_loop; }
extern "C" void input_handle_event(const SDL_Event *) { hotplugs++; }
static void input_reset() { resets++; }
static void ImGui_ImplVulkan_NewFrame() {}
using VkCommandBuffer = void *;
static bool vk_frame_begin(const float *) { return false; }
static void *vk_frame_cmd() { return nullptr; }
static void vk_frame_end(const char *) {}
static void rec_drain() {}
static void ImGui_ImplVulkan_RenderDrawData(ImDrawData *, VkCommandBuffer) {}
static ImGuiKey ui_pad_key(SDL_JoystickID, Uint8 button, bool) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    default: return ImGuiKey_None;
    }
}
static void vk_headstart_prepare(void (*progress)(int, int), bool (*cancelled)()) {
    progress(2, 10);
    while (!cancelled() && loops.load() < complete_loop) usleep(100);
    if (!cancelled()) progress(10, 10);
    worker_done++;
}
static int test_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*run)(void *), void *arg) {
    create_calls++;
    if (create_fails) return 1;
    if (synchronous_worker) { run(arg); return 0; }
    return pthread_create(thread, attr, run, arg);
}
static int test_pthread_join(pthread_t thread, void **result) { joins++; return synchronous_worker ? 0 : pthread_join(thread, result); }
static bool test_button(const char *label, const ImVec2 &size) {
    bool result = ImGui::Button(label, size);
    if (!strcmp(label, "Continue")) continue_rect = GImGui->LastItemData.Rect;
    if (!strcmp(label, "Cancel game")) cancel_rect = GImGui->LastItemData.Rect;
    return result;
}
static void test_progress_bar(float fraction, const ImVec2 &size, const char *overlay) {
    bars++; last_fraction = fraction; CHECK(fraction >= 0 && fraction <= 1);
    ImGui::ProgressBar(fraction, size, overlay);
}
#include "startup_prep_ui.inc"
static bool test_gate_ready() { return g_prep_finished.load(std::memory_order_acquire); }
static SDL_Event key(SDL_Scancode code, bool down = true) {
    SDL_Event e{}; e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP; e.key.scancode = code; return e;
}
static SDL_Event pad(Uint8 button, bool down = true) {
    SDL_Event e{}; e.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP; e.gbutton.button = button; e.gbutton.which = 1; return e;
}
static SDL_Event lifecycle(Uint32 type) { SDL_Event e{}; e.type = type; return e; }
static void prepare_draw() { startup_prep_draw(2, 10, PREP_WAIT); startup_prep_draw(2, 10, PREP_WAIT); }
static StartupPrepAction feed(const SDL_Event &e, StartupPrepTouch &touch) {
    startup_prep_input(&e, touch);
    StartupPrepAction action = PREP_WAIT;
    for (int i = 0; i < 4; i++) {
        action = startup_prep_draw(2, 10, action);
        if (!GImGui->InputEventsQueue.Size) return action;
    }
    CHECK(false); return action;
}
int main(int argc, char **argv) {
    CHECK(argc == 2); const char *mode = argv[1];
    if (!strcmp(mode, "touch_continue") || !strcmp(mode, "mouse_cancel") || !strcmp(mode, "touch_cancelled") || !strcmp(mode, "controller") || !strcmp(mode, "keyboard") || !strcmp(mode, "progress")) {
        prepare_draw(); StartupPrepTouch touch;
        if (!strcmp(mode, "progress")) {
            startup_prep_draw(-5, 10, PREP_WAIT); CHECK(last_fraction == 0);
            startup_prep_draw(20, 10, PREP_WAIT); CHECK(last_fraction == 1);
            unsigned old = bars;
            startup_prep_draw(10, 10, PREP_CONTINUE); startup_prep_draw(10, 10, PREP_QUIT);
            CHECK(bars == old);
        } else if (!strcmp(mode, "controller") || !strcmp(mode, "keyboard")) {
            bool keyboard = !strcmp(mode, "keyboard");
            ImGuiID cancel = ImGui::FindWindowByName("##shader-prep")->GetID("Cancel game");
            for (int i = 0; i < 4 && GImGui->NavId != cancel; i++) {
                feed(keyboard ? key(SDL_SCANCODE_TAB) : pad(SDL_GAMEPAD_BUTTON_DPAD_RIGHT), touch);
                feed(keyboard ? key(SDL_SCANCODE_TAB, false) : pad(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, false), touch);
            }
            CHECK(GImGui->NavId == cancel);
            CHECK(feed(keyboard ? key(SDL_SCANCODE_RETURN) : pad(SDL_GAMEPAD_BUTTON_SOUTH), touch) == PREP_QUIT);
        } else if (!strcmp(mode, "mouse_cancel")) {
            ImVec2 pos = cancel_rect.GetCenter(); SDL_Event e{};
            e.type = SDL_EVENT_MOUSE_BUTTON_DOWN; e.button.which = 1; e.button.button = SDL_BUTTON_LEFT; e.button.x = pos.x / 2; e.button.y = pos.y / 2;
            CHECK(feed(e, touch) == PREP_WAIT); e.type = SDL_EVENT_MOUSE_BUTTON_UP;
            CHECK(feed(e, touch) == PREP_QUIT);
        } else {
            ImVec2 pos = continue_rect.GetCenter(); SDL_Event e{};
            e.type = SDL_EVENT_FINGER_DOWN; e.tfinger.fingerID = 7; e.tfinger.x = pos.x / 1280; e.tfinger.y = pos.y / 720;
            CHECK(feed(e, touch) == PREP_WAIT);
            SDL_Event second = e; second.tfinger.fingerID = 8; startup_prep_input(&second, touch); CHECK(touch.id == 7);
            e.type = !strcmp(mode, "touch_cancelled") ? SDL_EVENT_FINGER_CANCELED : SDL_EVENT_FINGER_UP;
            CHECK(feed(e, touch) == (!strcmp(mode, "touch_cancelled") ? PREP_WAIT : PREP_CONTINUE));
            CHECK(!touch.active);
        }
        startup_prep_clear_input(); CHECK(!ImGui::GetIO().MouseDown[0] && !GImGui->InputEventsQueue.Size);
    } else {
        bool expected = true;
        if (!strcmp(mode, "stop_before")) { stop_before = true; expected = false; }
        else if (!strcmp(mode, "stop_during")) { stop_loop = 12; expected = false; }
        else if (!strcmp(mode, "quit")) { events.push_back({12, lifecycle(SDL_EVENT_QUIT)}); expected = false; }
        else if (!strcmp(mode, "back")) events.push_back({12, key(SDL_SCANCODE_AC_BACK)});
        else if (!strcmp(mode, "pad_back")) events.push_back({12, pad(SDL_GAMEPAD_BUTTON_BACK)});
        else if (!strcmp(mode, "quit_over_continue")) { events.push_back({12, key(SDL_SCANCODE_ESCAPE)}); events.push_back({12, lifecycle(SDL_EVENT_QUIT)}); events.push_back({12, key(SDL_SCANCODE_AC_BACK)}); expected = false; }
        else if (!strcmp(mode, "fast_stop")) { complete_loop = 0; events.push_back({0, lifecycle(SDL_EVENT_QUIT)}); expected = false; }
        else if (!strcmp(mode, "late_stop")) { complete_loop = 0; synchronous_worker = true; stop_at_check = 3; expected = false; }
        else if (!strcmp(mode, "ready_cancel")) { ready_cancel = true; expected = false; }
        else if (!strcmp(mode, "background_stop")) { events.push_back({2, lifecycle(SDL_EVENT_WILL_ENTER_BACKGROUND)}); stop_loop = 5; expected = false; }
        else if (!strcmp(mode, "background_restore")) { events.push_back({2, lifecycle(SDL_EVENT_WILL_ENTER_BACKGROUND)}); events.push_back({5, lifecycle(SDL_EVENT_DID_ENTER_FOREGROUND)}); }
        else if (!strcmp(mode, "hotplug")) { events.push_back({1, lifecycle(SDL_EVENT_GAMEPAD_ADDED)}); events.push_back({2, lifecycle(SDL_EVENT_GAMEPAD_REMOVED)}); }
        else if (!strcmp(mode, "thread_failure")) create_fails = true;
        else if (strcmp(mode, "normal")) return 2;
        CHECK(menu_shader_prep_startup() == expected);
        if (stop_before) CHECK(!create_calls && !joins);
        else if (create_fails) CHECK(create_calls == 1 && !joins);
        else CHECK(create_calls == 1 && joins == 1 && worker_done == 1 && resets == 1);
        if (!strcmp(mode, "normal")) CHECK(g_prep_done == 10 && !g_prep_cancel && bars);
        if (!strcmp(mode, "back") || !strcmp(mode, "pad_back")) CHECK(g_prep_cancel && g_prep_done == 2);
        if (!strcmp(mode, "hotplug")) CHECK(hotplugs == 2);
        if (!strcmp(mode, "background_stop")) CHECK(!bars && g_prep_cancel);
        if (!strcmp(mode, "late_stop")) CHECK(stop_checks == 3);
    }
    if (g_inited) ImGui::DestroyContext(g_ctx);
    printf("{\"assertions\":%u}\n", checks);
}
