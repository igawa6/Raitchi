/* Shared Android session entry, derived from LAS/Let's Go/Echoes startup policies.
 * Copyright (c) 2026 Peiikai; Copyright (c) 2026 raulzito12345rrs.
 * Copyright (c) 2026 EoWRecomp contributors.
 * MIT: native/runtime/LICENSE-LASRecomp, LICENSE-LetsGo and
 * native/adapters/echoes/LICENSE. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <unistd.h>
#include "gamelib.h"
#include "game_addresses.h"
#include "hle.h"
#include "kernel.h"
#include "paths.h"
#include "settings.h"
#include "native_session.h"

const char *g_romfs_dir;
static _Atomic int reboot_request;
static char stop_request_path[4200];
void rt_request_reboot(int kind) { atomic_store(&reboot_request, kind); }
int rt_reboot_requested(void) {
    int requested=atomic_load(&reboot_request);
    if(requested || !stop_request_path[0])return requested;
    if(!access(stop_request_path,F_OK)) {
        FILE *request=fopen(stop_request_path,"r");
        long pid=0;
        if(request){if(fscanf(request,"%ld",&pid)!=1)pid=0;fclose(request);}
        if(pid==(long)getpid()) {
            unlink(stop_request_path);
            atomic_store(&reboot_request,1);
            LOG("[shared] launcher requested orderly game shutdown\n");
            return 1;
        }
    }
    return 0;
}
void reboot_test_frame(uint64_t frame) { (void)frame; /* fresh processes only; no timed reboot harness */ }

static void run_init_stage(Cpu *cpu, int early) {
    const TitleStartupPolicy *startup = &title_adapter_selected()->startup;
    uint32_t count = early ? startup->early_count : startup->late_count;
    const uint32_t *ids = early ? startup->early_modules : startup->late_modules;
    for (uint32_t i = 0; i < count; ++i) {
        Module *module = NULL;
        for (int m = 0; m < g_nmodules; ++m)
            if (shared_module_id((uint32_t)m) == ids[i]) module = &g_modules[m];
        if (!module) FATAL("startup module policy is inconsistent\n");
        LOG("[shared] %s init %s\n", early ? "early" : "late", module->name);
        if (module->init) guest_call(cpu, module->init);
        else for (uint64_t k = 0; k < module->init_array_sz / 8; ++k) {
            uint64_t function = ((const uint64_t *)(uintptr_t)module->init_array)[k];
            if (function) guest_call(cpu, function);
        }
    }
}
static void init_sdk(Cpu *cpu) { run_init_stage(cpu, 1); }
static void init_main(Cpu *cpu) { run_init_stage(cpu, 0); }
/* SDK16.2 invokes x4 after nnMain. Match the pinned Echoes rtld policy: a real
 * host callback acknowledges normal completion before SDK quick-exit; it does
 * not fabricate or invoke unverified guest destructors. */
static void fini_main(Cpu *cpu) { (void)cpu; LOG("[shared] nnMain returned\n"); }

static void environment_file(void) {
    FILE *file = fopen("env.txt", "r");
    if (!file) return;
    char line[4096];
    while (fgets(line, sizeof line, file)) {
        line[strcspn(line, "\r\n")] = 0;
        char *equals = strchr(line, '=');
        if (line[0] == '#' || !equals || equals == line) continue;
        *equals = 0;
        setenv(line, equals + 1, 1);
    }
    fclose(file);
}

static void session_directory(void) {
    char directory[256];
    mkdir("sessions", 0755);
    snprintf(directory, sizeof directory, "sessions/%s%s", title_adapter_selected()->save_namespace,
        native_session_suffix());
    if ((mkdir(directory, 0755) && access(directory, W_OK)) || chdir(directory))
        FATAL("cannot create the selected title's isolated state directory\n");
    mkdir("save", 0755);
    /* Separate title state/cache/settings, without copying or changing reference saves. */
    setenv("LARECOMP_SAVE_DIR", "save", 1);
}

