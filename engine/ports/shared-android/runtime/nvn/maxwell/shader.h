/* Shared runtime overlay derived from LAS Recomp Android at
 * cca791a3e7218e1dac95e7d4a11b8f97dba3ea51, Copyright (c) 2026 Peiikai.
 * Android uint32/read-only snapshot adaptation also derives from Let's Go Recomp
 * 96894fd8e9ea20c1b38cdea53105ec2a335df620, Copyright (c) 2026 raulzito12345rrs.
 * MIT notices: native/runtime/LICENSE-LASRecomp and LICENSE-LetsGo. */
// Maxwell (GM20B) shader translation to Metal Shading Language (macOS) or Vulkan GLSL (Windows; compiled to
// SPIR-V at runtime with shaderc). ShaderKey.glsl selects the dialect; the MSL output does not depend on it.
//
// GLSL contract (what the Vulkan renderer binds):
//  - `#version 460`, entry point `main`. Descriptor set: vertex 0, fragment 1, compute 0.
//  - hardware cbuf k -> binding k: `layout(std140, set=S, binding=k) uniform CB<k> { uvec4 cb<k>[N]; };`
//    (anonymous block). N = max(cbuf_size[k] / 16, highest constant-index vec4 + 1), and at least 4096 when
//    cbuf_dynamic has bit k (the renderer binds up to 64 KB). Register-indexed reads are clamped to N - 1.
//  - global memory (LDG/STG) -> binding GMEM_BUFFER_INDEX (17):
//    `layout(std430, set=S, binding=17) [readonly] buffer GMem { uint w[]; } gmem_;`, 32-bit words relative to
//    maxwell_gmem_base. `readonly` unless the shader stores (needs vertexPipeline/fragmentStoresAndAtomics then);
//    byte / short stores are atomicAnd + atomicOr on the containing word.
//  - rscale table -> binding 18: `uniform RScale { vec4 rscale[65]; };` (std140, the MSL buffer 18 contents).
//  - per-draw tint -> binding 19: `uniform Tint { vec4 ltint[4]; };` (fragment, when uses_tint).
//  - texture slot i (ShaderInfo.tex[i]) -> binding 32 + i, a combined image sampler: sampler1D/2D/3D/Cube/
//    1DArray/2DArray/CubeArray, or its *Shadow variant when tex[i].shadow.
//  - vertex inputs `layout(location=i) in vec4|ivec4|uvec4 a<i>;` (key->attr_kind); varyings: location i =
//    generic attribute i, `vec4 v<i>` (fragment side flat / noperspective as in MSL); `invariant gl_Position`;
//    gl_PointSize when writes_point_size. Fragment outputs `layout(location=rt) out vec4|ivec4|uvec4 c<rt>;`,
//    gl_FragDepth when writes_depth.
//  - clip space exactly as the MSL output (key->flip_y / depth_minus_one epilogue unchanged): the renderer uses
//    a negative-height viewport so Vulkan matches Metal's y-up NDC, clip z in [0, 1].
//  - compute: `layout(local_size_x_id = 0, local_size_y_id = 1, local_size_z_id = 2) in;` (spec constants 0..2).
//  - extensions: GL_EXT_shader_explicit_arithmetic_types_int64 (64-bit carry / address math -> shaderInt64),
//    GL_KHR_shader_subgroup_basic/_shuffle/_shuffle_relative (SHFL, FSWZADD lane: fragment/compute subgroup
//    support), GL_EXT_texture_shadow_lod (explicit-LOD array / cube shadow lookups).
//  - not in GLSL: the maxwell_dbg_vs position dump (buffer 29) is MSL only.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { STAGE_VERTEX = 0, STAGE_FRAGMENT = 1, STAGE_GEOMETRY = 2, STAGE_TESS_CTRL = 3, STAGE_TESS_EVAL = 4,
       STAGE_COMPUTE = 5 };

enum { TEXTYPE_1D, TEXTYPE_2D, TEXTYPE_3D, TEXTYPE_CUBE, TEXTYPE_2D_ARRAY, TEXTYPE_CUBE_ARRAY, TEXTYPE_1D_ARRAY,
       TEXTYPE_BUFFER, TEXTYPE_2D_MS };

