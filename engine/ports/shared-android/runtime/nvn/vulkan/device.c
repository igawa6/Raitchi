/* Shared runtime overlay derived from LAS Recomp Android at
 * cca791a3e7218e1dac95e7d4a11b8f97dba3ea51, Copyright (c) 2026 Peiikai.
 * Android uint32/read-only snapshot adaptation also derives from Let's Go Recomp
 * 96894fd8e9ea20c1b38cdea53105ec2a335df620, Copyright (c) 2026 raulzito12345rrs.
 * MIT notices: native/runtime/LICENSE-LASRecomp and LICENSE-LetsGo. */
// Vulkan device, window, swapchain and presentation (counterpart of the window/present half of metal.m).
#include "../../shared_gpu_cache.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <math.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#endif

#include "../../paths.h"
#include "../../platform.h"
#include "../../rt.h"  // (HEAP_MAX: lazy_heap_predicted)
#include "../../settings.h"
#include "../../ui/menu.h"
#include "../rqueue.h"
#include "vk.h"
#include "record.h"
#include "../output_scale.h"
#include "../renderer.h"
#include "../maxwell/shader.h"
#ifdef __ANDROID__
#include <adrenotools/driver.h>
#include <dlfcn.h>
#include "../../platform/android.h"
#endif
#include "../pace.h"
#include "../shader_prep.h"
#include <pthread.h>

VkG g_vk;
static SDL_Window *window;

// The system memory heap a whole-heap import (exec.c host_import) lands in and what it can hold: the largest heap with
// a host-visible memory type that isn't device-local (all of them device-local: the largest host-visible one), the
// smaller of its size and its budget (budget 0: unknown). -1: no host-visible heap. Plain types, so
// tools/test_gmem_lowmem.py builds it without Vulkan headers.
typedef struct { uint64_t size, budget; bool device_local, host_visible; } ImportHeap;
static int import_heap_room(const ImportHeap *h, uint32_t n, uint64_t *room) {
    int best = -1;
    for (int pass = 0; pass < 2 && best < 0; pass++)
        for (uint32_t i = 0; i < n; i++)
            if (h[i].host_visible && (pass || !h[i].device_local) && (best < 0 || h[i].size > h[best].size)) best = (int)i;
    if (best >= 0) *room = h[best].budget && h[best].budget < h[best].size ? h[best].budget : h[best].size;
    return best;
}
// What the heap must hold beyond the import: the transient ring's chunks once they spill to system memory (64 MB
// each), readback, the driver's own. Issue #205 (8 GB laptop, Ryzen 5 5500U, AMD's Windows driver): system heap
// 2738 MB, budget 2601 - the 3220 MB import was accepted, then the first submission failed with out of device memory.
// An 8 GB desktop with a GPU of its own (system heap ~3950 MB, budget ~3840) stays on one import.
#define IMPORT_MARGIN (256ull << 20)
// Under this much room one import should fit but might not (a driver's own limits, other programs): the import is
// tried once at the start (exec.c whole_import_probe_oom), with buffer device addresses enabled so lazy chunks can take
// over. Machines with more (an RTX with 16 GB or more of RAM) skip the probe and keep the device as before.
#define IMPORT_PROBE_BELOW (2 * HEAP_MAX)

// Whether one import of the whole heap fits its system memory heap (import_heap_room), from the physical device;
// *probe: it fits, but not by much (IMPORT_PROBE_BELOW), or LARECOMP_VK_SIM_HEAP_OOM=1 (the probe then fails).
static bool whole_import_fits(bool budget_ext, bool *probe) {
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 mp2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
                                             .pNext = budget_ext ? &bp : NULL};
    vkGetPhysicalDeviceMemoryProperties2(g_vk.phys, &mp2);
    const VkPhysicalDeviceMemoryProperties *mp = &mp2.memoryProperties;
    ImportHeap h[VK_MAX_MEMORY_HEAPS] = {{0}};
    for (uint32_t i = 0; i < mp->memoryHeapCount; i++) {
        h[i].size = mp->memoryHeaps[i].size;
        h[i].budget = budget_ext ? bp.heapBudget[i] : 0;
        h[i].device_local = (mp->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
    }
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if (mp->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) h[mp->memoryTypes[i].heapIndex].host_visible = true;
    uint64_t room = 0;
    int heap = import_heap_room(h, mp->memoryHeapCount, &room);
    const char *sim = getenv("LARECOMP_VK_SIM_HEAP_OOM");
    if (heap < 0 || room >= HEAP_MAX + IMPORT_MARGIN) {
        *probe = (heap >= 0 && room < IMPORT_PROBE_BELOW) || (sim && atoi(sim));
        return true;
    }
    LOG("[vk] guest heap: memory heap %d holds %llu MB (size %llu, budget %llu), one import needs %llu MB: "
        "imported in chunks as the game binds them\n", heap, (unsigned long long)(room >> 20),
        (unsigned long long)(h[heap].size >> 20), (unsigned long long)(h[heap].budget >> 20),
        (unsigned long long)((HEAP_MAX + IMPORT_MARGIN) >> 20));
    return false;
}

// Whether the guest heap will be imported chunk by chunk (exec.c gmem_lazy): on a GPU whose storage buffers can't
// span the heap (maxStorageBufferRange < HEAP_MAX: Intel, 1 GB); where the system memory heap or its budget can't hold
// one import of the whole heap (whole_import_fits: 8 GB laptops, issue #205); and on RADV when the dma-buf heap
// (exec.c guest_heap_dmabuf) has no cached host-visible memory type that holds it - the same test, which needs only
// the physical device - or LARECOMP_VK_GMEM_LAZY=1 (=0: never; a GPU with a short range then gets windows of one
// import, exec.c gmem_chunk_shift). Decided before the device exists because the lazy import needs buffer device
// addresses, and on RADV enabling them puts every allocation on the driver's global BO list (amdgpu checks all of
// them each submission): a device that imports the heap another way shouldn't pay for that.
// *probe: one import is to be tried first (whole_import_fits), lazy chunks if it runs out of memory.
static bool lazy_heap_predicted(bool budget_ext, bool *probe) {
    *probe = false;
    const char *e = getenv("LARECOMP_VK_GMEM_LAZY");
    if (e) return atoi(e) != 0;
    if (getenv("LARECOMP_VK_NO_GMEM")) return false;
#ifdef __ANDROID__
    return false;  // (phones: the heap through the address table, or one import - exec.c mirror_table, gmem_info)
#endif
    uint64_t lim = g_vk.props.limits.maxStorageBufferRange;
    if (lim && lim < HEAP_MAX) return true;  // (UHD 770 main pass: lazy 8.4 ms, 512 MB windows 8.5)
    if (!whole_import_fits(budget_ext, probe)) return true;
    if (!strstr(g_vk.props.deviceName, "RADV")) return false;
    *probe = false;  // (RADV: as before - the dma-buf heap or lazy chunks, no probe)
#if defined(__linux__) && !defined(__ANDROID__)
    const char *d = getenv("LARECOMP_VK_HEAP_DMABUF");
    if ((d ? atoi(d) != 0 : true) && (g_vk.dmabuf || g_vk.map_placed)) {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((mp.memoryTypes[i].propertyFlags & want) == want && mp.memoryHeaps[mp.memoryTypes[i].heapIndex].size >= HEAP_MAX) return false;
    }
#endif
    return true;
}
static atomic_bool ui_ready;
static VkDebugUtilsMessengerEXT messenger;
static void exit_after_shot(void);  // (UI thread section)

// ================================================================== instance / device
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)type, (void)user;
    static atomic_uint n;
    bool stop = sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT && getenv("LARECOMP_VK_VALIDATION_STOP");
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT &&
        (stop || atomic_fetch_add_explicit(&n, 1, memory_order_relaxed) < 500)) LOG("[vk] %s\n", d->pMessage);
    if (stop) {
        LOG("[vk] stopping at the first validation error\n");
        fflush(stderr);
        _Exit(EXIT_FAILURE);  // do not run GPU cleanup after the validation failure
    }
    return VK_FALSE;
}

#ifdef __ANDROID__
// ---- a Vulkan driver the player installed (Mesa Turnip on Adreno ...: Video settings, GpuDrivers.java) in place of the
// system's. libadrenotools loads a private copy of the system's Vulkan loader whose driver lookup is hooked to the
// installed driver; volk takes its vkGetInstanceProcAddr. SDL's own handle is on the system loader, which doesn't know
// that instance: the window surface is then made here (create_surface). Anything failing: the system driver, logged.
static bool custom_driver_on;
static char driver_probe[512];  // GpuDrivers' marker: removed once the driver has presented a few frames
static pthread_mutex_t driver_probe_lock = PTHREAD_MUTEX_INITIALIZER;
static char driver_failed[256];
bool g_vk_shader_stats;  // (exec.c build_pipeline)  // the installed driver that didn't start (this time or last time), for the settings

// The settings' GPU driver rows (runtime/ui/menu.mm): whether the installed driver runs; failed: one that didn't start
bool vk_gpu_driver_custom(const char **failed) {
    if (failed) *failed = driver_failed[0] ? driver_failed : NULL;
    return custom_driver_on;
}

// A known recorder rejection is a host compatibility stop, not evidence that
// the selected Vulkan driver failed. Signals and all other fatals keep the probe.
void vk_gpu_driver_runtime_failure(const char *message) {
    if (!message || strcmp(message, "nvn: recording cannot chain a command chunk to itself\n") ||
        !atomic_load(&ui_ready) || !custom_driver_on)
        return;
    pthread_mutex_lock(&driver_probe_lock);
    if (driver_probe[0]) {
        if (!unlink(driver_probe) || errno == ENOENT) {
            driver_probe[0] = 0;
            LOG("[vk] driver startup marker cleared after controlled command-recorder stop\n");
        } else {
            LOG("[vk] cannot clear driver startup marker after controlled stop (errno %d)\n", errno);
        }
    }
    pthread_mutex_unlock(&driver_probe_lock);
}

static bool custom_driver(void) {
    AndroidGpuDriver d;
    if (!android_gpu_driver_config(&d)) {
        const char *failed = android_gpu_driver_failed();
        if (failed) LOG("[vk] GPU driver %s failed at its last start: back to the system driver\n", failed);
        if (failed) snprintf(driver_failed, sizeof driver_failed, "%s", failed);
        return false;
    }
    void *lib = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM, d.tmp_dir, d.hook_dir, d.dir, d.lib,
                                           NULL, NULL);
    PFN_vkGetInstanceProcAddr gipa = lib ? (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr") : NULL;
    if (!gipa) {  // (the probe stays: the next start goes back to the system driver and says so)
        LOG("[vk] GPU driver %s%s: %s (hooks in %s); using the system driver\n", d.dir, d.lib,
            lib ? "no vkGetInstanceProcAddr" : "libadrenotools could not load it", d.hook_dir);
        snprintf(driver_failed, sizeof driver_failed, "%s", d.lib);
        return false;
    }
    volkInitializeCustom(gipa);
    snprintf(driver_probe, sizeof driver_probe, "%s", d.probe);
    LOG("[vk] GPU driver: %s%s\n", d.dir, d.lib);
    return custom_driver_on = true;
}
#endif

// LARECOMP_VK_VALIDATION=1: Khronos validation layer (MSYS2 package vulkan-validation-layers)
// LARECOMP_VK_VALIDATION_STOP=1: require that layer and its callback, and exit nonzero on the first error.
static bool validating;  // the validation layer is on (validation_exit)
static void create_instance(void) {
    Uint32 n_sdl = 0;
    const char *const *sdl_ext = SDL_Vulkan_GetInstanceExtensions(&n_sdl);
    const char *ext[16];
    uint32_t n_ext = 0;
    for (Uint32 i = 0; i < n_sdl && n_ext < 14; i++) ext[n_ext++] = sdl_ext[i];
    bool strict = getenv("LARECOMP_VK_VALIDATION_STOP") != NULL;
    bool validate = strict || getenv("LARECOMP_VK_VALIDATION") != NULL;
    if (validate) ext[n_ext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
#ifdef VK_NO_PROTOTYPES
#ifdef __ANDROID__
    if (!custom_driver())
#endif
    if (volkInitialize() != VK_SUCCESS) FATAL("no Vulkan driver\n");
#endif
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "LARecomp",
                             .pEngineName = "LARecomp", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext,
                               .enabledLayerCount = validate ? 1 : 0, .ppEnabledLayerNames = layers};
    VkResult r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    if (r != VK_SUCCESS && validate) {
        if (strict) {
            LOG("[vk] cannot create an instance with required validation (%d)\n", r);
            fflush(stderr);
            _Exit(EXIT_FAILURE);
        }
        LOG("[vk] validation layer unavailable (%d); continuing without\n", r);
        ci.enabledLayerCount = 0;
        ci.enabledExtensionCount--;
        validate = false;
        r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    }
#ifdef __ANDROID__
    if (r != VK_SUCCESS && custom_driver_on) {
        LOG("[vk] vkCreateInstance with the installed GPU driver: %d; using the system driver\n", r);
        snprintf(driver_failed, sizeof driver_failed, "The installed driver");
        custom_driver_on = false, driver_probe[0] = 0;  // (the probe stays: the next start is back on the system driver)
        if (volkInitialize() != VK_SUCCESS) FATAL("no Vulkan driver\n");
        r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    }
#endif
    validating = validate;
    if (r != VK_SUCCESS) FATAL("vkCreateInstance: %d (is a Vulkan 1.3 driver installed?)\n", r);
#ifdef VK_NO_PROTOTYPES
    volkLoadInstance(g_vk.inst);
#endif
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT f =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_cb};
        VkResult result = f ? f(g_vk.inst, &mi, NULL, &messenger) : VK_ERROR_EXTENSION_NOT_PRESENT;
        if (result != VK_SUCCESS && strict) {
            LOG("[vk] required validation callback unavailable (%d)\n", result);
            fflush(stderr);
            _Exit(EXIT_FAILURE);
        }
        if (result == VK_SUCCESS) LOG("[vk] validation enabled%s\n", strict ? " (stop on error)" : "");
    }
}

static bool has_ext(VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return true;
    return false;
}

#ifdef VK_NO_PROTOTYPES
// Drivers below Vulkan 1.3 (most phones up to Android 13): what of 1.2 / 1.3 the backend uses, as extensions. The
// backend then runs exactly its 1.3 code: the extensions' functions take the core names' place (volk's pointers,
// compat_aliases) and their structures are the same. `below`: the version from which it is core.
static const struct { const char *name; uint32_t below; } COMPAT_EXT[] = {
    {VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, VK_API_VERSION_1_3},
    {VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME, VK_API_VERSION_1_2},  // (dynamic rendering needs these two)
    {VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, VK_API_VERSION_1_2},
    {VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, VK_API_VERSION_1_3},
    {VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME, VK_API_VERSION_1_3},
    {VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME, VK_API_VERSION_1_3},
    {VK_EXT_SHADER_DEMOTE_TO_HELPER_INVOCATION_EXTENSION_NAME, VK_API_VERSION_1_3},
    {VK_KHR_SHADER_TERMINATE_INVOCATION_EXTENSION_NAME, VK_API_VERSION_1_3},
    {VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME, VK_API_VERSION_1_2},
    {VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME, VK_API_VERSION_1_2},
    {VK_KHR_8BIT_STORAGE_EXTENSION_NAME, VK_API_VERSION_1_2},
    {VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME, VK_API_VERSION_1_2},
    {VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME, VK_API_VERSION_1_2},
};