int main(int argc, char **argv) {
    plat_init();
    plat_thread_name("recomp-main");
    rt_paths_init();
    if(snprintf(stop_request_path,sizeof stop_request_path,"%s/game-stop.request",rt_data_root()) >= (int)sizeof stop_request_path)
        FATAL("shared stop request path too long\n");
#ifdef __ANDROID__
    // Preserve the pinned Android baseline's Turnip depth workaround. LRZ can
    // incorrectly reject discarded fragments in video, water and occluder passes.
    // Read env.txt afterwards so an explicit owner setting still takes precedence.
    setenv("TU_DEBUG", "nolrz", 0);
#endif
    environment_file();
    const char *root = getenv("LARECOMP_GAME_ROOT");
    if (!root || !*root) root = "extracted";
    for (int i = 1; i < argc; ++i)
        if (argv[i][0] != '-') root = argv[i];
    char absolute[4096], exefs[4200], npdm[4300], romfs[4200], why[512];
    if (!realpath(root, absolute)) FATAL("selected game root is missing\n");
    snprintf(exefs, sizeof exefs, "%s/exefs", absolute);
    snprintf(npdm, sizeof npdm, "%s/main.npdm", exefs);
    snprintf(romfs, sizeof romfs, "%s/romfs", absolute);
    g_romfs_dir = strdup(romfs);

    if (!gamelib_load(absolute, why, sizeof why)) FATAL("%s\n", why);
    if (shared_title_las() && !game_version_select(absolute, why, sizeof why)) FATAL("%s\n", why);
    session_directory();
    settings_load();
    /* Retain supported user graphics choices; unsupported enhancements stay off. */
    g_settings.rando_seed[0] = 0;
    g_settings.frame_gen = 0;
    g_settings.mirror_mode = false;
    setenv("LARECOMP_FPS60", g_settings.frame_rate == FRAME_RATE_NATIVE_60 ? "1" : "0", 1);
    if (title_adapter_selected()->id == TITLE_EEVEE) {
        const char *unsupported = getenv("LARECOMP_G1_READERS");
        if (unsupported && *unsupported) FATAL("Eevee timing research requires its optional adapter\n");
    }
    extern void ui_window_init(void);
    extern void backend_guest_heap_prepare(void);
    ui_window_init();
    backend_guest_heap_prepare();
    loader_load(exefs);
    kernel_init();
    dispatch_init();
    native_session_start();

    /* The renderer already has its Vulkan device. Replay the known shader
     * cache before the guest starts, while the native window loop can still
     * show progress and accept Back to skip the wait. */
    extern bool menu_shader_prep_startup(void);
    extern void vk_shader_preparation_mode(bool preparing);
    vk_shader_preparation_mode(true);
    bool preparation_continue = menu_shader_prep_startup();
    vk_shader_preparation_mode(false);
    if (!preparation_continue) {
        // Closing the preparation window is terminal. A skipped wait normally
        // continues in the background; here finish those jobs before saving
        // the driver cache and ending the process.
        extern void vk_headstart_finish(void);
        vk_headstart_finish();
        native_session_finish();
        extern void backend_quit(void);
        backend_quit();
        return 0;
    }

    uint32_t stack_size = 0x100000;
    int priority = 44;
    FILE *file = fopen(npdm, "rb");
    if (!file) FATAL("main.npdm is missing\n");
    uint8_t meta[0x80], aci[0x18];
    if (fread(meta, 1, sizeof meta, file) != sizeof meta || memcmp(meta, "META", 4))
        FATAL("main.npdm META header rejected\n");
    priority = meta[0xe];
    memcpy(&stack_size, meta + 0x1c, sizeof stack_size);
    uint32_t aci_offset;
    memcpy(&aci_offset, meta + 0x70, sizeof aci_offset);
    if (fseek(file, aci_offset, SEEK_SET) || fread(aci, 1, sizeof aci, file) != sizeof aci || memcmp(aci, "ACI0", 4))
        FATAL("main.npdm ACI0 header rejected\n");
    fclose(file);
    uint64_t program_id;
    memcpy(&program_id, aci + 0x10, sizeof program_id);
    if (program_id != title_adapter_selected()->program_id || priority > 63 || meta[0xf] > 3 ||
        !stack_size || (stack_size & 0xfff) || stack_size > STACK_SIZE)
        FATAL("main.npdm does not match the selected title/thread policy\n");
    if (kernel_configure_thread_core(meta[0xf]))
        FATAL("NPDM default thread core rejected\n");
    uint32_t system_resource_size;
    memcpy(&system_resource_size,meta+0x14,sizeof system_resource_size);
    if(kernel_configure_system_resources(system_resource_size))
        FATAL("NPDM system-resource size or physical-memory backing policy rejected\n");
    uint32_t argument_count = title_adapter_selected()->startup.argument_count;
    if (!argument_count && title_adapter_selected()->id == TITLE_GENERIC) {
        /* Generic adapter: no measured SDK start signature. Use the one the
         * loaded SDK exports (5 arguments since SDK16, else 4). */
        argument_count = loader_find_symbol("_ZN2nn4init5StartEmmPFvvES2_S2_") ? 5 : 4;
        LOG("[generic] SDK exports the %u-argument nn::init::Start\n", argument_count);
    }
    const char *startup_symbol = argument_count == 4 ? "_ZN2nn4init5StartEmmPFvvES2_" :
        argument_count == 5 ? "_ZN2nn4init5StartEmmPFvvES2_S2_" : NULL;
    uint64_t start = startup_symbol ? loader_find_symbol(startup_symbol) : 0;
    if (!start)
        FATAL("unsupported SDK startup signature\n");
    uint64_t early = hle_register("<shared_init_early>", init_sdk);
    uint64_t late = hle_register("<shared_init_late>", init_main);
    uint64_t fini = argument_count == 5 ? hle_register("<shared_fini>", fini_main) : 0;
    KThread *thread = kernel_create_main_thread(start, stack_size, priority);
    if (argument_count == 5) kernel_run_main5(thread, thread->handle, 0, early, late, fini);
    else kernel_run_main(thread, thread->handle, 0, early, late);
    (void)ui_run();
    /* A title switch or requested reboot always ends this process. */
    kernel_stop_threads(native_session_continuous() ? native_session_stop_idle : NULL);
    native_session_finish();
    extern void backend_quit(void);
    backend_quit();
    /* Queued command handles may reference any retained shadow generation.
     * Guests are joined and both render workers/GPU are now idle. */
    extern void nvn_command_shadow_finish_after_gpu_idle(void);
    nvn_command_shadow_finish_after_gpu_idle();
    return 0; /* SDL finishes the Activity, stops PlayService, then kills :game. */
}
