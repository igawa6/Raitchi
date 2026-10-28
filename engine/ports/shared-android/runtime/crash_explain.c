// A fatal error, explained to the player: the crash dialog (plat_fatal_notice) says what went wrong in plain words
// and what to do about it; the technical lines (FATAL / signal, backtrace, registers) stay in the log for bug reports.
// Known causes are recognised from what the runtime knows when it stops: the fatal line, where the crash was (the
// game code, Raitchi itself, the graphics driver), and the game files.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "platform.h"
#include "rt.h"
#include "common_session.h"

static char g_line[512];   // the first fatal line (FATAL message / "signal 11 accessing 0x10")
static uint64_t g_pc;      // a crash's host pc (0: a FATAL)
int rt_romfs_files = -1;   // the game's own RomFS files (runtime/services/fs.c; -1: not read yet)
enum { ROMFS_FILES = 3836 };  // LAS versions qualified by the launcher only.

#ifdef __ANDROID__
void vk_gpu_driver_runtime_failure(const char *message);
#endif

void rt_fatal_msg(const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    fprintf(stderr, "FATAL: %s", b);
    if (!g_line[0]) {
        snprintf(g_line, sizeof g_line, "%s", b);
#ifdef __ANDROID__
        vk_gpu_driver_runtime_failure(g_line);
#endif
    }
}

void rt_crash_fault(const char *what, uint64_t fa, uint64_t pc) {
    if (!g_line[0]) snprintf(g_line, sizeof g_line, "%s accessing %#llx", what, (unsigned long long)fa);
    if (!g_pc) g_pc = pc;
}

static bool has(const char *s) { return strstr(g_line, s) != NULL; }

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c: inside the game code?