// Whether a device below 1.3 has every extension it needs (missing ones logged)
static bool compat_ok(VkPhysicalDevice pd, uint32_t api) {
    if (api < VK_API_VERSION_1_1) return false;
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ep);
    bool ok = true;
    for (size_t i = 0; i < sizeof COMPAT_EXT / sizeof *COMPAT_EXT; i++)
        if (api < COMPAT_EXT[i].below && !has_ext(ep, ne, COMPAT_EXT[i].name)) {
            LOG("[vk]   missing %s\n", COMPAT_EXT[i].name);
            ok = false;
        }
    free(ep);
    return ok;
}

static void compat_aliases(void) {
    if (g_vk.api < VK_API_VERSION_1_3) {
        vkCmdBeginRendering = vkCmdBeginRenderingKHR;
        vkCmdEndRendering = vkCmdEndRenderingKHR;
        vkCmdPipelineBarrier2 = vkCmdPipelineBarrier2KHR;
        vkQueueSubmit2 = vkQueueSubmit2KHR;
        vkCmdWriteTimestamp2 = vkCmdWriteTimestamp2KHR;
        vkCmdSetCullMode = vkCmdSetCullModeEXT;
        vkCmdSetFrontFace = vkCmdSetFrontFaceEXT;
        vkCmdSetPrimitiveTopology = vkCmdSetPrimitiveTopologyEXT;
        vkCmdSetDepthTestEnable = vkCmdSetDepthTestEnableEXT;
        vkCmdSetDepthWriteEnable = vkCmdSetDepthWriteEnableEXT;
        vkCmdSetDepthCompareOp = vkCmdSetDepthCompareOpEXT;
        vkCmdSetStencilTestEnable = vkCmdSetStencilTestEnableEXT;
        vkCmdSetStencilOp = vkCmdSetStencilOpEXT;
        vkCmdSetPrimitiveRestartEnable = vkCmdSetPrimitiveRestartEnableEXT;
        vkCmdSetDepthBiasEnable = vkCmdSetDepthBiasEnableEXT;
    }
    if (g_vk.api < VK_API_VERSION_1_2) {
        vkWaitSemaphores = vkWaitSemaphoresKHR;
        vkGetSemaphoreCounterValue = vkGetSemaphoreCounterValueKHR;
        vkSignalSemaphore = vkSignalSemaphoreKHR;
    }
}
#endif

// Discrete GPU first (LARECOMP_GPU=<index> picks one), needs Vulkan 1.3 and a queue that can present.
static void pick_device(void) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_vk.inst, &n, NULL);
    if (!n) FATAL("no Vulkan device\n");
    VkPhysicalDevice *pd = calloc(n, sizeof *pd);
    vkEnumeratePhysicalDevices(g_vk.inst, &n, pd);
    int want = getenv("LARECOMP_GPU") ? atoi(getenv("LARECOMP_GPU")) : -1, best = -1, best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        LOG("[vk] gpu %u: %s (api %u.%u.%u)\n", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
            VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
#ifdef VK_NO_PROTOTYPES
        if (p.apiVersion < VK_API_VERSION_1_3 && !compat_ok(pd[i], p.apiVersion)) continue;
#else
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
#endif
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if ((int)i == want) score = 100;
        if (score > best_score) { best_score = score; best = (int)i; }
    }
    if (getenv("LARECOMP_TEST_NO_DEVICE")) best = -1;  // (test: the start screen's driver error and its dialog)
    if (best < 0) FATAL("no Vulkan 1.3 device\n");
    g_vk.phys = pd[best];
    free(pd);
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    g_vk.api = g_vk.props.apiVersion < VK_API_VERSION_1_3 ? VK_MAKE_API_VERSION(0, 1, VK_API_VERSION_MINOR(g_vk.props.apiVersion), 0)
                                                          : VK_API_VERSION_1_3;
    {
        VkPhysicalDeviceSubgroupProperties sg = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &sg};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
        maxwell_frag_subgroups = (sg.supportedStages & VK_SHADER_STAGE_FRAGMENT_BIT) &&
                                 (sg.supportedOperations & VK_SUBGROUP_FEATURE_SHUFFLE_BIT);
        /* Maxwell votes operate on 32-lane warps. The translator partitions a
         * supported Vulkan ballot into 32-lane words; unsupported stages fail. */
        maxwell_vote_stages = 0;
        if ((sg.supportedOperations & (VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT)) ==
                (VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT) &&
            sg.subgroupSize >= 32 && sg.subgroupSize <= 128 && !(sg.subgroupSize & 31)) {
            const VkShaderStageFlags stages[] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT,
                VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_COMPUTE_BIT};
            for (unsigned stage = 0; stage < sizeof stages / sizeof stages[0]; ++stage)
                if (sg.supportedStages & stages[stage]) maxwell_vote_stages |= 1u << stage;
        }
        LOG("[vk] subgroups: size %u, stages %#x, operations %#x, Maxwell vote stages %#x\n",
            sg.subgroupSize, sg.supportedStages, sg.supportedOperations, maxwell_vote_stages);
        maxwell_h2f_bits = g_vk.props.vendorID == 0x10DE;  // (NVIDIA)
        if (!maxwell_frag_subgroups) LOG("[vk] no subgroup shuffles in fragment shaders: quad shuffles emulated\n");
    }
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq, sizeof *qp);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, qp);
    g_vk.qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.phys, i, g_vk.surface, &present);
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && present) {
            g_vk.qfam = i, g_vk.ts_bits = qp[i].timestampValidBits;
            break;
        }
    }
    free(qp);
    if (g_vk.qfam == UINT32_MAX) FATAL("no graphics+present queue\n");
    LOG("[vk] using %s\n", g_vk.props.deviceName);
    settings_resolve_defaults(g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU);
    // On by default on integrated GPUs only: lean usage and the SMAA stencil measured faster on an Intel UHD 770 and
    // slightly slower on an RTX 3080 Ti; skipping the per-frame target clears saved 3.7 ms on the UHD 770 (2x) with
    // identical images. LARECOMP_VK_LEAN_USAGE / LARECOMP_VK_SMAA_STENCIL / LARECOMP_NO_FRAME_CLEAR =0 / =1 override.
    bool igpu = g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
#ifdef __ANDROID__
    igpu = false;  // (phones: measured on desktop integrated GPUs only; their own format lists: textures.c vk_fmt_list)
#endif
    const char *e = getenv("LARECOMP_VK_LEAN_USAGE");
    g_vk.lean_usage = e && *e ? atoi(e) != 0 : igpu;
    e = getenv("LARECOMP_VK_SMAA_STENCIL");
    g_vk.smaa_stencil = e && *e ? atoi(e) != 0 : igpu;
    e = getenv("LARECOMP_NO_FRAME_CLEAR");
    g_vk.no_frame_clear = e && *e ? atoi(e) != 0 : igpu;
    LOG("[vk] lean usage %s, SMAA stencil %s, per-frame target clears %s (%s GPU)\n", g_vk.lean_usage ? "on" : "off",
        g_vk.smaa_stencil ? "on" : "off", g_vk.no_frame_clear ? "off" : "on", igpu ? "integrated" : "not an integrated");
}

static bool g_fault_ext, g_budget_ext;
// After VK_ERROR_DEVICE_LOST: what the driver says faulted (VK_EXT_device_fault), into the log.
void vk_device_fault_report(void) {
    static int once;
    if (once++ || !g_fault_ext) return;
    PFN_vkGetDeviceFaultInfoEXT get = (PFN_vkGetDeviceFaultInfoEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetDeviceFaultInfoEXT");
    if (!get) return;
    VkDeviceFaultCountsEXT c = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (get(g_vk.dev, &c, NULL) != VK_SUCCESS) { LOG("[vk] device fault: no info\n"); return; }
    VkDeviceFaultAddressInfoEXT addr[16];
    VkDeviceFaultVendorInfoEXT vend[16];
    if (c.addressInfoCount > 16) c.addressInfoCount = 16;
    if (c.vendorInfoCount > 16) c.vendorInfoCount = 16;
    c.vendorBinarySize = 0;
    VkDeviceFaultInfoEXT info = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT, .pAddressInfos = addr, .pVendorInfos = vend};
    if (get(g_vk.dev, &c, &info) != VK_SUCCESS && 0) return;
    LOG("[vk] device fault: %s\n", info.description);
    static const char *const kind[] = {"none", "read invalid", "write invalid", "execute invalid", "ip unknown", "ip invalid", "ip valid"};
    for (uint32_t i = 0; i < c.addressInfoCount; i++)
        LOG("[vk]   address %s %#llx (precision %#llx)\n", addr[i].addressType < 7 ? kind[addr[i].addressType] : "?",
            (unsigned long long)addr[i].reportedAddress, (unsigned long long)addr[i].addressPrecision);
    for (uint32_t i = 0; i < c.vendorInfoCount; i++)
        LOG("[vk]   vendor: %s (code %#llx data %#llx)\n", vend[i].description, (unsigned long long)vend[i].vendorFaultCode,
            (unsigned long long)vend[i].vendorFaultData);
}

static void core_features(VkPhysicalDeviceFeatures *b, const VkPhysicalDeviceFeatures *h) {
    b->independentBlend = h->independentBlend;
    b->dualSrcBlend = h->dualSrcBlend;
    b->fillModeNonSolid = h->fillModeNonSolid;
    b->samplerAnisotropy = h->samplerAnisotropy;
    b->textureCompressionBC = h->textureCompressionBC;
    b->shaderInt64 = h->shaderInt64;
    b->shaderStorageImageWriteWithoutFormat = h->shaderStorageImageWriteWithoutFormat;
    maxwell_storage_image_write = b->shaderStorageImageWriteWithoutFormat != VK_FALSE;
    b->depthBiasClamp = h->depthBiasClamp;
    b->depthClamp = h->depthClamp;
    b->largePoints = h->largePoints;
    b->wideLines = h->wideLines;
    b->imageCubeArray = h->imageCubeArray;
    b->sampleRateShading = h->sampleRateShading;
    b->fragmentStoresAndAtomics = h->fragmentStoresAndAtomics;
    b->vertexPipelineStoresAndAtomics = h->vertexPipelineStoresAndAtomics;
    b->robustBufferAccess = h->robustBufferAccess;  // guest shaders index cbufs freely: keep OOB reads harmless
    maxwell_robust_cbuf = b->robustBufferAccess;      // (then translate.c leaves out its own index clamp)
    b->shaderClipDistance = h->shaderClipDistance;
    maxwell_clip_distance = b->shaderClipDistance != VK_FALSE;
    if (!h->shaderInt64) LOG("[vk] warning: no shaderInt64 (64-bit address math in shaders)\n");
    if (!h->textureCompressionBC) LOG("[vk] no BC texture formats: decoded on upload\n");
    LOG("[vk] formatless storage image writes: %s\n", maxwell_storage_image_write ? "enabled" : "unavailable");
}

static void device_created(void);
#ifdef VK_NO_PROTOTYPES
// A device below 1.3 (COMPAT_EXT): the same features through the extensions' own structures, each enabled as far as
// the driver has it (what it lacks is logged: some only matter to some shaders).
static void create_device_compat(const char **ext, uint32_t n_ext, void *ext_features) {
    for (size_t i = 0; i < sizeof COMPAT_EXT / sizeof *COMPAT_EXT; i++)
        if (g_vk.api < COMPAT_EXT[i].below) ext[n_ext++] = COMPAT_EXT[i].name;
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dr = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR,
                                                      .pNext = ext_features};
    VkPhysicalDeviceSynchronization2FeaturesKHR s2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR,
                                                      .pNext = &dr};
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT, .pNext = &s2};
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT eds2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT, .pNext = &eds};
    VkPhysicalDeviceShaderDemoteToHelperInvocationFeaturesEXT dem = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES_EXT, .pNext = &eds2};
    VkPhysicalDeviceShaderTerminateInvocationFeaturesKHR term = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES_KHR, .pNext = &dem};
    // 1.2: its feature structure; 1.1: the extensions'
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &term};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12};
    VkPhysicalDeviceTimelineSemaphoreFeatures tl = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
                                                    .pNext = &term};
    VkPhysicalDeviceShaderFloat16Int8Features i8 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES,
                                                    .pNext = &tl};
    VkPhysicalDevice8BitStorageFeatures b8 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, .pNext = &i8};
    VkPhysicalDeviceScalarBlockLayoutFeatures sbl = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES,
                                                     .pNext = &b8};
    VkPhysicalDevice16BitStorageFeatures b16 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, .pNext = &sbl};
    bool v12 = g_vk.api >= VK_API_VERSION_1_2;
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = v12 ? (void *)&f11 : (void *)&b16};
    // what the driver has: everything wanted is enabled as far as it goes (the extensions' own features below
    // ext_features are asked for as they are, as on 1.3: hide them from the query)
    dr.pNext = NULL;
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &f);
    dr.pNext = ext_features;
    VkPhysicalDeviceFeatures have = f.features;
    memset(&f.features, 0, sizeof f.features);
    core_features(&f.features, &have);
#define WANT(cond, what) do { if (!(cond)) LOG("[vk] driver lacks %s\n", what); } while (0)
    WANT(dr.dynamicRendering, "dynamicRendering");
    WANT(s2.synchronization2, "synchronization2");
    WANT(eds.extendedDynamicState, "extendedDynamicState");
    WANT(eds2.extendedDynamicState2, "extendedDynamicState2");
    WANT(dem.shaderDemoteToHelperInvocation, "shaderDemoteToHelperInvocation");
    WANT(term.shaderTerminateInvocation, "shaderTerminateInvocation");
    eds2.extendedDynamicState2LogicOp = eds2.extendedDynamicState2PatchControlPoints = VK_FALSE;
    if (v12) {
        WANT(f12.timelineSemaphore, "timelineSemaphore");
        WANT(f12.shaderInt8, "shaderInt8");
        WANT(f12.storageBuffer8BitAccess, "storageBuffer8BitAccess");
        WANT(f12.scalarBlockLayout, "scalarBlockLayout");
        VkPhysicalDeviceVulkan12Features want12 = {.timelineSemaphore = f12.timelineSemaphore, .shaderInt8 = f12.shaderInt8,
            .samplerMirrorClampToEdge = f12.samplerMirrorClampToEdge,
            .uniformAndStorageBuffer8BitAccess = f12.uniformAndStorageBuffer8BitAccess,
            .storageBuffer8BitAccess = f12.storageBuffer8BitAccess, .scalarBlockLayout = f12.scalarBlockLayout};
        want12.sType = f12.sType, want12.pNext = f12.pNext;
        f12 = want12;
        VkPhysicalDeviceVulkan11Features want11 = {.sType = f11.sType, .pNext = f11.pNext,
                                                   .storageBuffer16BitAccess = f11.storageBuffer16BitAccess};
        f11 = want11;
    } else {
        WANT(tl.timelineSemaphore, "timelineSemaphore");
        WANT(i8.shaderInt8, "shaderInt8");
        WANT(b8.storageBuffer8BitAccess, "storageBuffer8BitAccess");
        WANT(sbl.scalarBlockLayout, "scalarBlockLayout");
        i8.shaderFloat16 = VK_FALSE;
        b8.storagePushConstant8 = VK_FALSE;
        b16.uniformAndStorageBuffer16BitAccess = b16.storagePushConstant16 = b16.storageInputOutput16 = VK_FALSE;
    }