typedef struct {
    uint8_t attr_kind[32];  // vertex inputs: 0 float, 1 signed int, 2 unsigned int
    uint8_t rt_kind[8];     // fragment outputs: 0 float, 1 signed int, 2 unsigned int
    bool depth_minus_one;   // NVN depth mode [-1,1]: remap clip z
    bool flip_y;            // lower-left window origin
    bool flip_x;            // mirror the image left to right (mirror mode's scene composite). (In the padding
                            // after flip_y: keys without it hash as before - shader caches stay valid.)
    uint32_t vs_out_mask;   // fragment: varyings the vertex shader provides
    uint8_t glsl;           // emit Vulkan GLSL (see the contract above) instead of MSL
    uint32_t tex_uint;      // bound handle slots (offset 0x20 + 8*i): UINT sampled views, including stencil
} ShaderKey;

typedef struct {
    uint32_t handle_offset;  // byte offset of the texture handle in the bound texture cbuf
    uint8_t type;            // TEXTYPE_*
    bool shadow, is_array, is_image, bindless, is_uint;
    bool legacy_stencil_compare; // comparison retains legacy depth sampling; accuracy unqualified
    uint32_t bindless_cbuf, bindless_offset; // proven handle source, raw Maxwell cbuf index/byteoffset;
                                            // applies to sampled TEX.B and images, not normalized NVN UBO slot
} ShaderTexture;

enum { SHADER_FLOW_STRAIGHT, SHADER_FLOW_NESTED, SHADER_FLOW_FLAGS, SHADER_FLOW_PC };

typedef struct {
    int stage;
    char *msl;              // complete MSL or GLSL source (MSL entry "main0", GLSL "main"); NULL on failure
    uint32_t cbuf_mask;     // hardware constant buffers read
    uint32_t cbuf_size[18];
    uint32_t cbuf_dynamic;  // cbufs indexed with a register: bind the whole buffer
    int ntex;
    ShaderTexture tex[64];
    uint32_t attr_mask;     // vertex: input attributes; fragment: input varyings
    uint32_t out_mask;      // vertex: output varyings; fragment: render targets written
    bool writes_depth, uses_discard, writes_point_size;
    uint8_t clip_mask;      // vertex: declared and written user clip distances (0..7), Vulkan GLSL
    bool uses_gmem;         // global memory access: bind the guest heap at GMEM_BUFFER_INDEX
    bool uses_gmem_map;     // (GLSL compute only) gmem loads look their address up in GMap (uniform, binding
                            // GMAP_BUFFER_INDEX) and read GFast (storage, GFAST_BUFFER_INDEX) - a device-local copy of the
                            // dispatch's storage buffers - when found, stores too (copied back after the dispatch).
                            // runtime/nvn/vulkan/exec.c
    bool uses_gmem_readonly; // capability-selected uint32 bound-range snapshot, UBO21/SSBO17
    bool uses_gmem_store;   // writes global memory (STG)
    bool uses_tint;        // fragment: reads the per-draw tint (buffer 19, float4[4]) applied to color target 0
    uint16_t ssbo_read;     // storage buffer slots whose binding (address) the shader reads from the driver constants
                            // (c0 0x110 + 16 * i vertex, 0x510 + 16 * i fragment): the slots it actually uses
    bool uses_rscale;       // reads the resolution-scale table (fragment/compute buffer 18): float4 [0] = render
                            // target scale, [1 + i] = allocation scale of texture i
    uint32_t num_insts;
    bool structured;  // GLSL: branches rebuilt as structured code (translate.c structure_body), not the pc state machine
    // Host-only diagnostics for successfully translated GLSL; never part of a shader key.
    uint8_t flow_kind;
    bool stacks_resolved, static_stacks;
    uint16_t local_words; // modeled capacity; sparse host storage may be smaller
    uint16_t register_count, half_register_count, half_conversion_count;
    uint32_t branch_labels, brx_count, brx_tables;
    char error[256];        // set when translation fails
    /* A direct backend supplies owned SPIR-V instead of source text. These
     * process-local fields are never serialized in shader keys or recipes. */
    uint32_t *spirv;
    size_t spirv_words;
    bool uses_vertex_id;
} ShaderInfo;

