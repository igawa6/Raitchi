/* Shared NVN host layouts derived from LAS Recomp Android (MIT),
 * cca791a3e7218e1dac95e7d4a11b8f97dba3ea51, Copyright(c)2026 Peiikai.
 * Command-shadow fields derive from EoWRecomp dd9c79d6c01d9eaddafc1fc5a07b6658bfc56505 (MIT). */
// NVN implemented on the host. Guest NVN objects hold a pointer to a host object at offset 0;
// builders and small state objects store their fields directly in guest memory.
#pragma once
#include "../hle.h"
#include "tegra.h"
#include "pool_mapping.h"

#define NVN(name) HLE_FUNC(#name, hle_##name)

// argument helpers
#define A(i) (c->x[(i)])
#define AP(i) ((void *)(uintptr_t)c->x[(i)])
#define AF(i) (*(float *)&c->v[(i)])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define RETF(f) do { c->v[0] = 0; *(float *)&c->v[0] = (f); } while (0)
#define HOST(T, guest) ((T *)(uintptr_t)(*(uint64_t *)(uintptr_t)(guest)))
#define SET_HOST(guest, p) (*(uint64_t *)(uintptr_t)(guest) = (uint64_t)(uintptr_t)(p))
#define G(T, base, off) (*(T *)((uint8_t *)(uintptr_t)(base) + (off)))

typedef struct NvnPool {
    uint64_t storage, size;
    uint32_t flags;
    NvnPoolMappings *mappings;
} NvnPool;
bool nvn_pool_resolve_span(const NvnPool *pool, uint64_t offset, uint64_t size,
    uint64_t *address, uint64_t *generation);

typedef struct NvnTexture {
    TexLayout layout;
    uint32_t target, format, flags, width, height, depth, levels, samples, ds_mode;
    uint8_t swizzle[4];
    uint64_t stride;
    uint64_t addr;  // guest address of texel storage
    // Cross-thread metadata: access only through atomic builtins. CPU writes
    // publish after bytes are copied; render ownership forbids stale CPU uploads.
    uint64_t cpu_write_generation;
    uint32_t gpu_write_owned;
    NvnPool *pool;
    uint64_t pool_offset;
    void *backend;  // renderer-side texture
    uint64_t debug_id;
    uint32_t pool_refs;  // texture pool entries that name it (cleared when it is finalized: runtime/nvn/objects.c)
    char label[64];
} NvnTexture;

typedef struct {
    uint32_t min_filter, mag_filter, wrap[3], compare_mode, compare_func, reduction;
    float lod_min, lod_max, lod_bias, max_aniso;
    float border[4];
    void *backend;
} NvnSamplerDesc;

// Guest NVNtextureView storage is 16 bytes (including the observed Wonder
// stack object). This is our opaque value representation: all TextureView APIs
// are intercepted, and command/descriptor copies retain the same named fields.
// No host pointers, external lifetime or proprietary driver byte layout implied.
typedef struct {
    uint8_t setmask, format, target, ds_mode;
    uint16_t base_level, num_levels, min_layer, num_layers;
    uint8_t swizzle[4];
} NvnTextureView;
#ifdef __cplusplus
static_assert(sizeof(NvnTextureView) == 16, "guest NVNtextureView extent");
#else
_Static_assert(sizeof(NvnTextureView) == 16, "guest NVNtextureView extent");
#endif

// A view naming a texture (pool entry, render target, clear, copy): one in another format is told to the backend before
// the texture is used through it.
void backend_texture_view_format(NvnTexture *tex, uint32_t format);  // (runtime/nvn/metal.m, vulkan/textures.c)
static inline void nvn_texture_note_view(NvnTexture *t, const NvnTextureView *v) {
    if (t && v && (v->setmask & 4) && v->format != t->format) backend_texture_view_format(t, v->format);
}

typedef struct {
    NvnTexture *tex;
    NvnTextureView view;
    bool has_view;
    uint32_t dead_format;  // the texture registered here was finalized (NVN format + 1; 0 = no): tex is NULL
} NvnTexEntry;

// A draw named a texture pool slot whose texture was finalized (runtime/nvn/objects.c): logged, a few times
void nvn_dead_slot_sampled(const NvnTexEntry *e, uint32_t slot);

typedef struct NvnDescPool {
    uint64_t addr;
    int count;
    void *entries;  // NvnTexEntry[] or NvnSamplerDesc[]
} NvnDescPool;

typedef struct NvnShaderStage {
    uint32_t stage;           // NVNshaderStage
    uint64_t code_addr;       // GPU (== CPU) address of the code
    const uint8_t *control;   // control section
    uint32_t const_offset, const_size;  // compiler constants (bound as c1), within the code blob
    void *backend;
} NvnShaderStage;

typedef struct NvnProgram {
    NvnShaderStage stages[6];
    uint32_t stage_mask;
    void *backend;
} NvnProgram;

typedef struct NvnWindow {
    uint64_t textures[8];  // guest NVNtexture*
    int num, active, interval, next;
    int crop[4];
} NvnWindow;

typedef struct NvnSync {
    volatile int signaled;
    volatile uint64_t seq;  // GPU submission this fence belongs to (renderer_fence_seq), 0 = none
} NvnSync;