#undef WANT
    if (!(tl.timelineSemaphore || (v12 && f12.timelineSemaphore)) || !dr.dynamicRendering || !s2.synchronization2 ||
        !eds.extendedDynamicState || !eds2.extendedDynamicState2)
        FATAL("this GPU driver lacks Vulkan features the game needs (see the lines above)\n");
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_vk.qfam,
                                  .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi, .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext};
    LOG("[vk] Vulkan %u.%u device with %u extensions for 1.3\n", VK_API_VERSION_MAJOR(g_vk.api), VK_API_VERSION_MINOR(g_vk.api), n_ext);
    VK_CHECK(vkCreateDevice(g_vk.phys, &di, NULL, &g_vk.dev));
    device_created();
}
#endif

// Each memory heap: its size, the driver's budget for this process (smaller when other programs hold that memory),
// this process's use and the part LAS Recomp allocated through VMA. Logged at start and when an allocation fails
// (issue #45: out of device memory on a 6 GB GTX 1060 - our own use, or other programs'?).
void vk_log_memory(const char *why) {
    if (!g_vk.vma) return;
    const VkPhysicalDeviceMemoryProperties *mp;
    vmaGetMemoryProperties(g_vk.vma, &mp);
    VmaBudget b[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(g_vk.vma, b);
    for (uint32_t i = 0; i < mp->memoryHeapCount; i++)
        LOG("[vk] memory (%s) heap %u %s: %llu MB, budget %llu MB, in use %llu MB, ours %llu MB in %u allocations\n", why, i,
            mp->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? "device-local" : "system",
            (unsigned long long)(mp->memoryHeaps[i].size >> 20), (unsigned long long)(b[i].budget >> 20),
            (unsigned long long)(b[i].usage >> 20), (unsigned long long)(b[i].statistics.allocationBytes >> 20),
            b[i].statistics.allocationCount);
}

// ---- pipeline cache file
#define PCACHE_FILE "cache/vk_pipelines.bin"
#define PCACHE_TAG "cache/vk_pipelines.bin.tag"
static size_t pcache_saved;  // bytes of the last save (or load)
void vk_pipeline_cache_save(void);

static bool pipeline_cache_device(SharedGpuDevice *device) {
    memset(device, 0, sizeof *device);
    device->vendor = g_vk.props.vendorID; device->device = g_vk.props.deviceID;
    device->driver = g_vk.props.driverVersion; device->api = g_vk.api;
    memcpy(device->uuid, g_vk.props.pipelineCacheUUID, 16);
    return shared_gpu_environment(device->environment);
}
static void pipeline_cache_legacy_gpu(char *out, size_t n) {
    char uuid[2 * VK_UUID_SIZE + 1];
    for (int i = 0; i < VK_UUID_SIZE; i++) snprintf(uuid + 2 * i, 3, "%02x", g_vk.props.pipelineCacheUUID[i]);
    snprintf(out, n, "%s, vendor %x device %x, driver %u, cache %s", g_vk.props.deviceName, g_vk.props.vendorID,
             g_vk.props.deviceID, g_vk.props.driverVersion, uuid);
}

static void *pipeline_cache_saver(void *a) {  // every 3 minutes, when it grew
    (void)a;
    for (;;) {
        SDL_Delay(180000);
        if (rq_stopping()) break;  // (quitting: backend_quit saves it)
        vk_pipeline_cache_save();
    }
    return NULL;
}

static void create_device(void) {
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, ep);
    const char *ext[32];
    uint32_t n_ext = 0;
    ext[n_ext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    g_vk.custom_border = has_ext(ep, ne, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    if (g_vk.custom_border) ext[n_ext++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
    g_vk.divisor = has_ext(ep, ne, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    if (g_vk.divisor) ext[n_ext++] = VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
    g_vk.host_import = has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);  // guest heap as a GPU buffer (exec.c)
#ifdef __ANDROID__
    // Phones: the import pins all of the 3 GB heap. Samsung's Xclipse driver took it and then failed the first
    // submission (VK_ERROR_UNKNOWN, Galaxy S26); Mali drivers have the extension too. The mirrored storage buffers
    // Turnip uses run everywhere. LARECOMP_VK_HOST_IMPORT=1: the import where the driver offers it.
    if (g_vk.host_import) {
        const char *hi = getenv("LARECOMP_VK_HOST_IMPORT");
        g_vk.host_import = hi && atoi(hi);
    }
#endif
    if (g_vk.host_import) ext[n_ext++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    maxwell_gmem_mirror = !g_vk.host_import;  // (exec.c mirror_table)
    {   // (shader.h) LARECOMP_VK_GMEM_ZERO=0 / LARECOMP_VK_GFIND_CACHE=0: the loads as before
        const char *z = getenv("LARECOMP_VK_GMEM_ZERO"), *gc = getenv("LARECOMP_VK_GFIND_CACHE");
        maxwell_gmem_zero = maxwell_gmem_mirror && (!z || atoi(z));
        maxwell_gfind_cache = maxwell_gmem_mirror && (!gc || atoi(gc));
    }
    bool fault = has_ext(ep, ne, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);  // device-lost diagnostics (vk_device_fault_report)
    if (fault) ext[n_ext++] = VK_EXT_DEVICE_FAULT_EXTENSION_NAME;
    g_fault_ext = fault;
    g_budget_ext = has_ext(ep, ne, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);  // (real per-heap budgets: vk_log_memory)
    if (g_budget_ext) ext[n_ext++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
    // LARECOMP_VK_SHADER_STATS=1: the driver's statistics of each pipeline's shaders (registers, instructions, spills)
    g_vk_shader_stats = getenv("LARECOMP_VK_SHADER_STATS") && has_ext(ep, ne, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
    if (g_vk_shader_stats) ext[n_ext++] = VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME;
    // pipeline-building features: logged below (issue #92); dynamic vertex input used, the library one only measured
    bool vids_ext = has_ext(ep, ne, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
#ifdef VK_EXT_graphics_pipeline_library
    bool gpl_fast = false;
    bool gpl_ext = has_ext(ep, ne, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) &&
                   has_ext(ep, ne, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
#endif
#ifdef VK_EXT_extended_dynamic_state3
    bool eds3_ext = has_ext(ep, ne, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
#endif
#if defined(__linux__) && !defined(__ANDROID__)  // (not phones: their guest heap stays their own memory, android.c)
    // the guest heap as driver memory mapped at HEAP_BASE (exec.c backend_guest_heap_prepare)
    g_vk.dmabuf = has_ext(ep, ne, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) &&
                  has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    if (g_vk.dmabuf) {
        ext[n_ext++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
        ext[n_ext++] = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
    }
    // ... mapped there by the driver itself (its own DRM file, as every vkMapMemory), not through the dma-buf (issue #92)
    // (older Vulkan headers, e.g. a distro's, lack it: then the dma-buf mapping only)
#ifdef VK_EXT_map_memory_placed
    bool placed_ext = has_ext(ep, ne, VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME) && has_ext(ep, ne, VK_KHR_MAP_MEMORY_2_EXTENSION_NAME);
#endif
#endif
    free(ep);

    // features the backend relies on (all present on current desktop drivers)
    VkPhysicalDeviceVulkan12Features have12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &have12};
#if defined(__linux__) && !defined(__ANDROID__) && defined(VK_EXT_map_memory_placed)
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT have_placed = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT};
    if (placed_ext) have12.pNext = &have_placed;
#endif
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT have_vids = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
    if (vids_ext) have_vids.pNext = have.pNext, have.pNext = &have_vids;
#ifdef VK_EXT_graphics_pipeline_library
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT have_gpl = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    if (gpl_ext) have_gpl.pNext = have.pNext, have.pNext = &have_gpl;
#endif
#ifdef VK_EXT_extended_dynamic_state3
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT have_eds3 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    if (eds3_ext) have_eds3.pNext = have.pNext, have.pNext = &have_eds3;
#endif
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &have);
    maxwell_shader_int64 = have.features.shaderInt64 != VK_FALSE;
    // Without shaderInt64, use low/high guest-address arithmetic and uint heap
    // offsets in the existing mirror. Imported heap/chunk addresses require the
    // advertised64-bit feature; the bounded64MiB mirror does not.
    if (!maxwell_shader_int64) g_vk.host_import = false;
    maxwell_gmem_mirror = !g_vk.host_import;
    maxwell_gmem_readonly = maxwell_gmem_mirror && g_vk.props.limits.maxStorageBufferRange < (64ull << 20);
    {
        const char *z = getenv("LARECOMP_VK_GMEM_ZERO"), *gc = getenv("LARECOMP_VK_GFIND_CACHE");
        maxwell_gmem_zero = maxwell_gmem_mirror && (!z || atoi(z));
        maxwell_gfind_cache = maxwell_gmem_mirror && (!gc || atoi(gc));
    }
    if (maxwell_gmem_readonly) {
        // Capability selection applies to every title. The writable mirror
        // needs64MiB descriptors; smaller limits retain the read-only fallback.
        LOG("[vk] shader global memory: read-only bound-range snapshots (storage range below64MiB)\n");
    } else if (!maxwell_shader_int64)
        LOG("[vk] shader global memory: uint32 guest addresses with writable64MiB storage-buffer mirror\n");
    {   // what the driver offers for building pipelines (one line, for #92 reports)
        VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 pr = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv};
        char gpl[96] = "no extension", eds3[160] = "no extension";
#ifdef VK_EXT_graphics_pipeline_library
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gplp = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
        if (gpl_ext) gplp.pNext = pr.pNext, pr.pNext = &gplp;
#else
        snprintf(gpl, sizeof gpl, "not in this build's headers");
#endif
#ifndef VK_EXT_extended_dynamic_state3
        snprintf(eds3, sizeof eds3, "not in this build's headers");
#endif
        vkGetPhysicalDeviceProperties2(g_vk.phys, &pr);
#ifdef VK_EXT_graphics_pipeline_library
        gpl_fast = gpl_ext && gplp.graphicsPipelineLibraryFastLinking;
        if (gpl_ext)
            snprintf(gpl, sizeof gpl, "%s (fast linking %s, independent interpolation decoration %s)",
                     have_gpl.graphicsPipelineLibrary ? "yes" : "no", gplp.graphicsPipelineLibraryFastLinking ? "yes" : "no",
                     gplp.graphicsPipelineLibraryIndependentInterpolationDecoration ? "yes" : "no");
#endif
#ifdef VK_EXT_extended_dynamic_state3
        if (eds3_ext)
            snprintf(eds3, sizeof eds3, "polygon mode %d, samples %d, alpha to coverage %d, blend enable %d, "
                     "blend equation %d, write mask %d", have_eds3.extendedDynamicState3PolygonMode,
                     have_eds3.extendedDynamicState3RasterizationSamples, have_eds3.extendedDynamicState3AlphaToCoverageEnable,
                     have_eds3.extendedDynamicState3ColorBlendEnable, have_eds3.extendedDynamicState3ColorBlendEquation,
                     have_eds3.extendedDynamicState3ColorWriteMask);
#endif
        LOG("[vk] driver %s %s; graphics pipeline library: %s; vertex input dynamic state: %s; extended dynamic state 3: %s\n",
            drv.driverName, drv.driverInfo, gpl, vids_ext ? (have_vids.vertexInputDynamicState ? "yes" : "no") : "no extension",
            eds3);
    }
    {   // dynamic vertex input (exec.c): fewer pipelines; LARECOMP_VK_DYN_VERTEX=0 keeps the layout in each pipeline
        const char *e = getenv("LARECOMP_VK_DYN_VERTEX");
        bool off = e && !atoi(e);
        g_vk.vids = vids_ext && have_vids.vertexInputDynamicState && !off;
        if (g_vk.vids) ext[n_ext++] = VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME;
        else LOG("[vk] vertex input: in each pipeline (%s)\n", off ? "LARECOMP_VK_DYN_VERTEX=0" :
                 !vids_ext ? "no VK_EXT_vertex_input_dynamic_state" : "vertexInputDynamicState not supported");
    }
#ifdef VK_EXT_graphics_pipeline_library
    {   // Default off: require an explicit fast-linking guarantee for the production path.
        const char *run = getenv("LARECOMP_VK_GPL"), *probe = getenv("LARECOMP_VK_GPL_PROBE");
        bool requested = run && atoi(run);
        g_vk.gpl = requested && gpl_ext && have_gpl.graphicsPipelineLibrary && gpl_fast;
        g_vk.gpl_probe = probe && atoi(probe) && gpl_ext && have_gpl.graphicsPipelineLibrary;
        if (g_vk.gpl || g_vk.gpl_probe) {
            ext[n_ext++] = VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME;
            ext[n_ext++] = VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME;
        }
        if (requested) LOG("[gpl] reusable libraries: %s\n", g_vk.gpl ? "enabled (fast linking)" :
                           "unavailable: feature or fast-linking guarantee missing; monolithic fallback");
        if (probe && atoi(probe) && !g_vk.gpl_probe) LOG("[gpl] probe: the driver has no graphics pipeline library; skipped\n");
    }
#else
    if (getenv("LARECOMP_VK_GPL") && atoi(getenv("LARECOMP_VK_GPL"))) LOG("[gpl] reusable libraries: unavailable in build headers; monolithic fallback\n");
    if (getenv("LARECOMP_VK_GPL_PROBE") && atoi(getenv("LARECOMP_VK_GPL_PROBE"))) LOG("[gpl] probe: this build's Vulkan headers have no graphics pipeline library; skipped\n");
#endif
#if defined(__linux__) && !defined(__ANDROID__) && defined(VK_EXT_map_memory_placed)
    have12.pNext = NULL;
    g_vk.map_placed = placed_ext && have_placed.memoryMapPlaced;
    g_vk.unmap_reserve = g_vk.map_placed && have_placed.memoryUnmapReserve;
    if (g_vk.map_placed) {
        ext[n_ext++] = VK_KHR_MAP_MEMORY_2_EXTENSION_NAME;
        ext[n_ext++] = VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME;
    }
#endif
    bool probe;
    g_vk.lazy_heap = lazy_heap_predicted(g_budget_ext, &probe);
    g_vk.heap_probe = !g_vk.lazy_heap && probe && g_vk.host_import && have12.bufferDeviceAddress;
    g_vk.bda = (g_vk.lazy_heap || g_vk.heap_probe) && have12.bufferDeviceAddress;
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT div = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT,
        .vertexAttributeInstanceRateDivisor = VK_TRUE, .vertexAttributeInstanceRateZeroDivisor = VK_FALSE};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, .customBorderColors = VK_TRUE,
        .customBorderColorWithoutFormat = VK_TRUE, .pNext = g_vk.divisor ? &div : NULL};
#ifdef VK_NO_PROTOTYPES
    if (g_vk.api < VK_API_VERSION_1_3) {
        VkPhysicalDeviceFaultFeaturesEXT ffc = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT,
                                                .deviceFault = VK_TRUE,
                                                .pNext = g_vk.custom_border ? (void *)&cbc : g_vk.divisor ? (void *)&div : NULL};
        if (g_vk.vids) {  // (not enabled through these drivers' feature structs: the layout stays in each pipeline)
            g_vk.vids = false;
            for (uint32_t i = 0; i < n_ext; i++)
                if (!strcmp(ext[i], VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME)) ext[i--] = ext[--n_ext];
            LOG("[vk] vertex input: in each pipeline (Vulkan 1.1 / 1.2 driver)\n");
        }
        create_device_compat(ext, n_ext, fault ? (void *)&ffc : ffc.pNext);
        return;
    }
#endif
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE,
                                            .shaderDemoteToHelperInvocation = VK_TRUE, .shaderTerminateInvocation = VK_TRUE,
                                            .maintenance4 = VK_TRUE,
                                            .pNext = g_vk.custom_border ? (void *)&cbc : g_vk.divisor ? (void *)&div : NULL};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .timelineSemaphore = VK_TRUE, .shaderInt8 = VK_TRUE, .samplerMirrorClampToEdge = VK_TRUE,
                                            .uniformAndStorageBuffer8BitAccess = VK_TRUE, .storageBuffer8BitAccess = VK_TRUE,
                                            .scalarBlockLayout = VK_TRUE, .bufferDeviceAddress = g_vk.bda, .pNext = &f13};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                            .storageBuffer16BitAccess = VK_TRUE, .pNext = &f12};
    VkPhysicalDeviceFaultFeaturesEXT ff = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, .deviceFault = VK_TRUE,
                                           .pNext = &f11};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = fault ? (void *)&ff : (void *)&f11};