static bool contains_nocase(const char *hay, const char *needle) {  // (no strcasestr on Windows)
    for (; *hay; hay++) {
        size_t i = 0;
        while (needle[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (!needle[i]) return true;
    }
    return false;
}

// The graphics driver's own code (the crash was inside it, not in Raitchi): Vulkan drivers on Windows / Linux,
// Apple's Metal driver
static bool in_gpu_driver(uint64_t pc) {
    char m[512];
    if (!pc || !plat_module_name(pc, m, sizeof m)) return false;
    static const char *const drivers[] = {"nvoglv", "nvgpucomp", "nvwgf", "libnvidia", "amdvlk", "amdxc", "atiumd",
                                          "atig6", "igvk", "igc64", "igd", "libvulkan_radeon", "libvulkan_intel",
                                          "libvulkan_nouveau", "libvulkan_lvp", "libvulkan_virtio", "AGXMetal",
                                          "MTLCompiler", "libLLVM", "radeonsi",
                                          // Android: Turnip / Qualcomm's (vulkan.adreno.so, vulkan.ad07xx.so), Mali,
                                          // PanVK, Xclipse, PowerVR
                                          "libvulkan_freedreno", "vulkan.ad", "libGLES_mali", "vulkan.mali",
                                          "libvulkan_panfrost", "vulkan.samsung", "libvulkan_sgpu", "vulkan.powervr",
                                          "libllvm-glnext", "libllvm-qgl"};
    for (size_t i = 0; i < sizeof drivers / sizeof *drivers; i++)
        if (contains_nocase(m, drivers[i])) return true;
    return false;
}

void rt_crash_explain(char *out, size_t n, const char *log_path) {
    static const char *const report = "Please report it as a GitHub issue and attach the log file below. "
                                      "Your progress up to your last save or autosave is safe.";
    char head[160], what[400];
    if (has("native guest reservation failed for ")) {
        snprintf(head, sizeof head, "This game's memory layout is unavailable on this device");
        snprintf(what, sizeof what, "Raitchi could not reserve the addresses this game needs before starting it. "
                 "Please report this device compatibility issue and attach the log file below.\n\n%.220s", g_line);
    } else if (has("native title selection failed for ") || has("native layout activation failed for ")) {
        snprintf(head, sizeof head, "The game session could not start");
        snprintf(what, sizeof what, "Close Raitchi completely and start it again. If this keeps happening, "
                 "please report it and attach the log file below.\n\n%.220s", g_line);
    } else if (shared_title_las() && rt_romfs_files >= 0 && rt_romfs_files < ROMFS_FILES) {
        snprintf(head, sizeof head, "Your game files are incomplete");
        snprintf(what, sizeof what, "Raitchi found %d of the game's %d files, and the game stopped when it needed "
                 "one that's missing. Copy your game files again so none are missing, then set them up again on the "
                 "start screen (Game Files).", rt_romfs_files, (int)ROMFS_FILES);
    } else if (has("doesn't fit its language")) {
        snprintf(head, sizeof head, "Your game files' text has been changed");
        snprintf(what, sizeof what, "The game's text for this language comes from another language (a translation "
                 "patch), and the game stopped at a question it can't show that way. Set up unmodified game files on "
                 "the start screen (Game Files). To play in another language, pick it in Settings (Language).");
    } else if (has("built for another version")) {
        snprintf(head, sizeof head, "The game code was built for other game files or another version of Raitchi");
        snprintf(what, sizeof what, "Start Raitchi again: the start screen rebuilds the game code for these game "
                 "files by itself.");
    } else if (has("unsupported version of the game")) {
        snprintf(head, sizeof head, "Wrong version of the game");
        snprintf(what, sizeof what, "Raitchi needs version 1.0.0, 1.0.1 or 1.1.0 of the game. Set up those game "
                 "files on the start screen (Game Files).");
    } else if (has("no font for the game's text")) {
        snprintf(head, sizeof head, "No font for the game's text");
        snprintf(what, sizeof what, "Import the Switch fonts in Settings (Game font), or reinstall Raitchi - it "
                 "comes with a font of its own.");
    } else if (has("get_terminate") || has("uncaught C++ exception")) {  // (the game threw: recompiled code can't
                                                                         // unwind; runtime/hle/warp.c rt_cxa_throw)
        snprintf(head, sizeof head, "The game ran into an error it couldn't recover from");
        snprintf(what, sizeof what, "This is a bug in Raitchi, not a problem with your game files or PC. %s", report);
    } else if (in_gpu_driver(g_pc)) {
        snprintf(head, sizeof head, "Your graphics driver crashed");
#ifdef __ANDROID__
        snprintf(what, sizeof what, "Try another GPU driver: install one (Mesa Turnip for Adreno, Mesa PanVK for Mali), "
                 "or go back to the system driver if one is installed. If it keeps happening, please report it with "
                 "the log file below.");
#else
        snprintf(what, sizeof what, "Update your graphics driver (NVIDIA, AMD or Intel) and try again. If it keeps "
                 "happening, please report it with the log file below.");
#endif
    } else if (has("VK_") || has("Vulkan")) {
        snprintf(head, sizeof head, "Graphics driver error");
#ifdef __ANDROID__
        snprintf(what, sizeof what, "Raitchi needs a Vulkan driver with Vulkan 1.3 (or 1.1 with its extensions), "
                 "and your phone's own driver didn't start the game. Install a GPU driver (a .zip as emulators use "
                 "them): Mesa Turnip for Adreno, Mesa PanVK for Mali. If one is installed already, try the system "
                 "driver. If it keeps happening, please report it with the log file below.");
#else
        snprintf(what, sizeof what, "Raitchi needs a graphics driver with Vulkan 1.3: install the latest driver for "
                 "your GPU (NVIDIA, AMD or Intel). If it keeps happening, please report it with the log file below.");
#endif
    } else if (has("out of memory") || has("cannot allocate") || has("alloc failed")) {
        snprintf(head, sizeof head, "Out of memory");
        snprintf(what, sizeof what, "Close other programs and try again.");
    } else if (g_pc && rt_host_fn_name(g_pc, NULL)) {
        snprintf(head, sizeof head, "The game crashed");
        snprintf(what, sizeof what, "This is a bug in Raitchi, not a problem with your game files or PC. %s", report);
    } else {
        snprintf(head, sizeof head, "Raitchi stopped because of an error");
        snprintf(what, sizeof what, "%s", report);
    }
    fprintf(stderr, "[crash] %s\n", head);
    if (log_path && *log_path) snprintf(out, n, "%s\n\n%s\n\nLog file:\n%s", head, what, log_path);
    else snprintf(out, n, "%s\n\n%s", head, what);
}