// Global memory (LDG/STG) is addressed through one buffer that wraps the guest heap.
#define GMEM_BUFFER_INDEX 17
#define GRO_MAP_BUFFER_INDEX 21 // read-only mode does not declare the writable GFast binding21
// int64 controls address arithmetic, independently of mirror mutability. GPUs
// without it can use the writable mirror with uint32 offsets when64MiB fits.
// Smaller storage limits select the bounded read-only snapshot implementation.
extern int maxwell_shader_int64, maxwell_gmem_readonly;
// (MSL) Metal's sampler slots a stage. A shader with more textures takes its samplers as an array of all the slots
// and the slot of each texture from a table of uints at SMAP_BUFFER_INDEX (fragment, compute; [i] = texture i's slot).
#define MSL_MAX_SAMPLERS 16
#define SMAP_BUFFER_INDEX 22
// (GLSL compute, Vulkan) the fast copy of a dispatch's storage buffers: its address table and its data
#define GMAP_BUFFER_INDEX 20
#define GFAST_BUFFER_INDEX 21
#define GMAP_MAX 32  // ranges (GMap { uvec4 gmap[GMAP_MAX + 1]; }): [i] = heap byte offset, byte size, buffer word offset, unused;
                     // [GMAP_MAX].x = count)
extern uint64_t maxwell_gmem_base, maxwell_gmem_size;
// 0: no subgroup shuffles in fragment shaders (Adreno: compute only); quad shuffles emulated with derivatives
extern int maxwell_frag_subgroups;
// BASIC+BALLOT support and subgroupSize32..128 divisible by32; bit(1u<<STAGE_*).
extern uint32_t maxwell_vote_stages;
// Actual enabled Vulkan shaderStorageImageWriteWithoutFormat; formatless SUST stores require it.
extern int maxwell_storage_image_write;
extern int maxwell_clip_distance; // actual enabled Vulkan shaderClipDistance capability
// 1: halves converted to float by bits (NVIDIA's driver runs math after unpackHalf2x16 in fp16); 0: unpackHalf2x16
extern int maxwell_h2f_bits;
// 1: no heap import (Vulkan without VK_EXT_external_memory_host): every stage reaches its storage buffers through the
// address table (GMap / GFast), not only compute (runtime/nvn/vulkan/exec.c mirror_table)
extern int maxwell_gmem_mirror;
// Phone (mirror): the heap binding is a zero buffer (exec.c gmem_info), so a load that misses the table reads 0 and the
// shader skips the heap path (maxwell_gmem_zero); the table's ranges are disjoint (mirror_table merges), so a shader keeps
// the last range found and reuses it for the next load inside it (maxwell_gfind_cache). device.c sets both.
extern int maxwell_gmem_zero, maxwell_gfind_cache;
// 1: half-precision instructions as native fp16 in GLSL (GL_EXT_shader_explicit_arithmetic_types_float16; the device
// enables shaderFloat16)
extern int maxwell_half_native;
// 1: GLSL vertex positions precise as well as invariant (0 on phones: invariant alone; translate.c)
extern int maxwell_precise_pos;
// 1: run-time indexed cbufs as word arrays (scalar block layout; translate.c scalar_cbuf_rewrite)
extern int maxwell_scalar_cbuf;
// 1: the device's min / max return the non-NaN operand and clamp(NaN, 0, 1) is 0, as Maxwell's FMNMX and .SAT do
// (device.c probes it): saturate / fmin / fmax in GLSL without the isnan selects (translate.c)
extern int maxwell_ieee_minmax;
// 1: the device reads zeros past a constant buffer's end (Vulkan robustBufferAccess): run-time indexed cbuf loads
// without the clamp to the block (translate.c glsl_prelude_cbr_robust)
extern int maxwell_robust_cbuf;
// (GLSL; from mamcjr/LASRecompAndroid, MIT) SSY / PBK / PCNT targets resolved while translating: SYNC / BRK / CONT
// jump to constants and the state machine has no stack arrays (48 dynamically indexed words: registers or scratch on
// a mobile GPU). Shaders where a target isn't the same on every path keep the stacks. LARECOMP_DYN_STACKS=1: off.
extern bool maxwell_static_stacks;
// (GLSL; from mamcjr/LASRecompAndroid, MIT) a BRX's targets read from its jump table in the constants after the
// code, instead of every instruction being a possible target (the labels split straight code into blocks the GPU's
// compiler can't schedule: an SSAO pass at 20 ms instead of 1). LARECOMP_VK_BRX_TABLE=0: off.
extern bool maxwell_brx_table;
// (GLSL / Vulkan) The heap reaches shaders in chunks of 1 << maxwell_gmem_chunk_shift bytes in two cases:
// - windows of one import where a storage buffer can't span the heap (issue #8) and lazy chunks aren't possible (or
//   LARECOMP_VK_GMEM_LAZY=0): one binding per window (GMEM_CHUNK_BINDING + k in set GMEM_CHUNK_SET) and plain
//   branches, each word picking its own window (a wide access over a window's end reads both);
// - chunks imported as the game binds them (AMD APUs, Intel: vulkan/exec.c gmem_lazy; maxwell_gmem_lazy): the shader takes
//   the chunk's device address and size from a table (GChunks, binding 0) and goes through a buffer reference after
//   one bounds check per LDG / STG (an aligned 8 or 16 byte access never crosses a chunk). A chunk not imported has
//   size 0: the access reads 0 / drops the store and counts in GMiss (binding 1), with the lowest word index hit.
//   (A per-access switch over constant descriptors, tried first, took the NVIDIA driver ~40 s to compile for the
//   chain chomps' compute shader with 87 access sites, and Intel ran it as a loop over the wave.)
// 0: one buffer.
extern uint32_t maxwell_gmem_chunk_shift;
extern bool maxwell_gmem_lazy;
// Lazy mode, fragment: a shader without discard / depth output / stores declares early fragment tests (its GMiss
// atomics would turn them off); one with discard / depth output (no stores) counts no misses - the
// runtime imports the bound storage buffers' chunks before the draw. true (LARECOMP_VK_GMEM_FS_MISS=1): every shader
// counts, none declares early tests.
extern bool maxwell_gmem_fs_miss;
// Lazy mode, vertex / fragment: the chunks under the draw's storage buffers (where the heap reads are) come as push
// constants, GDirect { uvec4 gd[GMEM_DIRECT_MAX]; } (x = first word, y = words, 0: unused, z, w = device address), and
// as storage buffers GDir<k> at GMEM_DIRECT_BINDING + k of the chunk set: an access in one is a compare and a plain
// storage buffer load (not a buffer reference: on Intel those miss the bindless surface path and the translator's
// dispatch loop can't hoist them), no table read and no miss path (exec.c gmem_direct_pick). The table after them.
#define GMEM_DIRECT_MAX 4
#define GMEM_DIRECT_BINDING 2  // lazy: GDir<k> at 2 + k (k < GMEM_DIRECT_MAX; unused: a small zero buffer)
#define GMEM_CHUNKS_MAX 256  // chunks at most (HEAP_MAX in 16 MB chunks is 208)
#define GMEM_CHUNK_SET(stage) ((stage) == STAGE_COMPUTE ? 1 : 2)
#define GMEM_CHUNK_BINDING 0  // windows: GMem<k> at binding k; lazy: the GChunks table
#define GMEM_MISS_BINDING 1   // lazy: GMiss { uint n[GMEM_CHUNKS_MAX]; uint w[GMEM_CHUNKS_MAX]; } (accesses, lowest word hit)
static inline int maxwell_gmem_chunks(void) {
    return maxwell_gmem_chunk_shift ? (int)((maxwell_gmem_size + (1ull << maxwell_gmem_chunk_shift) - 1) >> maxwell_gmem_chunk_shift) : 1;
}
// Debug: when translating the shader whose code starts at maxwell_dbg_code, output register
// maxwell_dbg_reg as captured after the instruction at maxwell_dbg_pc (greyscale, all targets).
extern const uint8_t *maxwell_dbg_code;
extern uint32_t maxwell_dbg_pc;
extern int maxwell_dbg_reg;
// Debug: the vertex shader at maxwell_dbg_vs writes each vertex's position to buffer 18 (slot iid*8+vid).
extern const uint8_t *maxwell_dbg_vs;
#define DBG_VS_BUFFER_INDEX 29

// code: shader binary (with the 0x50-byte SPH header for graphics stages)
ShaderInfo *maxwell_translate_sized(const uint8_t *code, size_t max_size, size_t constants_size, int stage, const ShaderKey *key);
ShaderInfo *maxwell_translate(const uint8_t *code, size_t max_size, int stage, const ShaderKey *key);
void maxwell_free(ShaderInfo *s);
// Disassemble for debugging (writes text to f).
void maxwell_disassemble(const uint8_t *code, size_t max_size, int stage, void *file);