#if defined(__linux__) && !defined(__ANDROID__) && defined(VK_EXT_map_memory_placed)
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT placed = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT,
                                                         .memoryMapPlaced = VK_TRUE, .memoryUnmapReserve = g_vk.unmap_reserve,
                                                         .pNext = f.pNext};
    if (g_vk.map_placed) f.pNext = &placed;
#endif
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT en_vids = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT, .vertexInputDynamicState = VK_TRUE};
    if (g_vk.vids) en_vids.pNext = f.pNext, f.pNext = &en_vids;
#ifdef VK_EXT_graphics_pipeline_library
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT en_gpl = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, .graphicsPipelineLibrary = VK_TRUE};
    if (g_vk.gpl || g_vk.gpl_probe) en_gpl.pNext = f.pNext, f.pNext = &en_gpl;
#endif
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pef = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR, .pipelineExecutableInfo = VK_TRUE,
        .pNext = f.pNext};
    if (g_vk_shader_stats) f.pNext = &pef;
    core_features(&f.features, &have.features);
    g_vk.ssbo_chunks = have.features.fragmentStoresAndAtomics && have.features.vertexPipelineStoresAndAtomics &&
                       have.features.robustBufferAccess;
#ifdef VK_NO_PROTOTYPES
    {   // phones: one feature the driver lacks fails the whole device - enable what it has, log what it lacks
        VkPhysicalDeviceVulkan13Features h13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan12Features h12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &h13};
        VkPhysicalDeviceVulkan11Features h11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &h12};
        VkPhysicalDeviceFeatures2 hf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &h11};
        vkGetPhysicalDeviceFeatures2(g_vk.phys, &hf);
#define HAVE(w, h, x) if (w.x && !h.x) { LOG("[vk] driver lacks %s\n", #x); w.x = VK_FALSE; }
        if (!h13.dynamicRendering || !h13.synchronization2 || !h12.timelineSemaphore)
            FATAL("this GPU driver lacks Vulkan features the game needs (dynamic rendering, synchronization2, timeline "
                  "semaphores)\n");
        HAVE(f13, h13, shaderDemoteToHelperInvocation)
        HAVE(f13, h13, shaderTerminateInvocation) HAVE(f13, h13, maintenance4) HAVE(f12, h12, timelineSemaphore)
        HAVE(f12, h12, shaderInt8) HAVE(f12, h12, samplerMirrorClampToEdge) HAVE(f12, h12, uniformAndStorageBuffer8BitAccess)
        HAVE(f12, h12, storageBuffer8BitAccess) HAVE(f12, h12, scalarBlockLayout) HAVE(f11, h11, storageBuffer16BitAccess)
#undef HAVE
        // guest shaders fetch texels and images with coordinates nobody checked: out of the image, a phone GPU may
        // fault (device lost) where desktop ones just return zero
        f13.robustImageAccess = h13.robustImageAccess;
#ifdef __ANDROID__
        {   // native fp16 for the guest's half-precision instructions (translate.c maxwell_half_native)
            const char *e = getenv("LARECOMP_HALF_NATIVE");
            if (h12.shaderFloat16 && (!e || atoi(e))) f12.shaderFloat16 = VK_TRUE, maxwell_half_native = 1;
            const char *sc = getenv("LARECOMP_VK_SCALAR_CBUF");  // run-time indexed cbufs as words (translate.c)
            maxwell_scalar_cbuf = h12.scalarBlockLayout && (!sc || atoi(sc));
        }
#endif
        if (!h13.robustImageAccess) LOG("[vk] driver lacks robustImageAccess\n");
    }
#endif
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_vk.qfam,
                                  .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi, .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext};
    VK_CHECK(vkCreateDevice(g_vk.phys, &di, NULL, &g_vk.dev));
    device_created();
}

// Do this GPU's min / max keep the non-NaN operand, and its clamp make NaN 0 - what Maxwell's FMNMX and .SAT do? Then
// the guest shaders' saturate / fmin / fmax go without the isnan selects around them (maxwell_ieee_minmax; shader.h,
// translate.c). A one-thread compute dispatch over a NaN read from a buffer (nothing the compiler can fold), its
// result words compared with the Switch's. LARECOMP_VK_IEEE_MINMAX=0/1 decides instead of the probe.
static void probe_ieee_minmax(void) {
    const char *e = getenv("LARECOMP_VK_IEEE_MINMAX");
    if (e) { maxwell_ieee_minmax = atoi(e) != 0; return; }
    char src[1200];
    snprintf(src, sizeof src,
             "#version 460\n%s"
             "layout(local_size_x = 1) in;\n"
             "layout(std430, set = 0, binding = 0) buffer B { uint w[12]; };\n"
             "void main() { float n = uintBitsToFloat(w[0]), one = uintBitsToFloat(w[1]);\n"
             "  w[2] = floatBitsToUint(max(n, one)); w[3] = floatBitsToUint(min(n, one));\n"
             "  w[4] = floatBitsToUint(max(one, n)); w[5] = floatBitsToUint(min(one, n));\n"
             "  w[6] = floatBitsToUint(clamp(n, 0.0f, 1.0f)); w[7] = floatBitsToUint(clamp(fma(n, one, one), 0.0f, 1.0f));\n"
             "  w[8] = floatBitsToUint(clamp(n * one, 0.0f, 1.0f));%s }\n",
             maxwell_half_native ? "#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require\n" : "",
             maxwell_half_native ? " w[9] = packFloat2x16(clamp(f16vec2(float16_t(n), float16_t(one) + float16_t(n)), f16vec2(0.0hf), f16vec2(1.0hf)));"
                                 : "");
    VkShaderModule sm = vk_compile_glsl(src, STAGE_COMPUTE, "ieee_minmax.comp");
    if (!sm) { LOG("[vk] NaN probe: no shader (isnan selects kept)\n"); return; }
    VkDescriptorSetLayout dl = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkPipeline p = VK_NULL_HANDLE;
    VkDescriptorPool dp = VK_NULL_HANDLE;
    VkDescriptorSet ds = VK_NULL_HANDLE;
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation mem = NULL;
    VmaAllocationInfo info = {0};
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkDescriptorSetLayoutBinding b = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
                                      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
    bool ok = vkCreateDescriptorSetLayout(g_vk.dev, &dli, NULL, &dl) == VK_SUCCESS;
    VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dl};
    ok = ok && vkCreatePipelineLayout(g_vk.dev, &pli, NULL, &pl) == VK_SUCCESS;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main"}};
    ok = ok && vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &ci, NULL, &p) == VK_SUCCESS;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 64, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    ok = ok && vmaCreateBuffer(g_vk.vma, &bi, &ai, &buf, &mem, &info) == VK_SUCCESS && info.pMappedData;
    VkDescriptorPoolSize ps = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
    ok = ok && vkCreateDescriptorPool(g_vk.dev, &dpi, NULL, &dp) == VK_SUCCESS;
    VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp,
                                       .descriptorSetCount = 1, .pSetLayouts = &dl};
    ok = ok && vkAllocateDescriptorSets(g_vk.dev, &dai, &ds) == VK_SUCCESS;
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_vk.pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    ok = ok && vkAllocateCommandBuffers(g_vk.dev, &cai, &cb) == VK_SUCCESS;
    uint32_t *w = info.pMappedData;
    if (ok) {
        memset(w, 0xEE, 64);
        w[0] = 0x7fc00000u, w[1] = 0x3f800000u;  // NaN, 1.0
        vmaFlushAllocation(g_vk.vma, mem, 0, 64);
        VkDescriptorBufferInfo dbi = {.buffer = buf, .range = 64};
        VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi};
        vkUpdateDescriptorSets(g_vk.dev, 1, &wr, 0, NULL);
        VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        ok = vkBeginCommandBuffer(cb, &cbi) == VK_SUCCESS;
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
        vkCmdDispatch(cb, 1, 1, 1);
        ok = ok && vkEndCommandBuffer(cb) == VK_SUCCESS;
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
        ok = ok && vkQueueSubmit(g_vk.queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS && vkQueueWaitIdle(g_vk.queue) == VK_SUCCESS;
        vmaInvalidateAllocation(g_vk.vma, mem, 0, 64);
    }
    if (ok) {
        bool good = w[2] == 0x3f800000u && w[3] == 0x3f800000u && w[4] == 0x3f800000u && w[5] == 0x3f800000u && !w[6] && !w[7] &&
                    !w[8] && (!maxwell_half_native || !w[9]);
        maxwell_ieee_minmax = good;
        LOG("[vk] NaN through max min max min clamp clamp(fma) clamp(mul) clamp16: %08x %08x %08x %08x %08x %08x %08x %08x: %s\n",
            w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], good ? "as Maxwell: plain saturate / fmin / fmax" : "not Maxwell's: isnan selects kept");
    } else {
        LOG("[vk] NaN probe failed (isnan selects kept)\n");
    }
    if (cb) vkFreeCommandBuffers(g_vk.dev, g_vk.pool, 1, &cb);
    if (dp) vkDestroyDescriptorPool(g_vk.dev, dp, NULL);
    if (buf) vmaDestroyBuffer(g_vk.vma, buf, mem);
    if (p) vkDestroyPipeline(g_vk.dev, p, NULL);
    if (pl) vkDestroyPipelineLayout(g_vk.dev, pl, NULL);
    if (dl) vkDestroyDescriptorSetLayout(g_vk.dev, dl, NULL);
    vkDestroyShaderModule(g_vk.dev, sm, NULL);
}

// What follows the device's creation, whichever way it was made
static void device_created(void) {
#ifdef VK_NO_PROTOTYPES
    volkLoadDevice(g_vk.dev);
    compat_aliases();
#endif
    vkGetDeviceQueue(g_vk.dev, g_vk.qfam, 0, &g_vk.queue);
    if (g_vk.vids) {
        g_vk.set_vertex_input = (PFN_vkCmdSetVertexInputEXT)vkGetDeviceProcAddr(g_vk.dev, "vkCmdSetVertexInputEXT");
        if (!g_vk.set_vertex_input) g_vk.vids = false;
        LOG("[vk] vertex input: %s\n", g_vk.vids ? "dynamic (VK_EXT_vertex_input_dynamic_state)" :
            "in each pipeline (vkCmdSetVertexInputEXT missing)");
    }

    VmaAllocatorCreateInfo ai = {.physicalDevice = g_vk.phys, .device = g_vk.dev, .instance = g_vk.inst,
                                 .vulkanApiVersion = g_vk.api,
                                 .flags = g_budget_ext ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0};
#ifdef VK_NO_PROTOTYPES
    VmaVulkanFunctions vf = {.vkGetInstanceProcAddr = vkGetInstanceProcAddr, .vkGetDeviceProcAddr = vkGetDeviceProcAddr};
    ai.pVulkanFunctions = &vf;
#endif
    VK_CHECK(vmaCreateAllocator(&ai, &g_vk.vma));
    vk_log_memory("start");

    VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &g_vk.timeline));
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_vk.qfam};
    VK_CHECK(vkCreateCommandPool(g_vk.dev, &pi, NULL, &g_vk.pool));
    probe_ieee_minmax();  // (before any guest shader is translated: vk_headstart_begin below)

    // Opaque driver data is keyed by the Vulkan header/device/driver, never app code.
    SharedGpuDevice cache_device;
    char legacy_gpu[400]; pipeline_cache_legacy_gpu(legacy_gpu, sizeof legacy_gpu);
    size_t sz = 0; bool migrated = false;
    void *data = pipeline_cache_device(&cache_device) ? shared_gpu_cache_load(
        PCACHE_FILE, PCACHE_TAG, &cache_device, legacy_gpu, &sz, &migrated) : NULL;
    if (!data && !access(PCACHE_FILE, F_OK)) LOG("[vk] pipeline cache incompatible or incomplete: starting a new one\n");
    VkPipelineCacheCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = sz,
                                    .pInitialData = data};
    if (vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache) != VK_SUCCESS) {
        // A driver-rejected blob must not be retried forever or blessed during
        // legacy migration. Cache data is disposable; preserve game/save files.
        remove(PCACHE_FILE); remove(PCACHE_TAG);
        migrated = false;
        pc.initialDataSize = 0; pc.pInitialData = NULL;
        VK_CHECK(vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache));
    }
    if (migrated) {
        SharedGpuCacheMeta metadata;
        if (shared_gpu_cache_metadata(&metadata, data, sz, &cache_device) &&
            shared_gpu_cache_tag_write(PCACHE_TAG, &metadata))
            LOG("[vk] pipeline cache: imported legacy device-compatible blob (sidecar upgraded)\n");
    }
    free(data);
    extern void vk_headstart_begin(void);
    vk_headstart_begin();  // (the shaders and pipelines seen before, built in the background from now: exec.c)
    if (pc.initialDataSize) LOG("[vk] pipeline cache: loaded %zu bytes\n", pc.initialDataSize);
    pcache_saved = pc.initialDataSize;
    pthread_t t;  // (kept every few minutes while it grows, not only at a clean exit)
    pthread_create(&t, NULL, pipeline_cache_saver, NULL);
    pthread_detach(t);
}