// ---- command recording (host shadow of guest command chunks)
typedef struct {
    uint64_t mem;    // current command memory chunk (guest address)
    uint64_t size;   // API-requested command capacity (may exceed backed tail)
    uint64_t guest_capacity; // usable budget, bounded by actual pool tail
    uint64_t used;   // host-record bytes used in the current shadow
    uint64_t guest_used; // independent word-aligned HLE command budget; resets only on Add
    uint64_t host, hcap; // current host shadow and capacity; guest capacity remains size
    uint64_t start;  // first record of the list being recorded
    uint64_t callback, callback_data;
    bool recording;
    bool fences;     // the list being recorded has a fence sync or calls lists (marked in its first record)
    uint64_t guest;  // the guest NVNcommandBuffer*
    bool deferred;  // direct deferred binds or calls needing a nested scan
    void *experimental; // opt-in command recording state; existing early offsets preserved
    // Conservative lower bound of every guest command-storage span in the
    // active recording. Reset only by Begin; successful Adds can only lower it.
    uint64_t recording_guest_floor;
} NvnCmdBuf;
// Only after all guest threads and queued GPU/list consumers have stopped.
void nvn_command_shadow_finish_after_gpu_idle(void);
// A list's first record (CMD_BARRIER) has x[6] == CMD_LIST_FENCES when the list has fences or calls other lists: the
// render thread (rqueue.c prescan) looks through only those.
#define CMD_LIST_FENCES 0x46454e4345534cull
#define CMD_LIST_DEFERRED 0x4445464552524544ull
bool nvn_command_has_deferred(uint64_t handle);

// One recorded command: header, the call's integer and float arguments, then copied data.
typedef struct {
    uint32_t op, size;  // size includes the header, multiple of 8
    uint64_t x[7];      // x1..x7 of the call
    float f[4];         // s0..s3 of the call
    uint8_t extra[];
} CmdRec;

enum {
    CMD_END = 0, CMD_JUMP, CMD_CALL,
    CMD_BIND_PROGRAM, CMD_BIND_BLEND, CMD_BIND_CHANNEL_MASK, CMD_BIND_COLOR, CMD_BIND_DEPTH_STENCIL,
    CMD_BIND_MULTISAMPLE, CMD_BIND_POLYGON, CMD_BIND_VERTEX_ATTRIB, CMD_BIND_VERTEX_STREAM,
    CMD_BIND_VERTEX_BUFFERS, CMD_BIND_UNIFORM_BUFFERS, CMD_BIND_STORAGE_BUFFERS, CMD_BIND_TEXTURES,
    CMD_BIND_IMAGES, CMD_SET_TEXTURE_POOL, CMD_SET_SAMPLER_POOL, CMD_SET_RENDER_TARGETS,
    CMD_SET_VIEWPORTS, CMD_SET_SCISSORS, CMD_SET_DEPTH_RANGES, CMD_SET_BLEND_COLOR, CMD_SET_STENCIL,
    CMD_SET_PRIMITIVE_RESTART, CMD_SET_ALPHA_REF, CMD_SET_DEPTH_BOUNDS, CMD_SET_POLYGON_OFFSET,
    CMD_SET_LINE_WIDTH, CMD_SET_POINT_SIZE, CMD_SET_SAMPLE_MASK, CMD_SET_RENDER_ENABLE,
    CMD_CLEAR_COLOR, CMD_CLEAR_DEPTH_STENCIL, CMD_CLEAR_TEXTURE, CMD_CLEAR_BUFFER,
    CMD_DRAW_ARRAYS, CMD_DRAW_ELEMENTS, CMD_DRAW_ARRAYS_INDIRECT, CMD_DRAW_ELEMENTS_INDIRECT,
    CMD_DISPATCH, CMD_DISPATCH_INDIRECT,
    CMD_COPY_BUFFER_TO_BUFFER, CMD_COPY_BUFFER_TO_TEXTURE, CMD_COPY_TEXTURE_TO_BUFFER,
    CMD_COPY_TEXTURE_TO_TEXTURE, CMD_DOWNSAMPLE, CMD_UPDATE_UNIFORM_BUFFER, CMD_FENCE_SYNC, CMD_WAIT_SYNC,
    CMD_BARRIER, CMD_SIGNAL_EVENT, CMD_WAIT_EVENT, CMD_REPORT_COUNTER, CMD_DISCARD_COLOR,
    CMD_DISCARD_DEPTH_STENCIL, CMD_SET_COPY_STRIDES, CMD_DRAW_TEXTURE, CMD_RESOLVE_DEPTH,
    // Deferred binds retain a guest array pointer in x[3], read at replay;
    // x[0..2] are stage/first/count, with no inline payload.
    CMD_BIND_TEXTURES_DEFERRED, CMD_BIND_UNIFORM_BUFFERS_DEFERRED, CMD_BIND_STORAGE_BUFFERS_DEFERRED,
    CMD_COUNT
};



// renderer backend (runtime/nvn/metal.m)
void backend_init(void);
void backend_execute(uint64_t handle);            // run a recorded command list
void backend_present(NvnTexture *tex);            // show a texture in the window
void backend_texture_destroyed(NvnTexture *tex);
void backend_pool_written(uint64_t addr, uint64_t size);