// Saved atomically (temp file + rename: a crash mid-write leaves the previous one), with its tag; one save at a time
void vk_pipeline_cache_save(void) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    SharedGpuDevice cache_device;
    if (!pipeline_cache_device(&cache_device)) { pthread_mutex_unlock(&lock); return; }
    size_t sz = 0;
    if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, NULL) == VK_SUCCESS && sz && sz != pcache_saved) {
#ifndef _WIN32
        // straight into the file through a shared mapping: the copy is file pages the system writes out and drops, not
        // a second in-memory copy of a cache of 100+ MB (phones: the spike every few minutes got the game closed)
        mkdir("cache", 0755);
        int fd = open(PCACHE_FILE ".tmp", O_RDWR | O_CREAT | O_TRUNC, 0644);
        void *m = fd >= 0 && ftruncate(fd, (off_t)sz) == 0 ? mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
        if (m != MAP_FAILED) {
            size_t got = sz;
            bool ok = vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &got, m) == VK_SUCCESS;
            SharedGpuCacheMeta metadata;
            ok = ok && got <= sz && shared_gpu_cache_metadata(&metadata, m, got, &cache_device);
            ok &= msync(m, sz, MS_SYNC) == 0;
            munmap(m, sz);
            ok &= ftruncate(fd, (off_t)got) == 0;
            ok &= close(fd) == 0;
            fd = -1;
            if (ok && rename(PCACHE_FILE ".tmp", PCACHE_FILE) == 0 &&
                shared_gpu_cache_tag_write(PCACHE_TAG, &metadata)) {
                pcache_saved = got;
                LOG("[vk] pipeline cache: saved %zu bytes\n", got);
            } else remove(PCACHE_FILE ".tmp");
            pthread_mutex_unlock(&lock);
            return;
        }
        if (fd >= 0) close(fd), remove(PCACHE_FILE ".tmp");
#endif
        void *data = malloc(sz);
        SharedGpuCacheMeta metadata;
        if (data && vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, data) == VK_SUCCESS &&
            shared_gpu_cache_metadata(&metadata, data, sz, &cache_device) &&
            shader_cache_write(PCACHE_FILE, data, sz) && shared_gpu_cache_tag_write(PCACHE_TAG, &metadata)) {
            pcache_saved = sz;
            LOG("[vk] pipeline cache: saved %zu bytes\n", sz);
        }
        free(data);
    }
    pthread_mutex_unlock(&lock);
}

// ================================================================== swapchain
static VkPresentModeKHR present_mode(void) {
    // MAILBOX by default: the game paces itself (vi.c's emulated 60 Hz vsync), and with FIFO every submission
    // queued behind the one waiting for a window image, so the game's fence waits followed the presentation engine:
    // a throttled / captured / busy compositor slowed the whole game (4-21 fps with the GPU mostly idle; mailbox:
    // 60). No tearing. LARECOMP_VK_PRESENT=fifo|immediate|mailbox to choose.
    // Frame generation (framegen.c): FIFO, which shows the in-between frame, presented right after the real one, a
    // refresh later.
    extern bool fg_enabled(void);
    const char *e = getenv("LARECOMP_VK_PRESENT");
    if (!e && fg_enabled()) e = "fifo";
    VkPresentModeKHR want = !e || !strcmp(e, "mailbox") ? VK_PRESENT_MODE_MAILBOX_KHR
                          : !strcmp(e, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, NULL);
    VkPresentModeKHR m[16];
    if (n > 16) n = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, m);
    for (uint32_t i = 0; i < n; i++) if (m[i] == want) return want;
    return VK_PRESENT_MODE_FIFO_KHR;
}

static bool g_sc_turned, g_fifo_present;  // (FIFO: a second picture per game frame shows a refresh later)
static atomic_bool g_background, g_surface_stale;  // (the app in the background: below)
static void create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    VkResult capr = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps);
    if (capr == VK_ERROR_SURFACE_LOST_KHR) { atomic_store(&g_surface_stale, true); return; }  // (Android: window gone)
    VK_CHECK(capr);
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    VkExtent2D ext = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : (VkExtent2D){(uint32_t)w, (uint32_t)h};
    VkSurfaceTransformFlagBitsKHR xf = caps.currentTransform;
#ifdef __ANDROID__
    // Phones are portrait underneath: in landscape the surface reports a 90 / 270 degree transform. The images are
    // made in the window's own (landscape) shape, the extent it reports, and the system turns them (identity)
    if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) xf = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    // Pre-rotation: the images in the panel's own (portrait) orientation with its transform, everything drawn into
    // them turned (vk_sc_rect, the present / in-between frame / UI vertex shaders). Otherwise SurfaceFlinger composites
    // the landscape layer with the GPU, turning the whole screen at every present (CLIENT composition): on the S20 FE
    // ~13% of the GPU's time with frame generation. LARECOMP_PRETRANSFORM=0: the system turns them.
    static int pre = -1;
    if (pre < 0) pre = getenv("LARECOMP_PRETRANSFORM") ? atoi(getenv("LARECOMP_PRETRANSFORM")) != 0 : 1;
    g_vk.sc_quarter = 0;
    if (pre && (caps.currentTransform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
                caps.currentTransform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)) {
        xf = caps.currentTransform;
        g_vk.sc_quarter = xf == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ? 1 : 3;
    }
#endif
    g_sc_turned = xf != caps.currentTransform;  // (then presents say "suboptimal": that's this, not a new size)
    if (!ext.width || !ext.height) return;  // minimised
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, NULL);
    VkSurfaceFormatKHR fmts[64];
    if (nf > 64) nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, fmts);
    // scanout shows the bytes the game wrote (see the raw views in the present pass): a UNORM swapchain like Metal's
    VkSurfaceFormatKHR sf = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sf = fmts[i]; break; }
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > 8) count = 8;
    VkSwapchainKHR old = g_vk.swapchain;
    VkExtent2D phys = g_vk.sc_quarter ? (VkExtent2D){ext.height, ext.width} : ext;
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_vk.surface, .minImageCount = count,
        .imageFormat = sf.format, .imageColorSpace = sf.colorSpace, .imageExtent = phys, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = xf,
        .compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                                                                          : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        .presentMode = present_mode(), .clipped = VK_TRUE,
        .oldSwapchain = old};
    VkResult cr = vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &g_vk.swapchain);
    g_fifo_present = ci.presentMode == VK_PRESENT_MODE_FIFO_KHR;
    if (cr == VK_ERROR_SURFACE_LOST_KHR || cr == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) {  // (Android: the app went away)
        LOG("[vk] swapchain: surface lost (%d)\n", cr);
        g_vk.swapchain = VK_NULL_HANDLE;
        if (old) vkDestroySwapchainKHR(g_vk.dev, old, NULL);
        for (uint32_t i = 0; i < g_vk.sc_count; i++) vkDestroyImageView(g_vk.dev, g_vk.sc_views[i], NULL);
        g_vk.sc_count = 0;
        atomic_store(&g_surface_stale, true);  // (a new surface is made on the next frame)
        return;
    }
    VK_CHECK(cr);
    static const char *const modes[] = {"immediate", "mailbox", "fifo", "fifo-relaxed"};
    LOG("[vk] swapchain %ux%u, %u images, %s\n", ext.width, ext.height, count,
        ci.presentMode < 4 ? modes[ci.presentMode] : "other present mode");
    for (uint32_t i = 0; i < g_vk.sc_count; i++) vkDestroyImageView(g_vk.dev, g_vk.sc_views[i], NULL);
    if (old) vkDestroySwapchainKHR(g_vk.dev, old, NULL);
    g_vk.sc_format = sf.format;
    g_vk.sc_extent = ext;
    g_vk.sc_phys = phys;
    if (g_vk.sc_quarter) LOG("[vk] swapchain pre-rotated %d degrees (images %ux%u)\n", g_vk.sc_quarter * 90, phys.width, phys.height);
    {
        int pw, ph;
        SDL_GetWindowSize(window, &pw, &ph);
        LOG("[vk] swapchain %ux%u (surface %ux%u, transform %#x; window %dx%d px, %dx%d points)\n", ext.width, ext.height,
            caps.currentExtent.width, caps.currentExtent.height, caps.currentTransform, w, h, pw, ph);
    }
    uint32_t actual_count = 0;
    VK_CHECK(vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &actual_count, NULL));
    if (!actual_count || actual_count > VK_SWAPCHAIN_MAX_IMAGES)
        FATAL("swapchain returned %u images; supported count is 1..%u\n", actual_count, VK_SWAPCHAIN_MAX_IMAGES);
    g_vk.sc_count = actual_count;
    VK_CHECK(vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &g_vk.sc_count, g_vk.sc_images));
    if (!g_vk.sc_count || g_vk.sc_count > VK_SWAPCHAIN_MAX_IMAGES)
        FATAL("invalid retrieved swapchain image count %u\n", g_vk.sc_count);
    LOG("[vk] complete swapchain image inventory: %u (capacity %u)\n", g_vk.sc_count, VK_SWAPCHAIN_MAX_IMAGES);
    for (uint32_t i = 0; i < g_vk.sc_count; i++) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_vk.sc_images[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = sf.format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_vk.sc_views[i]));
    }
}

static void recreate_swapchain(void) {
    rec_drain();  // (every queued present and submission made: the queue is idle for the wait)
    vkDeviceWaitIdle(g_vk.dev);
    create_swapchain();
}

// ---- the app in the background (Android): the window's surface goes away. Nothing is drawn meanwhile, and the game
// waits in its next present (it pauses where it is: no CPU, no battery); back in front, a new surface and swapchain.
static bool SDLCALL lifecycle_watch(void *user, SDL_Event *e) {
    (void)user;
    if (e->type == SDL_EVENT_WILL_ENTER_BACKGROUND || e->type == SDL_EVENT_DID_ENTER_BACKGROUND) {
        if (!atomic_exchange(&g_background, true)) {
            LOG("[vk] in the background: game paused\n");
            vk_pipeline_cache_save();  // (phones end apps in the background without exit(): the exit handler never runs)
            settings_save();           // (likewise: options changed with the menu still open were lost)
        }
    } else if (e->type == SDL_EVENT_DID_ENTER_FOREGROUND && atomic_load(&g_background)) {
        LOG("[vk] in front again\n");
        atomic_store(&g_surface_stale, true);
        atomic_store(&g_background, false);
    }
    return true;
}

// The window's Vulkan surface (false: SDL_GetError says why)
static bool create_surface(void) {
#ifdef __ANDROID__
    if (custom_driver_on) {  // (as SDL's Android_Vulkan_CreateSurface, through the installed driver's loader)
        struct ANativeWindow *nw = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, NULL);
        if (!nw) return SDL_SetError("no native window (the app is in the background)");
        VkAndroidSurfaceCreateInfoKHR si = {.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, .window = nw};
        VkResult r = vkCreateAndroidSurfaceKHR(g_vk.inst, &si, NULL, &g_vk.surface);
        if (r != VK_SUCCESS) return SDL_SetError("vkCreateAndroidSurfaceKHR: %d", r);
        return true;
    }
#endif
    return SDL_Vulkan_CreateSurface(window, g_vk.inst, NULL, &g_vk.surface);
}

static void new_surface(void) {
    vkDeviceWaitIdle(g_vk.dev);
    for (uint32_t i = 0; i < g_vk.sc_count; i++) vkDestroyImageView(g_vk.dev, g_vk.sc_views[i], NULL);
    g_vk.sc_count = 0;
    if (g_vk.swapchain) vkDestroySwapchainKHR(g_vk.dev, g_vk.swapchain, NULL);
    g_vk.swapchain = VK_NULL_HANDLE;
    if (g_vk.surface) vkDestroySurfaceKHR(g_vk.inst, g_vk.surface, NULL);
    g_vk.surface = VK_NULL_HANDLE;
    if (!create_surface()) {
        LOG("[vk] new surface: %s\n", SDL_GetError());
        atomic_store(&g_surface_stale, true);  // (the window isn't back yet: next frame)
        return;
    }
    create_swapchain();
}

// Called by the game's presenting thread: blocks while the app is in the background.
static void wait_foreground(void) {
    while (atomic_load(&g_background)) SDL_Delay(50);
}

// Time this thread spends waiting here - for a frame slot's GPU work, a window image, the presentation engine -
// counts as idle for the frame pacer (pace.c), like the game's own waits: in FIFO (exclusive fullscreen
// on Windows offers no mailbox) acquire / present block until the display's refresh, and counted as busy that made
// every frame look ~16.6 ms: the game's 30 fps mode outdoors on a fast GPU (issue #44). GPU-bound frames still show
// in the game's GPU counters.
extern __thread uint64_t g_blocked_ns;  // kernel.c
#define WAITING(x) do { uint64_t w0_ = plat_now_ns(); rq_watch(#x, 0); x; rq_watch(NULL, 0); g_blocked_ns += plat_now_ns() - w0_; } while (0)
// the same, also counted by place (dynamic resolution's log / LARECOMP_VK_PROF: 0 frame slot, 1 acquire, 2 present)
uint64_t g_vk_frame_waits[4];
#define WAITING_AT(i, x) do { uint64_t w0_ = plat_now_ns(); rq_watch(#x, 0); x; rq_watch(NULL, 0); uint64_t w1_ = plat_now_ns() - w0_; \
                              g_blocked_ns += w1_; g_vk_frame_waits[i] += w1_; } while (0)

// ================================================================== frames (start screen and game)
// Acquire semaphores rotate per frame in flight; a slot is reused once the submission that waited on it is done.
#define FRAMES 3
static VkSemaphore acquire_sem[FRAMES], done_sem[VK_SWAPCHAIN_MAX_IMAGES];
static uint64_t slot_seq[FRAMES];
static int slot;

static VkSemaphore binary_semaphore(void) {
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore s;
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &s));
    return s;
}

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = from, .newLayout = to, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &di);
}

typedef struct { uint32_t image; VkSemaphore acquired; bool ok; } Frame;
static Frame cur_frame;

static void check_swapchain_image(uint32_t image) {
    if (image >= g_vk.sc_count || image >= VK_SWAPCHAIN_MAX_IMAGES)
        FATAL("swapchain image index %u outside actual count %u (capacity %u)\n",
              image, g_vk.sc_count, VK_SWAPCHAIN_MAX_IMAGES);
}

// Acquire the next window image and begin rendering into it (cleared to `clear`). Returns false when there is
// nothing to draw into (minimised window).
bool vk_frame_begin(const float clear[4]) {
    cur_frame.ok = false;
    if (rq_stopping()) return false;  // quitting: no window image after the window closed (#40)
    if (atomic_load(&g_background)) return false;
    if (atomic_exchange(&g_surface_stale, false)) new_surface();
    if (!g_vk.surface || atomic_load(&g_surface_stale)) return false;
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (!w || !h) return false;  // minimised
    // The last present (the recorder's, record.c) must be done before the swapchain is used again - acquire and present
    // on the same swapchain are not allowed at the same time - and it may have asked for a new swapchain
    WAITING(rec_wait_presents(rec_presents_queued()));
    if (rec_present_recreate()) recreate_swapchain();
    if (!g_vk.swapchain || (uint32_t)w != g_vk.sc_extent.width || (uint32_t)h != g_vk.sc_extent.height) recreate_swapchain();
    if (!g_vk.swapchain) return false;
    static int trace = -1;
    if (trace < 0) trace = getenv("LARECOMP_VK_TRACE") != NULL;
    if (trace) LOG("[vk] frame begin %dx%d (swapchain %ux%u)\n", w, h, g_vk.sc_extent.width, g_vk.sc_extent.height);
    slot = (slot + 1) % FRAMES;
    // Phones: the window image is waited for on the CPU (a fence), not by the GPU (a semaphore). Turnip waits for the
    // compositor's release inside the submission that clears the image, and now and then that wait didn't end: the
    // kernel's hang detection, device lost (breadcrumbs: the GPU done with the game's frame, stuck before the window
    // image's first pass). With MAILBOX the image is ready at once. LARECOMP_VK_ACQUIRE_FENCE=0/1.
    static int by_fence = -1;
    if (by_fence < 0) {
        const char *e = getenv("LARECOMP_VK_ACQUIRE_FENCE");
#ifdef __ANDROID__
        by_fence = !e || atoi(e) != 0;
#else
        by_fence = e && atoi(e) != 0;
#endif
    }
    static VkFence acquire_fence;
    if (by_fence && !acquire_fence) {
        VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK_CHECK(vkCreateFence(g_vk.dev, &fi, NULL, &acquire_fence));
    }
    if (!by_fence && !acquire_sem[slot]) acquire_sem[slot] = binary_semaphore();
    {   // (its time counts as blocked by itself, vk_wait_seq: only counted by place here)
        uint64_t w0 = plat_now_ns();
        vk_wait_seq(slot_seq[slot]);
        g_vk_frame_waits[0] += plat_now_ns() - w0;
    }
    VkSemaphore asem = by_fence ? VK_NULL_HANDLE : acquire_sem[slot];
    VkFence afence = by_fence ? acquire_fence : VK_NULL_HANDLE;
    VkResult r;
    WAITING_AT(1, r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, asem, afence, &cur_frame.image));
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        WAITING_AT(1, r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, asem, afence, &cur_frame.image));
    }
    if (r == VK_ERROR_SURFACE_LOST_KHR) atomic_store(&g_surface_stale, true);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { LOG("[vk] acquire failed: %d\n", r); return false; }
    if (by_fence) {
        WAITING(vkWaitForFences(g_vk.dev, 1, &acquire_fence, VK_TRUE, UINT64_MAX));
        vkResetFences(g_vk.dev, 1, &acquire_fence);
    }
    check_swapchain_image(cur_frame.image);
    cur_frame.acquired = asem;
    slot_seq[slot] = vk_pending_seq();
    VkCommandBuffer cb = vk_cmd();
    extern void vk_bc_mark(VkCommandBuffer cb, const char *label);
    vk_bc_mark(cb, "window image: begin");
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    vk_gpu_timer_window_start();  // (the GPU timer: this submission's work starts after that barrier, which waits for the image)
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_vk.sc_views[cur_frame.image],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                    .clearValue.color.float32 = {clear[0], clear[1], clear[2], clear[3]}};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_phys}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    rec_CmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)g_vk.sc_phys.width, (float)g_vk.sc_phys.height, 0, 1};
    VkRect2D sc = {{0, 0}, g_vk.sc_phys};
    rec_CmdSetViewport(cb, 0, 1, &vp);
    rec_CmdSetScissor(cb, 0, 1, &sc);
    extern void vk_imgui_prerotation(int quarter);  // (the UI drawn into this image turned like it: imgui_glue.cpp)
    vk_imgui_prerotation(g_vk.sc_quarter);
    cur_frame.ok = true;
    return true;
}

// Passes of their own between vk_frame_begin and vk_frame_end (exec.c post passes): end the window image's
// rendering, and begin it again keeping what was drawn.
void vk_frame_suspend(void) {
    if (!cur_frame.ok) return;
    rec_CmdEndRendering(vk_cmd());
    extern void vk_bc_mark(VkCommandBuffer cb, const char *label);
    vk_bc_mark(vk_cmd(), "window image: cleared");
}
void vk_frame_resume(void) {
    if (!cur_frame.ok) return;
    VkCommandBuffer cb = vk_cmd();
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_vk.sc_views[cur_frame.image],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_phys}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    rec_CmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)g_vk.sc_phys.width, (float)g_vk.sc_phys.height, 0, 1};
    VkRect2D sc = {{0, 0}, g_vk.sc_phys};
    rec_CmdSetViewport(cb, 0, 1, &vp);
    rec_CmdSetScissor(cb, 0, 1, &sc);
}

VkExtent2D vk_frame_extent(void) { return g_vk.sc_extent; }

// A rectangle of the (landscape) window in the swapchain image's pixels, for viewports and scissors drawn turned
// (clip space turned by the same quarter: 1 -> (-y, x), 3 -> (y, -x))
VkRect2D vk_sc_rect(int32_t x, int32_t y, int32_t w, int32_t h) {
    int32_t W = (int32_t)g_vk.sc_extent.width, H = (int32_t)g_vk.sc_extent.height;
    if (g_vk.sc_quarter == 1) return (VkRect2D){{H - y - h, x}, {(uint32_t)h, (uint32_t)w}};
    if (g_vk.sc_quarter == 3) return (VkRect2D){{y, W - x - w}, {(uint32_t)h, (uint32_t)w}};
    return (VkRect2D){{x, y}, {(uint32_t)w, (uint32_t)h}};
}
VkViewport vk_sc_viewport(float x, float y, float w, float h) {
    float W = (float)g_vk.sc_extent.width, H = (float)g_vk.sc_extent.height;
    if (g_vk.sc_quarter == 1) return (VkViewport){H - y - h, x, h, w, 0, 1};
    if (g_vk.sc_quarter == 3) return (VkViewport){y, W - x - w, h, w, 0, 1};
    return (VkViewport){x, y, w, h, 0, 1};
}
int vk_sc_quarter(void) { return g_vk.sc_quarter; }
// The window image's command buffer for direct recording (ImGui, the window's own passes): the recorder has recorded
// everything queued before it, and records what is queued after it.
void *vk_frame_cmd(void) {
    VkCommandBuffer cb = vk_cmd();
    rec_drain();
    return (void *)cb;
}

// A present's result (the recorder's thread, record.c present_run): whether the swapchain must be made again. A turned
// swapchain (pre-rotation) presents "suboptimal" every frame: that's the turn, not a new size.
bool vk_present_result(VkResult r) {
    if (r == VK_ERROR_SURFACE_LOST_KHR) atomic_store(&g_surface_stale, true);
#ifdef __ANDROID__
    static int presented;
    if (presented < 60 && r >= 0 && ++presented == 60) {
        pthread_mutex_lock(&driver_probe_lock);
        if (driver_probe[0]) unlink(driver_probe), driver_probe[0] = 0;  // (the driver works)
        pthread_mutex_unlock(&driver_probe_lock);
    }
#endif
    return r == VK_ERROR_OUT_OF_DATE_KHR || (r == VK_SUBOPTIMAL_KHR && !g_sc_turned);
}

static void save_window_image(VkCommandBuffer cb, VkSpan *out) {
    uint32_t w = g_vk.sc_phys.width, h = g_vk.sc_phys.height;
    *out = vk_readback((uint64_t)w * h * 4);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy rg = {.bufferOffset = out->off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageExtent = {w, h, 1}};
    rec_CmdCopyImageToBuffer(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out->buf, 1, &rg);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

static bool g_fg_copy;  // vk_frame_end: copy the window picture for frame generation (backend_present_gpu)
static double g_shown_fps;
double vk_shown_fps(void) { return g_shown_fps; }
// End rendering into the window image and present it. shot_path: also save the window image as PNG.
void vk_frame_end(const char *shot_path) {
    if (!cur_frame.ok) return;
    check_swapchain_image(cur_frame.image);
    VkCommandBuffer cb = vk_cmd();
    rec_CmdEndRendering(cb);
    VkSpan shot = {0};
    if (shot_path) save_window_image(cb, &shot);
    if (g_fg_copy) {  // (frame generation: the real window picture, for the in-between frame)
        extern void fg_copy_window(VkCommandBuffer cb, VkImage sc, VkFormat fmt, uint32_t w, uint32_t h);
        fg_copy_window(cb, g_vk.sc_images[cur_frame.image], g_vk.sc_format, g_vk.sc_extent.width, g_vk.sc_extent.height);
        g_fg_copy = false;
    }
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    uint32_t img = cur_frame.image;
    if (!done_sem[img]) done_sem[img] = binary_semaphore();
    uint64_t seq = vk_pending_seq();
    vk_submit_extra(cur_frame.acquired, done_sem[img], VK_NULL_HANDLE);
    // the present follows the submission in the recorder's order (its wait for the display is the recorder's, counted in
    // g_vk_frame_waits[2]; vk_present_result: an out-of-date result makes the next vk_frame_begin recreate the swapchain)
    rec_QueuePresentKHR(done_sem[img], g_vk.swapchain, img);
    cur_frame.ok = false;
    {   // pictures shown a second (generated ones too): the menu's FPS counter
        static uint64_t t0, n;
        uint64_t now = SDL_GetTicksNS();
        n++;
        if (!t0) t0 = now;
        if (now - t0 >= 500000000ull) g_shown_fps = n * 1e9 / (double)(now - t0), n = 0, t0 = now;
    }
    if (shot_path) {
        vk_wait_seq(seq);
        uint32_t w = g_vk.sc_phys.width, h = g_vk.sc_phys.height;
        uint8_t *px = shot.ptr;
        bool bgr = g_vk.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g_vk.sc_format == VK_FORMAT_B8G8R8A8_SRGB;
        for (uint64_t i = 0; bgr && i < (uint64_t)w * h; i++) { uint8_t t = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = t; }
        for (uint64_t i = 0; i < (uint64_t)w * h; i++) px[i * 4 + 3] = 255;
        if (g_vk.sc_quarter) {  // (the pre-rotated image turned back into the window's shape)
            uint32_t W = h, H = w;  // landscape
            uint8_t *t = malloc((size_t)W * H * 4);
            for (uint32_t y = 0; y < H; y++)
                for (uint32_t x = 0; x < W; x++) {
                    uint32_t qx = g_vk.sc_quarter == 1 ? H - 1 - y : y, qy = g_vk.sc_quarter == 1 ? x : W - 1 - x;
                    memcpy(t + ((size_t)y * W + x) * 4, px + ((size_t)qy * w + qx) * 4, 4);
                }
            vk_save_png(shot_path, t, W, H);
            free(t);
        } else {
            vk_save_png(shot_path, px, w, h);
        }
    }
}

void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
#ifdef __ANDROID__
    // (a relative path would name an APK asset: under the app's files folder instead, its folders made)
    char full[1024];
    const char *dir = SDL_GetAndroidExternalStoragePath();
    if (path[0] != '/' && dir) {
        snprintf(full, sizeof full, "%s/%s", dir, path);
        for (char *q = full + strlen(dir) + 1; (q = strchr(q, '/')); q++) { *q = 0; mkdir(full, 0755); *q = '/'; }
        path = full;
    }
#endif
    SDL_Surface *s = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_ABGR8888, (void *)rgba, (int)w * 4);
    if (!s || !SDL_SavePNG(s, path)) LOG("[vk] saving %s failed: %s\n", path, SDL_GetError());
    else LOG("[vk] saved %s (%ux%u)\n", path, w, h);
    SDL_DestroySurface(s);
}

// ================================================================== game presentation (metal.m backend_present)
// Draws the game's image into the window with the widescreen crop, then the overlay menu.
extern void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh, const float pv[8]);
extern bool g_debug_frame;  // (exec.c; set in present_hooks, read in the render thread's present)
void renderer_submit(void);
void renderer_set_scale(float sx, float sy);
uint64_t renderer_present_count(void);
void renderer_save_texture_tex(TexBackend *b, const char *path);

static uint64_t frames, total_frames, g_frame_counter;
static unsigned g_gen_frames;  // in-between frames since the last fps line
uint64_t renderer_present_count(void) { return total_frames; }  // (the game thread's frame: the HLE hooks')
// The frame whose work is being translated (rqueue.h rq_frame): "once per frame" state of the translation. Without the
// render thread, total_frames.
uint64_t renderer_exec_frame(void) { return rq_current_item() ? rq_frame() : total_frames; }
uint64_t vk_frame_counter(void) { return renderer_exec_frame(); }  // (the translation's: g_frame_counter == total_frames)
static float g_shot_crop[4] = {0, 0, 1, 1};
static double last_fps_time;
static NvnTexture *g_last_presented;

const float *vk_shot_crop(void) { return g_shot_crop; }

void backend_present(NvnTexture *t);
void *backend_pool_push(void) { return NULL; }
void backend_pool_pop(void *p) { (void)p; }
void backend_represent(void) {
    if (!g_last_presented) { SDL_Delay(16); return; }
    if (rq_on()) rq_present(g_last_presented, total_frames, false);
    else backend_present_now(g_last_presented, total_frames);
}

static bool menu_open(void) { extern bool menu_is_open(void); return menu_is_open(); }
static uint32_t game_interval(void) { extern uint32_t g_game_interval; return g_game_interval; }

// A frame of only the game's 16:9 UI box in a window of another shape (exec.c g_box_drawn): the target's rest is
// another frame's picture - the bars are covered with the box's own edges, blurred and dimmed (one picture with the
// game's, no flicker). The picture was drawn into (vx, vy, vw, vh) from the uv crop of b.
// again: the same frame's picture once more (frame generation, the menu's redraw)
static void bars_backdrop(VkCommandBuffer cb, TexBackend *b, double vx, double vy, double vw, double vh, const float crop[4],
                          bool flip, bool again) {
    extern bool g_box_drawn, g_full_drawn;
    extern void *g_box_rt;
    extern float g_box_frac[4];
    extern void vk_present_backdrop(VkCommandBuffer cb, float x, float y, float w, float h, const float uvr[4], bool flip,
                                    float light);
    static int on = -1;  // LARECOMP_BARS=0: the bars as the target has them
    if (on < 0) on = !getenv("LARECOMP_BARS") || atoi(getenv("LARECOMP_BARS"));
    static bool need;  // (this present's real frame: kept for its in-between frame and the menu's redraw)
    if (!again) need = g_box_drawn && !g_full_drawn && g_box_rt == (void *)b, g_box_drawn = g_full_drawn = false;
    if (!on || g_settings.black_bars || crop[2] <= 0 || crop[3] <= 0) return;
    {   // the window around the picture (a 16:9 picture - movies, menus, Authentic 16:9 - in a wider or taller window):
        // the picture's own edges there too, instead of black
        double W = g_vk.sc_extent.width, H = g_vk.sc_extent.height, cw = crop[2] * 0.3, ch = crop[3] * 0.3;
        if (vx >= 1) {
            float uv[4] = {crop[0], crop[1], (float)cw, crop[3]};
            vk_present_backdrop(cb, 0, (float)vy, (float)vx, (float)vh, uv, flip, 0.42f);
        }
        if (W - vx - vw >= 1) {
            float uv[4] = {(float)(crop[0] + crop[2] - cw), crop[1], (float)cw, crop[3]};
            vk_present_backdrop(cb, (float)(vx + vw), (float)vy, (float)(W - vx - vw), (float)vh, uv, flip, 0.42f);
        }
        if (vy >= 1) {
            float uv[4] = {crop[0], flip ? (float)(crop[1] + crop[3] - ch) : crop[1], crop[2], (float)ch};
            vk_present_backdrop(cb, 0, 0, (float)W, (float)vy, uv, flip, 0.42f);
        }
        if (H - vy - vh >= 1) {
            float uv[4] = {crop[0], flip ? crop[1] : (float)(crop[1] + crop[3] - ch), crop[2], (float)ch};
            vk_present_backdrop(cb, 0, (float)(vy + vh), (float)W, (float)(H - vy - vh), uv, flip, 0.42f);
        }
    }
    if (!need) return;
    const float *f = g_box_frac;
    double x0 = vx + (f[0] - crop[0]) / crop[2] * vw, x1 = vx + (f[1] - crop[0]) / crop[2] * vw;
    double y0 = vy + (f[2] - crop[1]) / crop[3] * vh, y1 = vy + (f[3] - crop[1]) / crop[3] * vh;
    float bw = f[1] - f[0], bh = f[3] - f[2], light = 0.42f;
    if (x0 - vx >= 1) {  // left, from the box's left third; right, from its right third
        float uv[4] = {f[0], f[2], bw * 0.3f, bh};
        vk_present_backdrop(cb, (float)vx, (float)vy, (float)(x0 - vx), (float)vh, uv, flip, light);
    }
    if (vx + vw - x1 >= 1) {
        float uv[4] = {f[1] - bw * 0.3f, f[2], bw * 0.3f, bh};
        vk_present_backdrop(cb, (float)x1, (float)vy, (float)(vx + vw - x1), (float)vh, uv, flip, light);
    }
    if (y0 - vy >= 1) {  // (a taller window: above and below)
        float uv[4] = {f[0], flip ? f[3] - bh * 0.3f : f[2], bw, bh * 0.3f};
        vk_present_backdrop(cb, (float)x0, (float)vy, (float)(x1 - x0), (float)(y0 - vy), uv, flip, light);
    }
    if (vy + vh - y1 >= 1) {
        float uv[4] = {f[0], flip ? f[2] : f[3] - bh * 0.3f, bw, bh * 0.3f};
        vk_present_backdrop(cb, (float)x0, (float)y1, (float)(x1 - x0), (float)(vy + vh - y1), uv, flip, light);
    }
}

// (LARECOMP_VK_PROF) the present's time by part, per frame in the fps line ("present/frame"): submit = the frame's
// last work submitted, acquire = frame slot + window image (waits included), scale = output scale / widescreen /
// dynamic resolution, blit = the game image into the window (FSR / blit / AA recording), overlay = menu_render (ImGui),
// end = the window image's submission + vkQueuePresentKHR, rest = test hooks and the fps line
enum { PP_SUBMIT, PP_ACQUIRE, PP_SCALE, PP_BLIT, PP_OVERLAY, PP_END, PP_REST, PP_N };
static int pp_on = -1;
static uint64_t pp_ns[PP_N], pp_frames, pp_t;
static void pp_mark(int part) {
    if (!pp_on) return;
    uint64_t t = SDL_GetTicksNS();
    pp_ns[part] += t - pp_t;
    pp_t = t;
}
static const char *pp_text(void) {
    static char s[192];
    s[0] = 0;
    if (!pp_on || !pp_frames) return s;
    double f = (double)pp_frames * 1e6;
    snprintf(s, sizeof s, " | present/frame submit %.3f acquire %.3f scale %.3f blit %.3f overlay %.3f end %.3f rest %.3f ms",
             pp_ns[PP_SUBMIT] / f, pp_ns[PP_ACQUIRE] / f, pp_ns[PP_SCALE] / f, pp_ns[PP_BLIT] / f, pp_ns[PP_OVERLAY] / f,
             pp_ns[PP_END] / f, pp_ns[PP_REST] / f);
    memset(pp_ns, 0, sizeof pp_ns);
    pp_frames = 0;
    return s;
}

// The GPU side of a present (the render thread when it's on, rqueue.c): the present pass, the overlay, shots.
// frame: the number of this present (the game thread's frame when it was queued).
void backend_present_now(NvnTexture *t, uint64_t frame) {
    extern void vk_pass_gpu_report(void);
    vk_pass_gpu_report();  // (LARECOMP_VK_PASS_GPU: exec.c)
    if (pp_on < 0) pp_on = getenv("LARECOMP_VK_PROF") != NULL;
    if (pp_on) pp_t = SDL_GetTicksNS();
    TexBackend *b = vk_tex(t, true);
    b->presented = true;
    vk_end_pass();
    // the frame's work goes to the GPU now, in a submission of its own: it starts while we wait for the window's image.
    // Its GPU time is counted, and the window image's submission's from after its wait (exec.c gpu_timer_add)
    renderer_submit();
    pp_mark(PP_SUBMIT);
    static const float black[4] = {0, 0, 0, 1};
    wait_foreground();
    uint64_t t0 = SDL_GetTicksNS();
    bool have = vk_frame_begin(black);
    pp_mark(PP_ACQUIRE);
    uint64_t nd = SDL_GetTicksNS() - t0;
    if (nd > 30000000) {
        static int warned;
        if (warned++ < 20) LOG("[vk] acquire blocked %llums\n", (unsigned long long)(nd / 1000000));
    }
    if (have) {
        double dw = g_vk.sc_extent.width, dh = g_vk.sc_extent.height;
        extern void widescreen_frame_end(uint64_t frame);
        widescreen_frame_end(frame);
        extern void widescreen_set_factor(float f);
        extern bool widescreen_allowed(void);
        // render scale + placement in the window: runtime/nvn/output_scale.c (Video tab "Internal resolution");
        // GPU time since the last frame (exec.c's GPU timer; -1 without one: dynamic resolution goes by frame times):
        // the game's submissions and the window image's (LARECOMP_DYNRES_COUNT_PRESENT=0: the game's only, as before)
        static int count_present = -1;
        if (count_present < 0) count_present = !getenv("LARECOMP_DYNRES_COUNT_PRESENT") || atoi(getenv("LARECOMP_DYNRES_COUNT_PRESENT"));
        static uint64_t sp_prev[4], wt_prev[4], cw_prev;
        static bool sp_have;
        uint64_t sp[4], wt[4];
        for (int i = 0; i < 4; i++) wt[i] = __atomic_load_n(&g_vk_frame_waits[i], __ATOMIC_RELAXED);  // ([2], [3]: the recorder's)
        double game_ms = -1, win_ms = -1;
        if (vk_gpu_split_ns(sp)) {
            uint64_t cw = vk_cum_wait_ns();
            if (sp_have) {
                double game = (sp[0] - sp_prev[0]) / 1e6, win = (sp[1] - sp_prev[1]) / 1e6;
                game_ms = game, win_ms = count_present ? win : -1;
                double slotw = (wt[0] - wt_prev[0]) / 1e6;
                DynSplit ds = {game, win, (sp[2] - sp_prev[2]) / 1e6, (double)(sp[3] - sp_prev[3]), slotw,
                               (wt[1] - wt_prev[1]) / 1e6, (wt[2] - wt_prev[2]) / 1e6,
                               fmax(0, (cw - cw_prev) / 1e6 - slotw), count_present != 0};
                output_scale_note_split(&ds);
            }
            memcpy(sp_prev, sp, sizeof sp);
            memcpy(wt_prev, wt, sizeof wt_prev);
            cw_prev = cw;
            sp_have = true;
        }
        {   // phones (and no GPU timer split): the GPU's time for the frame from the fence watcher's sum, exec.c - what
            // the phones' dynamic resolution controller (output_scale.c) is tuned to
            extern _Atomic uint64_t g_gpu_busy_ns;
            static uint64_t last_gb;
            uint64_t gb = atomic_load(&g_gpu_busy_ns);
            double fence_ms = gb && last_gb ? (gb - last_gb) / 1e6 : -1;
            last_gb = gb;
#ifdef __ANDROID__
            game_ms = fence_ms, win_ms = -1;
#else
            if (game_ms < 0) game_ms = fence_ms;
#endif
        }
        OutScale os;
        output_scale(dw, dh, widescreen_allowed(), SDL_GetTicksNS() / 1e9, game_ms, win_ms, &os);
        double sx = os.sx, sy = os.sy, vx = os.vx, vy = os.vy, vw = os.vw, vh = os.vh, shape = os.shape;
        output_scale_log((unsigned long long)frame, dw, dh, &os);  // (diagnostics: [scale] on a change)
        // interiors / dungeons: their 16:9 centre, or the part inside a room wider than one screen
        extern void widescreen_present_crop(uint64_t, double, float *, double *, double *, double *, double *);
        float crop[4];  // uv origin and size
        widescreen_present_crop(frame, shape, crop, &vx, &vy, &vw, &vh);
        widescreen_set_factor((float)shape);
        extern void renderer_set_alloc_scale(float ax, float ay);
        renderer_set_alloc_scale((float)os.ax, (float)os.ay);
        renderer_set_scale((float)sx, (float)sy);
        extern void renderer_set_display_height(float h);
        extern void input_set_present_rect(double, double, double, double);
        input_set_present_rect(vx, vy, vw, vh);
        renderer_set_display_height((float)vh);  // (the game picture's height in the window)
        pp_mark(PP_SCALE);
        VkCommandBuffer cb = vk_cmd();
        float pv[8] = {renderer_origin_lower_left() ? 1.0f : 0.0f, 0, 0, 0, crop[0], crop[1], crop[2], crop[3]};
        memcpy(g_shot_crop, crop, sizeof crop);
        vk_present_draw(cb, b, (float)vx, (float)vy, (float)vw, (float)vh, pv);
        bars_backdrop(cb, b, vx, vy, vw, vh, crop, pv[0] > 0.5f, false);
        pp_mark(PP_BLIT);
        static long menu_at = -1;
        if (menu_at < 0) menu_at = getenv("LARECOMP_MENU_AT") ? atol(getenv("LARECOMP_MENU_AT")) : 0;
        if (menu_at && (long)frame + 1 == menu_at) { extern void menu_test_open(void); menu_test_open(); }
        // (ImGui records into cb itself: menu_render drains the recorder right before, when it draws anything)
        menu_render((void *)cb, g_vk.sc_extent.width, g_vk.sc_extent.height);
        pp_mark(PP_OVERLAY);
        static long win_shot = -1;
        if (win_shot < 0) win_shot = getenv("LARECOMP_WINDOW_SHOT") ? atol(getenv("LARECOMP_WINDOW_SHOT")) : 0;
        bool shot = win_shot && (long)frame + 1 == win_shot;
        extern bool fg_frame_wanted(uint64_t frame, TexBackend *fin);
        extern bool fg_draw(VkCommandBuffer cb, VkFormat fmt, uint32_t ww, uint32_t wh, TexBackend *fin, const float rect[4],
                            const float crop[4], bool flip_present);
        bool gen = fg_frame_wanted(frame, b);
        g_gen_frames += gen;  // (the fps line: in-between frames shown)
        g_fg_copy = gen;
        // LARECOMP_FG_SHOT=<frame>: that frame's window picture, its in-between one and the next real one (build/fg_*.png)
        static long fg_shot = -1;
        if (fg_shot < 0) fg_shot = getenv("LARECOMP_FG_SHOT") ? atol(getenv("LARECOMP_FG_SHOT")) : 0;
        bool fs0 = fg_shot && (long)frame == fg_shot, fs1 = fg_shot && (long)frame == fg_shot + 1;
        vk_frame_end(shot ? "build/window.png" : fs0 ? "build/fg_real0.png" : fs1 ? "build/fg_real1.png" : NULL);
        if (gen && vk_frame_begin(black)) {  // the in-between frame: the next swapchain image, presented a refresh later
            float rect[4] = {(float)vx, (float)vy, (float)vw, (float)vh};
            fg_draw(vk_cmd(), g_vk.sc_format, g_vk.sc_extent.width, g_vk.sc_extent.height, b, rect, crop, pv[0] > 0.5f);
            bars_backdrop(vk_cmd(), b, vx, vy, vw, vh, crop, pv[0] > 0.5f, true);
            extern bool fg_draws_overlays(void);  // (drawn from the presented picture: the window's overlays on top again)
            extern void menu_redraw(void *cmd);  // (the real frame's overlays again: no second UI frame - touch input)
            if (fg_draws_overlays()) menu_redraw((void *)vk_cmd());
            vk_frame_end(fs0 ? "build/fg_gen0.png" : NULL);
        } else if (!gen && g_fifo_present && menu_open() && game_interval() == 2 && vk_frame_begin(black)) {
            // the menu over the game's 30: a second picture with the menu drawn again from the latest input, a refresh
            // later - scrolling with a finger moves at 60 instead of with the game's frames
            cb = vk_cmd();
            vk_present_draw(cb, b, (float)vx, (float)vy, (float)vw, (float)vh, pv);
            bars_backdrop(cb, b, vx, vy, vw, vh, crop, pv[0] > 0.5f, true);
            menu_render((void *)cb, g_vk.sc_extent.width, g_vk.sc_extent.height);
            vk_frame_end(NULL);
        }
        if (shot && getenv("LARECOMP_EXIT_AFTER_SHOT")) exit_after_shot();
    } else {
        renderer_submit();
    }
    pp_mark(PP_END);
    pp_frames++;
    extern void vk_prof_frame(uint64_t frame, uint64_t present_ns);
    vk_prof_frame(frame, SDL_GetTicksNS() - t0);
    frames++;
    static long shot_every = -1;
    if (shot_every < 0) { const char *e = getenv("LARECOMP_SCREENSHOT_EVERY"); shot_every = e ? atol(e) : 0; }
    {   // LARECOMP_GPUPROF_AT=<frame>[,<frame>...]: GPU time per pass of those frames ("[gpuprof]" in the log)
        extern bool g_gpuprof_frame;
        extern void vk_gpuprof_report(void);
        if (g_gpuprof_frame) { g_gpuprof_frame = false; vk_gpuprof_report(); }
        const char *e = getenv("LARECOMP_GPUPROF_AT");
        for (const char *p = e; p && *p;) {
            long at = strtol(p, (char **)&p, 10);
            if (at == (long)frame) { g_gpuprof_frame = true; break; }
            if (*p == ',') p++; else break;
        }
    }
    if (g_debug_frame) LOG("===== debug frame %llu =====\n", (unsigned long long)frame);
    {   // LARECOMP_SHOT_AT=<frame>,<frame>,... (ascending): the game image at those frames -> build/frames/frame_N.png
        static const char *next;
        static int init;
        if (!init) { init = 1; next = getenv("LARECOMP_SHOT_AT"); if (next) mkdir("build/frames", 0755); }
        while (next && *next) {
            long at = strtol(next, (char **)&next, 10);
            if ((long)frame < at) { while (next > getenv("LARECOMP_SHOT_AT") && next[-1] != ',') next--; break; }
            if ((long)frame == at) {
                char path[256];
                snprintf(path, sizeof path, "build/frames/frame_%05llu.png", (unsigned long long)frame);
                renderer_save_texture_tex(b, path);
            }
            if (*next == ',') next++;
        }
    }
    static long shot_from = -1;
    if (shot_from < 0) { const char *e = getenv("LARECOMP_SCREENSHOT_FROM"); shot_from = e ? atol(e) : 0; }
    if (shot_every && frame % shot_every == 0 && (long)frame >= shot_from) {
        char path[256];
        if (shot_from) mkdir("build/frames", 0755);
        snprintf(path, sizeof path, shot_from ? "build/frames/frame_%05llu.png" : "build/frame_%05llu.png",
                 (unsigned long long)frame);
        renderer_save_texture_tex(b, path);
        // a series (SCREENSHOT_FROM) runs on until LARECOMP_WINDOW_SHOT exits
        if (getenv("LARECOMP_EXIT_AFTER_SHOT") && !shot_from) exit_after_shot();
    }
    double now = SDL_GetTicks() / 1000.0;
    static double fps_iv = -1, prev_present, max_gap;
    if (fps_iv < 0) fps_iv = getenv("LARECOMP_FPS_INTERVAL") ? atof(getenv("LARECOMP_FPS_INTERVAL")) : 2.0;
    if (prev_present > 0 && now - prev_present > max_gap) max_gap = now - prev_present;
    static double hitch_ms = -1;  // LARECOMP_LOG_HITCH_MS=<ms>: every present gap over that (tests: hitches per run)
    if (hitch_ms < 0) hitch_ms = getenv("LARECOMP_LOG_HITCH_MS") ? atof(getenv("LARECOMP_LOG_HITCH_MS")) : 0;
    if (hitch_ms > 0 && prev_present > 0 && (now - prev_present) * 1000 > hitch_ms)
        LOG("[hitch] t=%.2f frame %llu gap %.0f ms\n", now, (unsigned long long)frame, (now - prev_present) * 1000);
    prev_present = now;
    pp_mark(PP_REST);  // (the fps line itself is not counted)
    if (now - last_fps_time >= fps_iv) {
        extern const char *game_level_name(void);
        if (last_fps_time > 0)
        {
            extern double g_game_speed;  // (runtime/hle/framepace.c: the game's speed, its 30 / 60 fps mode)
            extern uint32_t g_game_interval;
            extern _Atomic uint64_t g_gpu_busy_ns;
            static uint64_t gb_at;
            uint64_t gbn = atomic_load(&g_gpu_busy_ns);
            double gpu_avg = frames && gb_at ? (gbn - gb_at) / 1e6 / frames : 0;  // (GPU ms per presented frame)
            gb_at = gbn;
            LOG("[vk] t=%.1f frame %llu %.1f fps (+%.1f generated) max %.0fms gpu %.1fms speed %.0f%% (mode %d) level %s (%s%s)\n", now,
                (unsigned long long)frame, frames / (now - last_fps_time), g_gen_frames / (now - last_fps_time), max_gap * 1000.0,
                gpu_avg, g_game_speed * 100, g_game_interval ? 60 / (int)g_game_interval : 60, game_level_name(), renderer_stats(), pp_text());
            g_gen_frames = 0;
        }
        max_gap = 0;
        frames = 0;
        last_fps_time = now;
    }
    static double mem_at = 60.0;  // (memory in steady state: a minute in, then every 10 minutes)
    if (now >= mem_at) { vk_log_memory("playing"); mem_at = now + 600.0; }
}

// The game thread's side of a present: the frame counters and the per-frame hooks (input, pacing, test hooks).
static void present_hooks(void) {
    total_frames++;
    g_frame_counter++;
    if (total_frames % 120 == 1) plat_thread_core(PLAT_CORE_MAIN);  // (again now and then: Android resets it when the
                                                                    // app's cpuset changes, e.g. on reaching the foreground)
    if (getenv("LARECOMP_LOG_SYNCWAIT")) {
        extern uint64_t vk_pending_seq(void);
        LOG("[sync] present f%llu after seq %llu\n", (unsigned long long)total_frames, (unsigned long long)vk_pending_seq() - 1);
    }
    pace_frame();  // (pace.c: the game's 30 fps mode when frames don't fit 60)
    extern void input_frame(uint64_t frame);
    input_frame(total_frames);
    static long slow_ms = -1;  // LARECOMP_SIM_SLOW_FRAME_MS=<ms>: sleep per presented frame (simulate a slow machine)
    if (slow_ms < 0) slow_ms = getenv("LARECOMP_SIM_SLOW_FRAME_MS") ? atol(getenv("LARECOMP_SIM_SLOW_FRAME_MS")) : 0;
    static long slow_until = -1;  // LARECOMP_SLOW_FRAME_UNTIL=<frame>: the stall ends there (frame pacing recovery tests)
    if (slow_until < 0) slow_until = getenv("LARECOMP_SLOW_FRAME_UNTIL") ? atol(getenv("LARECOMP_SLOW_FRAME_UNTIL")) : 0;
    if (slow_ms > 0 && total_frames > 2600 && (!slow_until || (long)total_frames < slow_until)) SDL_Delay((Uint32)slow_ms);
    extern void fnprof_frame(uint64_t frame);
    fnprof_frame(total_frames);
    extern void warp_frame(uint64_t frame);
    warp_frame(total_frames);
    extern void reboot_test_frame(uint64_t frame);  // runtime/main.c (LARECOMP_REBOOT_AT)
    reboot_test_frame(total_frames);
    extern void save_editor_frame(uint64_t frame);
    if (shared_title_las()) save_editor_frame(total_frames);
    static long pgo_every = -1;  // LARECOMP_PGO_DUMP_EVERY=<frames>: an instrumented game library's profile (build/pgo/)
    if (pgo_every < 0) pgo_every = getenv("LARECOMP_PGO_DUMP_EVERY") ? atol(getenv("LARECOMP_PGO_DUMP_EVERY")) : 0;
    if (pgo_every > 0 && total_frames % (uint64_t)pgo_every == 0) {
        extern bool gamelib_pgo_dump(const char *path);
        mkdir("build", 0755);
        mkdir("build/pgo", 0755);
        if (gamelib_pgo_dump("build/pgo/lasgame.profraw")) LOG("[pgo] profile written (frame %llu)\n", (unsigned long long)total_frames);
    }
    extern void vk_level_change_check(void);
    vk_level_change_check();
    extern void vk_pipeline_cache_autosave(void);
    vk_pipeline_cache_autosave();
    static long debug_at = -1;
    if (debug_at < 0) { const char *e = getenv("LARECOMP_DEBUG_FRAME"); debug_at = e ? atol(e) : 0; }
    extern volatile int g_debug_frame_request;
    if (g_debug_frame_request) { g_debug_frame_request = 0; debug_at = (long)total_frames + 1; }
    // LARECOMP_DEBUG_FRAMES=<n> (with LARECOMP_DEBUG_FRAME / F9): dump n consecutive frames (build/pass_<frame>_NNN.png)
    static long debug_n = -1;
    if (debug_n < 0) { const char *e = getenv("LARECOMP_DEBUG_FRAMES"); debug_n = e && atol(e) > 1 ? atol(e) : 1; }
    g_debug_frame = debug_at && total_frames >= (uint64_t)debug_at && total_frames < (uint64_t)debug_at + (uint64_t)debug_n;
}

void backend_present(NvnTexture *t) {
    g_last_presented = t;
    uint64_t frame = total_frames, t0 = plat_now_ns();
    if (rq_on()) rq_present(t, frame, true);
    else backend_present_now(t, frame);
    uint64_t t1 = plat_now_ns();
    g_present_ns += t1 - t0;  // (LARECOMP_LOG_PACE, pace.c)
    present_hooks();
    g_hooks_ns += plat_now_ns() - t1;
}

// ================================================================== UI thread
void vk_exec_init(void);
void vk_shaders_init(void);

// The end of a validation run (LARECOMP_VK_VALIDATION, after backend_quit: the GPU idle, the pipeline cache kept):
// the debug messenger goes, then the process ends at once - no vkDestroyDevice / vkDestroyInstance, no atexit
// handlers, no DLL teardown. _exit() alone isn't enough on Windows: it ends in ExitProcess, which runs the layer's
// and the driver's DLL teardown after every other thread is gone, and with the Khronos layer on NVIDIA that left the
// process hung there and unkillable (one thread waiting, gigabytes of the layer's state).
static _Noreturn void validation_exit(void) {
    if (messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT f =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst, "vkDestroyDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerEXT m = messenger;
        messenger = VK_NULL_HANDLE;
        if (f) f(g_vk.inst, m, NULL);
    }
    LOG("[vk] validation run: ending without GPU teardown\n");
    fflush(stdout);
    fflush(stderr);
#if defined(_WIN32) || defined(__linux__)
    plat_terminate();
#endif
    _Exit(0);
}

// LARECOMP_EXIT_AFTER_SHOT: the test run ends with its shot. As it always did (_exit: no cache save), except under
// validation: backend_quit, then validation_exit.
static void exit_after_shot(void) {
    fflush(stderr);
    if (validating) {
        backend_quit();
        validation_exit();
    }
    _exit(0);
}

// Quitting, before exit() (the window closed; also from exit() itself, quick_exit_handler): the render thread stops
// between items, so no image is acquired or presented any more; the recorder thread stops making driver calls (the
// packets it still had are dropped, record.c rec_quit) and is idle; then the GPU finishes and the pipeline cache is
// kept. All of it before exit() runs the atexit handlers and the libraries' destructors: a Vulkan layer loaded with
// the instance (the Steam overlay on the Steam Deck, #40) tears its swapchain state down there, and a present from
// the render thread after that crashed in vkQueuePresentKHR. The swapchain, surface and window are never destroyed
// explicitly: the process ends with them.
void backend_quit(void) {
    static atomic_flag once = ATOMIC_FLAG_INIT;
    if (atomic_flag_test_and_set(&once)) return;
    static int trace = -1;
    if (trace < 0) trace = getenv("LARECOMP_VK_TRACE") != NULL;
    if (trace) LOG("[vk] quit: stopping the render thread\n");
    rq_stop();
    if (trace) LOG("[vk] quit: stopping the recorder\n");
    rec_quit();  // (what it still had is dropped, not run: no submission or present after this; the queue idle for the wait)
    if (!g_vk.dev) return;
    extern void vk_headstart_finish(void);
    // A fatal build worker must not wait for its own unfinished preparation job.
    if (SDL_IsMainThread()) vk_headstart_finish();
    if (trace) LOG("[vk] quit: waiting for the GPU\n");
    vkDeviceWaitIdle(g_vk.dev);
    if (trace) LOG("[vk] quit: saving the pipeline cache\n");
    vk_pipeline_cache_save();
}

// exit() from any thread: backend_quit (when the window close didn't already), then end the process without the DLL
// teardown (driver / validation-layer threads can stall it for minutes while the game's threads still run).
static void quick_exit_handler(void) {
    backend_quit();
    if (validating) validation_exit();
    fflush(stdout);
    fflush(stderr);
#if defined(_WIN32) || defined(__linux__)
    plat_terminate();
#endif
}

void ui_window_init(void) {
    if (window) return;
    atexit(quick_exit_handler);
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");  // (Back is a key for the menus, not "close the app")
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");        // (touches are handled as such: menus, launcher, controls)
#endif
    settings_load();
    input_apply_background();  // controllers in the background: "Allow background inputs" (Controls tab)
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) FATAL("SDL_Init: %s\n", SDL_GetError());
    input_load_user_mappings();
    int win_w = 1280, win_h = 720;
    if (getenv("LARECOMP_WINDOW")) sscanf(getenv("LARECOMP_WINDOW"), "%dx%d", &win_w, &win_h);
    SDL_WindowFlags wflags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (getenv("LARECOMP_HIDDEN")) wflags |= SDL_WINDOW_HIDDEN;
#ifdef __ANDROID__
    wflags |= SDL_WINDOW_FULLSCREEN;  // (immersive: no status / navigation bars over the game)
#endif
    {   // scripted test runs (user input ignored: runtime/hle/input.c input_user_blocked) open without taking the
        // keyboard focus from whatever the player is doing
        const char *v = getenv("LARECOMP_NO_USER_INPUT");
        bool scripted = v ? atoi(v) != 0 : (getenv("LARECOMP_INPUT") || getenv("LARECOMP_DOORJUMP_TEST") || getenv("LARECOMP_PLACE") ||
                                            getenv("LARECOMP_HOST_INPUT") || getenv("LARECOMP_BIND_TEST"));
        if (scripted) SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    }
    window = SDL_CreateWindow("Raitchi", win_w, win_h, wflags);
    if (!window) FATAL("SDL_CreateWindow: %s\n", SDL_GetError());
    rt_window_display(window);
    SDL_AddEventWatch(lifecycle_watch, NULL);
    rt_window_icon(window);
    // the Fullscreen setting applies from the start screen on (not only once the game runs); test windows stay windows
    if (g_settings.fullscreen && !getenv("LARECOMP_WINDOW") && !getenv("LARECOMP_HIDDEN")) SDL_SetWindowFullscreen(window, true);

    create_instance();
    if (!create_surface()) FATAL("SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
    pick_device();
    create_device();
    create_swapchain();
    vk_exec_init();
    vk_shaders_init();
}

SDL_Window *ui_window(void) { return window; }

void backend_init(void) {
    while (!atomic_load(&ui_ready)) SDL_Delay(1);
    LOG("[vk] %s, swapchain %ux%u\n", g_vk.props.deviceName, g_vk.sc_extent.width, g_vk.sc_extent.height);
}

// ---- in-place reboot (runtime/main.c teardown; main thread, guest threads stopped)
void backend_reset(void) {
    if (!g_vk.dev) return;
    rq_drain();  // (the render thread: everything queued ran; it idles while the game is down)
    rec_drain();  // (the recorder: everything the render thread queued is recorded and submitted)
    vkDeviceWaitIdle(g_vk.dev);
    extern void vk_textures_reset(void);
    extern void vk_exec_reset(void);
    vk_textures_reset();
    vk_exec_reset();
    g_last_presented = NULL;
}
void backend_guest_memory_reset(void) {}  // (exec.c re-imports the heap on first use)
void backend_clear_window(void) {
    if (rq_stopping()) return;
    rq_drain();
    static const float black[4] = {0, 0, 0, 1};
    if (g_vk.dev && vk_frame_begin(black)) vk_frame_end(NULL);
}

int ui_run(void) {
    ui_window_init();
    atomic_store(&ui_ready, true);
    for (;;) {
        if (rt_reboot_requested()) return rt_reboot_requested();  // (runtime/main.c tears the game down)
        SDL_Event e;
        static int log_events = -1;  // LARECOMP_LOG_EVENTS=1: what the window loop gets, every 5 s
        static uint64_t ev_n[0x10000 >> 8], loops, ev_t0;
        if (log_events < 0) log_events = getenv("LARECOMP_LOG_EVENTS") != NULL;
        if (log_events) {
            loops++;
            uint64_t now = SDL_GetTicks();
            if (now - ev_t0 > 5000) {
                LOG("[events] %llu loops in 5 s:", (unsigned long long)loops);
                for (int k = 0; k < (0x10000 >> 8); k++) if (ev_n[k]) LOG(" %#x00:%llu", k, (unsigned long long)ev_n[k]);
                LOG("\n");
                memset(ev_n, 0, sizeof ev_n), loops = 0, ev_t0 = now;
            }
        }
#ifdef __ANDROID__
        // (SDL_WaitEventTimeout spins there: a big core busy all the time for nothing. Touch events are queued by the
        // Java side as they come; looking every 4 ms is quicker than any finger)
        bool any = SDL_PollEvent(&e);
        if (!any) SDL_Delay(4);
        for (bool got = any; got; got = SDL_PollEvent(&e)) {
#else
        while (SDL_WaitEventTimeout(&e, 16)) {
#endif
            if (log_events) ev_n[(e.type >> 8) & 0xff]++;
            input_rumble_poll();
            if (e.type == SDL_EVENT_QUIT) {
                input_rumble_stop();
                input_led_stop();
                LOG("window closed\n");
                return 1;  // Shared main stops/joins guest threads before renderer teardown and ELF finalizers.
            }
            extern bool input_user_blocked(const SDL_Event *e);
            if (input_user_blocked(&e)) continue;  // scripted test runs: the real keyboard / mouse / pads do nothing (as metal.m)
            input_cursor_event(&e);
            if (menu_handle_event(&e)) continue;
            input_handle_event(&e);
        }
        menu_apply_window_requests(window);
        input_rumble_poll();
        input_led_poll();
    }
}
