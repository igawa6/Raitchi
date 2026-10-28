/* Shared runtime overlay derived from LAS Recomp Android at
 * cca791a3e7218e1dac95e7d4a11b8f97dba3ea51, Copyright (c) 2026 Peiikai.
 * Android uint32/read-only snapshot adaptation also derives from Let's Go Recomp
 * 96894fd8e9ea20c1b38cdea53105ec2a335df620, Copyright (c) 2026 raulzito12345rrs.
 * SDK16.2 compute header / BFE.BREV / FLO / LEA.HI / HSET(P) immediate / VOTE / RED from EoWRecomp
 * dd9c79d6c01d9eaddafc1fc5a07b6658bfc56505, Copyright (c) 2026 Peiikai and EoWRecomp contributors.
 * MIT notices: native/runtime/LICENSE-LASRecomp, LICENSE-LetsGo and LICENSE-Echoes. */
// Maxwell (SM 5.x) shader binary -> Metal Shading Language, or Vulkan GLSL (key->glsl; contract in shader.h).
//
// The binary is an NVN header (0x30), the shader program header (SPH, 0x50; graphics stages), then
// instructions in groups of four 64-bit words (one scheduling word + three instructions).
// Registers are emitted as `uint` variables; float ops use as_type<> conversions. Shaders with
// branches become a `switch (pc)` state machine with explicit SSY/PBK/PCNT stacks.
//
// GLSL: the instruction body is generated in the MSL spelling and converted by glsl_rewrite() (a token-aware
// pass: as_type<T>() -> as_u/as_f overloads, float4 -> vec4, ulong -> uint64_t, char()/short() casts -> sx8/sx16,
// in.x / out.x stage I/O -> GLSL globals and built-ins), helpers with MSL names (saturate, fmin, clz, mulhi, ...)
// are defined in the GLSL prelude, and the structurally different constructs (texture lookups, global memory,
// half-float packing, subgroup shuffles, barriers, discard, declarations and entry point) are emitted per dialect.
// The MSL text is byte-identical to the MSL-only translator (tools/test_translate.c checks it).
#if defined(_WIN32)
#include <malloc.h>  // alloca
#endif
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "shader.h"
#include "../../common_session.h"

uint64_t maxwell_gmem_base, maxwell_gmem_size;
int maxwell_frag_subgroups = 1;
// Populated from actual Vulkan BASIC/BALLOT stages and a subgroup size divisible by32.
uint32_t maxwell_vote_stages;
int maxwell_storage_image_write;
int maxwell_clip_distance;
int maxwell_h2f_bits = 1;
int maxwell_gmem_mirror;
int maxwell_shader_int64 = 1;
int maxwell_gmem_readonly;
int maxwell_gmem_zero, maxwell_gfind_cache;  // (phone: shader.h)
int maxwell_half_native;
int maxwell_precise_pos = 1;
int maxwell_scalar_cbuf;
int maxwell_ieee_minmax;
int maxwell_robust_cbuf;
// (both were turned off on 2026-10-18 as a precaution while the phone's restarts were looked into; those were frame
// generation's depth textureGather (08bbaef). Back on with brx_table reading only the table itself: the Field 24.66 ->
// 24.45 ms of GPU, S20 FE. LARECOMP_DYN_STACKS=1 / LARECOMP_VK_BRX_TABLE=0: off)
bool maxwell_static_stacks = true;
bool maxwell_brx_table = true;
uint32_t maxwell_gmem_chunk_shift;  // 0: one buffer for the whole heap (GLSL; shader.h)
bool maxwell_gmem_lazy;
bool maxwell_gmem_fs_miss;
const uint8_t *maxwell_dbg_code;
uint32_t maxwell_dbg_pc;
int maxwell_dbg_reg;
const uint8_t *maxwell_dbg_vs;

#define RZ 255
#define LMEM_WORDS 256  // local memory per invocation (1 KB)
#define LMEM_SCALAR_MAX 16  // larger possible store footprints retain the array
#define PT 7

typedef struct {
    char *buf;
    size_t len, cap;
} Str;

static void sp(Str *s, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (s->len + n + 1 > s->cap) {
        s->cap = (s->len + n + 1) * 2;
        s->buf = realloc(s->buf, s->cap);
    }
    vsnprintf(s->buf + s->len, n + 1, fmt, ap2);
    va_end(ap2);
    s->len += n;
}

// The per-draw tint's recolour mode (ltint[0].w < 0; runtime/nvn/recolour.c, Cosmetics tab): an HSV change of the
// output colour, in its gamma encoding: for colours whose hue is within ltint[1].y of ltint[1].x (>= 0.5: any) and
// saturation >= ltint[1].z, the hue goes to ltint[1].x + ltint[0].x + (its distance from ltint[1].x) * ltint[1].w
// (turns: .w = 1 a plain shift, < 1 also pulls the hues together), saturation * .y, value * .z (kept <= 1 unless it
// was above). The shading stays: only the colour moves. ltint[2] / [3] (ltint[2].w < 0): a second such change with
// its own hue window, made from the same colour (a draw blending two recoloured maps, e.g. grass and sand: each keeps
// its own; where both windows take a colour, the two results are averaged by weight). `c` is the colour target's
// expression ("c0" / "o.c0"), v3 / v4 the vector types ("vec3" / "float3").
static void emit_recolour_window(Str *s, const char *v3, const char *v4, int k, const char *r, const char *m) {
    // (rc_hh0 / rc_ss0 / rc_vv0: the colour's HSV) -> r: the changed colour (linear), m: its weight
    sp(s, "    float rc_sd%d = fract(rc_hh0 - ltint[%d].x + 0.5f) - 0.5f, rc_dh%d = abs(rc_sd%d);\n", k, k + 1, k, k);
    sp(s, "    float %s = (ltint[%d].y >= 0.5f ? 1.0f : 1.0f - smoothstep(ltint[%d].y * 0.6f, ltint[%d].y, rc_dh%d)) *\n"
          "              smoothstep(ltint[%d].z * 0.5f, ltint[%d].z + 1e-4f, rc_ss0);\n", m, k + 1, k + 1, k + 1, k, k + 1, k + 1);
    sp(s, "    float rc_hh%d = fract(ltint[%d].x + ltint[%d].x + rc_sd%d * ltint[%d].w), rc_ss%d = clamp(rc_ss0 * ltint[%d].y, 0.0f, 1.0f);\n",
       k + 1, k + 1, k, k, k + 1, k + 1, k);
    sp(s, "    float rc_vv%d = min(rc_vv0 * ltint[%d].z, max(rc_vv0, 1.0f));\n", k + 1, k);
    sp(s, "    %s rc_k%d = clamp(abs(fract(%s(rc_hh%d) + %s(1.0f, 2.0f / 3.0f, 1.0f / 3.0f)) * 6.0f - %s(3.0f)) - %s(1.0f), "
          "%s(0.0f), %s(1.0f));\n", v3, k, v3, k + 1, v3, v3, v3, v3, v3);
    sp(s, "    %s %s = pow(rc_vv%d * mix(%s(1.0f), rc_k%d, %s(rc_ss%d)), %s(2.2f));\n", v3, r, k + 1, v3, k, v3, k + 1, v3);
    (void)v4;
}

static void emit_recolour(Str *s, const char *c, const char *v3, const char *v4) {
    sp(s, "  else if (ltint[0].w < 0.0f) {\n");
    sp(s, "    %s rc_g = pow(max(%s.rgb, %s(0.0f)), %s(1.0f / 2.2f));\n", v3, c, v3, v3);
    sp(s, "    %s rc_K = %s(0.0f, -1.0f / 3.0f, 2.0f / 3.0f, -1.0f);\n", v4, v4);
    sp(s, "    %s rc_p = mix(%s(rc_g.b, rc_g.g, rc_K.w, rc_K.z), %s(rc_g.g, rc_g.b, rc_K.x, rc_K.y), %s(step(rc_g.b, rc_g.g)));\n", v4, v4, v4, v4);
    sp(s, "    %s rc_q = mix(%s(rc_p.x, rc_p.y, rc_p.w, rc_g.r), %s(rc_g.r, rc_p.y, rc_p.z, rc_p.x), %s(step(rc_p.x, rc_g.r)));\n", v4, v4, v4, v4);
    sp(s, "    float rc_d = rc_q.x - min(rc_q.w, rc_q.y);\n");
    sp(s, "    float rc_hh0 = abs(rc_q.z + (rc_q.w - rc_q.y) / (6.0f * rc_d + 1e-6f)), rc_ss0 = rc_d / (rc_q.x + 1e-6f), rc_vv0 = rc_q.x;\n");
    emit_recolour_window(s, v3, v4, 0, "rc_r", "rc_m");
    sp(s, "    if (ltint[2].w < 0.0f) {\n");
    emit_recolour_window(s, v3, v4, 2, "rc_r2", "rc_m2");
    sp(s, "      float rc_w = rc_m + rc_m2;\n");
    sp(s, "      if (rc_w > 0.0f) { rc_r = (rc_r * rc_m + rc_r2 * rc_m2) / rc_w; rc_m = min(rc_w, 1.0f); }\n    }\n");
    sp(s, "    %s.rgb = mix(%s.rgb, rc_r, %s(rc_m));\n  }\n", c, c, v3);
}

typedef struct {
    const uint8_t *code;   // first instruction group
    size_t ninsts;         // number of 8-byte words to scan
    int stage;
    const ShaderKey *key;
    ShaderInfo *info;
    Str body;
    uint8_t reg_used[256];
    uint8_t pred_used[8];
    uint32_t in_attr_mask, out_attr_mask;
    uint8_t clip_declared; // vertex SPH output map: only enabled shader outputs may clip
    uint32_t vs_in_comp[32];
    bool uses_branches, uses_cc, uses_local, uses_vid, uses_iid, uses_frag_coord, uses_front_facing, uses_lane;
    bool uses_rscale;  // resolution-scale table (fragment/compute only)
    uint32_t flat_mask;           // fragment inputs using constant (flat) interpolation
    uint32_t noperspective_mask;  // fragment inputs using pass (linear) interpolation
    uint32_t omap_target, omap_depth;
    int tex_slot[8192];           // texture index -> Metal slot + 1
    bool tex_weak[64];            // slot type only guessed (from a size query): the next sample decides
    bool fail;
    bool branchy;  // program uses the pc state machine
    int ipa_mode;  // interpolation mode of the IPA being translated (-1 for plain loads)
    int ipa_mul;   // its multiplier register (mode 1: the result is the "pass" value attr/w times this register)
    // GLSL (fields kept last: the MSL path's layout, including its out-of-range reg_used writes, is unchanged)
    bool glsl;               // emit Vulkan GLSL: dialect-specific constructs here, the rest via glsl_rewrite()
    bool uses_gmem_store;    // STG / RED present: the gmem buffer is not readonly
    bool uses_vote, uses_red;
    // Conservative straight-block provenance: nonzero = ((Maxwell cbuf +1)<<16)|byteoffset.
    uint32_t image_origin[256], image_origin_new;
    int image_origin_dst;
    uint32_t cbuf_need[18];  // highest constant vec4 index + 1 per cbuf (array size of the GLSL block)
    // GLSL: how many low bits of each register are known to be zero (0: nothing known, 32: the register is zero), kept
    // through the integer instructions that preserve them (tz_result). A constant-buffer load from a register that is
    // a multiple of 16 is then one load of a constant word of a constant row (cbuf_ref_aligned): on Adreno one ldc
    // instead of a vec4 ldc and a chain of selects for the word (nir_lower_ubo_vec4, unaligned path).
    uint8_t reg_tz[256];
    int tz_dst, tz_val;  // the instruction being translated: its tracked result, or tz_dst -1 (tz_after_insn)
    // GLSL, native fp16: the half-precision instructions keep their results in f16vec2 shadows h<r> of the registers
    // and read them there (hsrc / hstore); the register's uint is packed from the shadow only when a 32-bit read
    // needs it (reg). hstate: 0 the uint is current, 1 the shadow is (uint stale), 2 both. Every block entry and every
    // predicated instruction sees the uints current (hflush_all, hreset_all: maxwell_translate, emit_insn).
    uint8_t hstate[256];
    bool huse[256];
    // hc<r>: the register's f32 value as a half (hx(r, 1u)), kept while the register stands: the lit shaders convert a
    // pixel's inputs again for every light (hcstate 1: current; any write to r<r> or h<r> drops it)
    uint8_t hcstate[256];
    bool hcuse[256];
    bool in_guard;  // translating a predicated instruction: nothing may change state inside it
    // an XMAD of a's low half by an immediate (cfull, C = RZ) waiting for the XMAD.PSL of the high half that completes
    // the 32-bit product (emit_insn: one multiply instead of the two halves' expansions)
    uint32_t xmad_lo_pc;  // its pc + 1, or 0
    bool xmad_new;        // this instruction is one (registered after its writes are scanned)
    int xmad_a, xmad_d;
    uint32_t xmad_imm;
    // (from mamcjr/LASRecompAndroid, MIT) maxwell_static_stacks: the SSY / PBK / PCNT stacks followed at translation
    // (resolve_stacks): flow_to[i] is the target of the SYNC / BRK / CONT at i, and the state machine has no stack
    // arrays. maxwell_brx_table: each BRX's targets read from the compiler's constants after the code (brx_table).
    bool static_stacks;
    int32_t *flow_to;
    int nbrx;
    struct { uint32_t at; int n; uint32_t t[256]; } brx[4];
    bool local_sparse;
    uint8_t local_written[LMEM_WORDS];
    unsigned local_count;
} Ctx;

static bool id_char(char ch);
static bool hshadow_on(Ctx *c) {
    static int on = -1;  // LARECOMP_VK_HALF_SHADOW=0: hx / hpack on the registers, as before (comparisons)
    if (on < 0) on = getenv("LARECOMP_VK_HALF_SHADOW") ? atoi(getenv("LARECOMP_VK_HALF_SHADOW")) != 0 : 1;
    return on && c->glsl && maxwell_half_native;
}
static void hmat(Ctx *c, int r) {  // the 32-bit value of a register whose shadow is the current one
    if (r == RZ || c->hstate[r] != 1) return;
    sp(&c->body, "    r%d = packFloat2x16(h%d);\n", r, r);
    c->hstate[r] = 2;
}
static void hflush_all(Ctx *c) { for (int r = 0; r < 256; r++) hmat(c, r); }
static bool hc_on(void) {  // LARECOMP_VK_HALF_CONV=0: hx(r, 1u) converts each time (comparisons)
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_VK_HALF_CONV") ? atoi(getenv("LARECOMP_VK_HALF_CONV")) != 0 : 1;
    return on;
}
static void hreset_all(Ctx *c) { memset(c->hstate, 0, sizeof c->hstate), memset(c->hcstate, 0, sizeof c->hcstate); }

static uint64_t bits(uint64_t w, int lo, int n) { return (w >> lo) & ((n == 64) ? ~0ull : ((1ull << n) - 1)); }
static int64_t sbits(uint64_t w, int lo, int n) {
    int64_t v = (int64_t)bits(w, lo, n);
    return (v & (1ll << (n - 1))) ? v - (1ll << n) : v;
}

static void fail(Ctx *c, const char *fmt, ...) {
    if (c->fail) return;
    c->fail = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->info->error, sizeof c->info->error, fmt, ap);
    va_end(ap);
}

// Branch targets may point at a scheduling word; execution continues at the next instruction.
static uint32_t norm_target(int64_t t) {
    if (t >= 0 && ((t / 8) % 4) == 0) t += 8;
    return (uint32_t)t;
}

// ------------------------------------------------------------------ operands
static const char *reg(Ctx *c, int r, char *buf) {
    if (r == RZ) return "0u";
    c->reg_used[r] = 1;
    if (c->hstate[r]) hmat(c, r);  // a 32-bit read
    sprintf(buf, "r%d", r);
    return buf;
}
static const char *reg_raw(Ctx *c, int r, char *buf) {  // a half instruction's operand: hsrc picks the shadow or the uint
    if (r == RZ) return "0u";
    c->reg_used[r] = 1;
    sprintf(buf, "r%d", r);
    return buf;
}

#define R(r) reg(c, (r), alloca(16))
#define RH(r) reg_raw(c, (r), alloca(16))
#define F(expr) fmt_f(alloca(512), (expr))
static char *fmt_f(char *b, const char *e) { sprintf(b, "as_type<float>(%s)", e); return b; }

static const char *pred(Ctx *c, int p, bool neg, char *buf) {
    if (p == PT) return neg ? "false" : "true";
    c->pred_used[p] = 1;
    sprintf(buf, "%sp%d", neg ? "!" : "", p);
    return buf;
}
#define P(p, n) pred(c, (p), (n), alloca(16))

static const char *cbuf_ref(Ctx *c, int idx, int64_t off, const char *dyn, char *buf) {
    c->info->cbuf_mask |= 1u << idx;
    if (idx == 0) {  // a storage buffer binding: which slot (any, when indexed with a register)
        if (dyn) c->info->ssbo_read = 0xFFFF;
        else if (off >= 0x110 && off < 0x210) c->info->ssbo_read |= (uint16_t)(1u << ((off - 0x110) >> 4));
        else if (off >= 0x510 && off < 0x610) c->info->ssbo_read |= (uint16_t)(1u << ((off - 0x510) >> 4));
    }
    if (idx < 18 && off + 16 > (int64_t)c->info->cbuf_size[idx]) c->info->cbuf_size[idx] = (uint32_t)((off + 16 + 255) & ~255);
    if (dyn) c->info->cbuf_dynamic |= 1u << idx;
    if (!dyn && idx < 18 && off >= 0 && (uint32_t)(off >> 4) + 1 > c->cbuf_need[idx]) c->cbuf_need[idx] = (uint32_t)(off >> 4) + 1;
    if (dyn) sprintf(buf, "cbr(cb%d, (%s) + %lldu)", idx, dyn, (long long)off);
    else sprintf(buf, "cb%d[%lld][%lld]", idx, (long long)(off >> 4), (long long)((off >> 2) & 3));
    return buf;
}

// LDC from a register known to be a multiple of 16: the row is (reg >> 4) + a constant, the word a constant (cbra, the
// prelude; cbras through scalar_cbuf_rewrite). The prelude masks the sum to 28
// row bits, preserving the 32-bit byte-address wrap of reg + signed offset.
static const char *cbuf_ref_aligned(Ctx *c, int idx, int64_t off, const char *dyn, char *buf) {
    c->info->cbuf_mask |= 1u << idx;
    c->info->cbuf_dynamic |= 1u << idx;
    sprintf(buf, "cbra(cb%d, (%s) >> 4, %lldu, %lldu)", idx, dyn, (long long)(off >> 4), (long long)((off >> 2) & 3));
    return buf;
}

// Operand B in its register / constant buffer / immediate forms, selected by the opcode class.
typedef enum { B_REG, B_CBUF, B_IMM } BForm;

static BForm b_form(uint64_t w) {
    uint64_t top = w >> 60;
    if (top == 0x5) return B_REG;
    if (top == 0x4) return B_CBUF;
    return B_IMM;
}

static const char *operand_b(Ctx *c, uint64_t w, bool float_imm, char *buf) {
    switch (b_form(w)) {
    case B_REG: return reg(c, (int)bits(w, 20, 8), buf);
    case B_CBUF: return cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, buf);
    default: {
        uint32_t v = (uint32_t)bits(w, 20, 19) | ((uint32_t)bits(w, 56, 1) << 19);
        if (float_imm) v <<= 12;
        else if (v & 0x80000u) v |= 0xFFF00000u;  // sign-extend 20 bits
        sprintf(buf, "0x%08xu", v);
        return buf;
    }
    }
}
#define B(fi) operand_b(c, w, (fi), alloca(128))

// Known trailing zero bits (Ctx reg_tz): of a register, of operand B, of the halves XMAD multiplies
static int ctz_u32(uint32_t v) { int n = 0; if (!v) return 32; while (!(v & 1u)) v >>= 1, n++; return n; }
#define TZR(r) ((r) == RZ ? 32 : (int)c->reg_tz[(r)])
#define TZMIN(x, y) ((x) < (y) ? (x) : (y))
#define TZADD(x, y) ((x) + (y) > 32 ? 32 : (x) + (y))
static void tz_result(Ctx *c, int d, int tz) { c->tz_dst = d; c->tz_val = tz; }
static int tz_half(int tz, bool hi) { return hi ? (tz >= 32 ? 32 : tz > 16 ? tz - 16 : 0) : (tz >= 16 ? 32 : tz); }
static int tz_of_b(Ctx *c, uint64_t w) {
    switch (b_form(w)) {
    case B_REG: return TZR((int)bits(w, 20, 8));
    case B_CBUF: return 0;
    default: {
        uint32_t v = (uint32_t)bits(w, 20, 19) | ((uint32_t)bits(w, 56, 1) << 19);
        if (v & 0x80000u) v |= 0xFFF00000u;
        return ctz_u32(v);
    }
    }
}
// After an instruction: every register it wrote (`r<n> = ` in its text; every write is spelled so) knows nothing any
// more, but for the tracked result; guarded, the old value may survive, so the weaker of the two. Its half shadow is
// stale too - unless the line is the shadow being packed into it (hmat).
static void writes_after_insn(Ctx *c, size_t text0, bool guarded) {
    if (!c->body.buf) return;  // (nothing emitted yet)
    for (const char *p = c->body.buf + text0; (p = strstr(p, " = ")) != NULL; p += 3) {
        const char *q = p;
        while (q > c->body.buf + text0 && q[-1] >= '0' && q[-1] <= '9') q--;
        if (q == p || q[-1] != 'r' || (q - 1 > c->body.buf + text0 && id_char(q[-2]))) continue;
        int r = atoi(q);
        if (r >= 256) continue;
        c->image_origin[r] = !guarded && r == c->image_origin_dst ? c->image_origin_new : 0;
        int tz = r == c->tz_dst ? c->tz_val : 0;
        c->reg_tz[r] = (uint8_t)(guarded ? TZMIN((int)c->reg_tz[r], tz) : tz);
        if (strncmp(p + 3, "packFloat2x16(h", 15)) c->hstate[r] = 0, c->hcstate[r] = 0;
        if (!strncmp(p + 3, "floatBitsToUint(float((", 23)) c->hcstate[r] = 0;  // (an F32 half result: a new value)
        if (c->xmad_lo_pc && (r == c->xmad_a || r == c->xmad_d)) c->xmad_lo_pc = 0;  // (the pair's operands changed)
    }
}

// The "C" operand of three-source ops: REG_39, or the cbuf/reg swap form (0x51/0x53/... classes).
static const char *operand_c_for_ffma(Ctx *c, uint64_t w, char *buf, const char **b_out, char *bbuf) {
    uint64_t op8 = w >> 56;
    if ((op8 & 0xF8) == 0x50 && (op8 & 7) != 0) {  // e.g. 0x51: B = REG_39, C = cbuf
        *b_out = reg(c, (int)bits(w, 39, 8), bbuf);
        return cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, buf);
    }
    *b_out = operand_b(c, w, true, bbuf);
    return reg(c, (int)bits(w, 39, 8), buf);
}

static void set_reg(Ctx *c, int r, const char *expr) {
    if (r == RZ) return;
    c->reg_used[r] = 1;
    sp(&c->body, "    r%d = %s;\n", r, expr);
}

static const char *fneg_abs(const char *e, bool neg, bool abs_, char *buf) {
    if (abs_ && neg) sprintf(buf, "(-abs(%s))", e);
    else if (abs_) sprintf(buf, "abs(%s)", e);
    else if (neg) sprintf(buf, "(-%s)", e);
    else return e;
    return buf;
}
#define NA(e, n, a) fneg_abs((e), (n), (a), alloca(600))

static const char *fresult(const char *e, bool sat, char *buf) {
    if (sat) sprintf(buf, "as_type<uint>(saturate(%s))", e);
    else sprintf(buf, "as_type<uint>(%s)", e);
    return buf;
}
#define FRES(e, sat) fresult((e), (sat), alloca(700))

static const char *cmp_op(int op) {
    static const char *const ops[] = {"false", "<", "==", "<=", ">", "!=", ">=", "num", "nan",
                                      "<", "==", "<=", ">", "!=", ">=", "true"};
    return ops[op & 15];
}

// float comparison including unordered variants
static void fcmp_expr(char *out, int op, const char *a, const char *b) {
    switch (op) {
    case 0: strcpy(out, "false"); break;
    case 15: strcpy(out, "true"); break;
    case 7: sprintf(out, "(!isnan(%s) && !isnan(%s))", a, b); break;
    case 8: sprintf(out, "(isnan(%s) || isnan(%s))", a, b); break;
    default:
        if (op < 7) sprintf(out, "(%s %s %s)", a, cmp_op(op), b);
        else sprintf(out, "((%s %s %s) || isnan(%s) || isnan(%s))", a, cmp_op(op), b, a, b);
    }
}

static const char *bool_op(int op) { return op == 0 ? "&&" : op == 1 ? "||" : "!="; }

static void icmp_expr(char *out, int op, const char *a, const char *b, bool is_signed) {
    static const char *const ops[] = {"false", "<", "==", "<=", ">", "!=", ">=", "true"};
    if (op == 0) strcpy(out, "false");
    else if (op == 7) strcpy(out, "true");
    else if (is_signed) sprintf(out, "(int(%s) %s int(%s))", a, ops[op], b);
    else sprintf(out, "(%s %s %s)", a, ops[op], b);
}

// ------------------------------------------------------------------ attributes
// Maxwell attribute space: 0x70 position, 0x80 + 16*i generic attribute i, 0x2F8 instance id,
// 0x2FC vertex id, 0x3FC front facing, 0x6C point size.
static void attr_load(Ctx *c, int dst, uint32_t addr, const char *idx) {
    char e[256];
    if (idx) { fail(c, "indexed attribute load"); return; }
    if (c->stage == STAGE_VERTEX) {
        if (addr >= 0x80 && addr < 0x280) {
            int a = (addr - 0x80) >> 4, comp = (addr >> 2) & 3;
            c->in_attr_mask |= 1u << a;
            c->vs_in_comp[a] |= 1u << comp;
            int kind = c->key ? c->key->attr_kind[a] : 0;
            if (kind == 0) sprintf(e, "as_type<uint>(in.a%d[%d])", a, comp);
            else sprintf(e, "as_type<uint>(in.a%d[%d])", a, comp);
        } else if (addr == 0x2F8) { c->uses_iid = true; strcpy(e, "iid"); }
        else if (addr == 0x2FC) { c->uses_vid = true; strcpy(e, "vid"); }
        else { sprintf(e, "0u /* attr %#x */", addr); }
    } else if (c->stage == STAGE_FRAGMENT) {
        if (addr >= 0x70 && addr < 0x80) {
            c->uses_frag_coord = true;
            int comp = (addr >> 2) & 3;
            if (comp == 0 && c->key && c->key->flip_x) {  // mirror mode's composite: x mirrored (rscale[65].x = width)
                c->uses_rscale = true;
                strcpy(e, "as_type<uint>(rscale[65][0] - (in.pos[0] - rscale[0][2]) / rscale[0][0])");
            } else if (comp < 2) {  // guest pixel units: the render target may be allocated larger
                c->uses_rscale = true;
                sprintf(e, "as_type<uint>((in.pos[%d] - rscale[0][%d]) / rscale[0][%d])", comp, comp + 2, comp);
            } else {
                sprintf(e, "as_type<uint>(in.pos[%d])", comp);
            }
        } else if (addr >= 0x80 && addr < 0x280) {
            int a = (addr - 0x80) >> 4, comp = (addr >> 2) & 3;
            // (MSL: a mask of 0 means unknown. GLSL: the renderer always passes the vertex shader's outputs, and
            // Vulkan requires every fragment input to be a vertex output, so 0 means none.)
            if (c->key && (c->key->vs_out_mask || c->key->glsl) && !(c->key->vs_out_mask & (1u << a))) {
                strcpy(e, comp == 3 ? "0x3f800000u" : "0u");  // not provided by the vertex shader
            } else {
                c->in_attr_mask |= 1u << a;
                // IPA "pass" yields attr/w (the shader multiplies by w itself); Metal gives attr. "multiply" is the
                // pass value times a register - usually w (from the pass of position w: Metal's attr again), but a
                // shader may pass its own (the Mysterious Woods mist: 1/(v4.w * pos.w), a projective screen lookup)
                if (c->ipa_mode == 0) sprintf(e, "as_type<uint>(in.v%d[%d] * in.pos.w)", a, comp);
                else if (c->ipa_mode == 1 && c->ipa_mul != RZ) {
                    char mb[16];
                    sprintf(e, "as_type<uint>(in.v%d[%d] * in.pos.w * as_type<float>(%s))", a, comp, reg(c, c->ipa_mul, mb));
                } else sprintf(e, "as_type<uint>(in.v%d[%d])", a, comp);
            }
        } else if (addr == 0x3FC) { c->uses_front_facing = true; strcpy(e, "(front_facing ? 0xFFFFFFFFu : 0u)"); }
        else if (addr == 0x2E0 || addr == 0x2E4) { sprintf(e, "as_type<uint>(point_coord[%d])", (addr >> 2) & 1); }
        else { sprintf(e, "0u /* attr %#x */", addr); }
    } else {
        sprintf(e, "0u /* attr %#x */", addr);
    }
    set_reg(c, dst, e);
}

static void attr_store(Ctx *c, int src, uint32_t addr) {
    char b[16];
    const char *v = reg(c, src, b);
    if (c->glsl && c->stage != STAGE_VERTEX) {  // no vertex outputs in this stage (MSL: undeclared `out`)
        if (addr >= 0x80 && addr < 0x280) c->out_attr_mask |= 1u << ((addr - 0x80) >> 4);
        if (addr == 0x6C) c->info->writes_point_size = true;
        sp(&c->body, "    /* store attr %#x */\n", addr);
        return;
    }
    if (c->glsl && addr >= 0x2C0 && addr < 0x2E0) {
        unsigned plane = (addr - 0x2C0) / 4;
        if (c->clip_declared & (1u << plane)) {
            if (!maxwell_clip_distance) { fail(c, "clip distance requires shaderClipDistance"); return; }
            c->info->clip_mask |= (uint8_t)(1u << plane);
            sp(&c->body, "    gl_ClipDistance[%u] = as_type<float>(%s);\n", plane, v);
        } else sp(&c->body, "    /* disabled clip distance %u */\n", plane);
    } else if (addr >= 0x70 && addr < 0x80) {
        sp(&c->body, "    out.pos[%d] = as_type<float>(%s);\n", (addr >> 2) & 3, v);
    } else if (addr >= 0x80 && addr < 0x280) {
        int a = (addr - 0x80) >> 4;
        c->out_attr_mask |= 1u << a;
        sp(&c->body, "    out.v%d[%d] = as_type<float>(%s);\n", a, (addr >> 2) & 3, v);
    } else if (addr == 0x6C) {
        c->info->writes_point_size = true;
        sp(&c->body, "    out.psize = as_type<float>(%s);\n", v);
    } else {
        sp(&c->body, "    /* store attr %#x */\n", addr);
    }
}


// ------------------------------------------------------------------ half precision (SM53)
// Swizzle: 0 H1_H0 (both halves), 1 F32 (one f32 used for both lanes), 2 H0_H0, 3 H1_H1.
// Merge: 0 H1_H0 (pack both), 1 F32 (lane 0 as f32), 2 MRG_H0 (replace low half), 3 MRG_H1 (replace high half).
static const char *hsrc(Ctx *c, const char *v, int swz, bool neg, bool abs_, char *buf) {
    char t[300];
    int r = v[0] == 'r' && v[1] >= '0' && v[1] <= '9' ? atoi(v + 1) : -1;  // (a register operand: RH)
    if (r >= 0 && swz != 1 && hshadow_on(c) && (c->hstate[r] || !c->in_guard)) {  // the shadow, made from the uint if need be
        if (!c->hstate[r]) {  // (not under a guard: the other path would miss the unpack)
            sp(&c->body, "    h%d = unpackFloat2x16(r%d);\n", r, r);
            c->hstate[r] = 2;
        }
        c->huse[r] = true;
        sprintf(t, swz == 2 ? "h%d.xx" : swz == 3 ? "h%d.yy" : "h%d", r);
    } else if (r >= 0 && swz == 1 && hshadow_on(c) && hc_on() && (c->hcstate[r] || !c->in_guard)) {  // the f32 as a half, kept
        if (!c->hcstate[r]) {
            hmat(c, r);
            sp(&c->body, "    hc%d = f16vec2(float16_t(uintBitsToFloat(r%d)));\n", r, r);
            c->hcstate[r] = 1;
        }
        c->hcuse[r] = true;
        sprintf(t, "hc%d", r);
    } else {
        if (r >= 0) hmat(c, r);  // the f32 bits (swz 1), or the uint under a guard
        sprintf(t, "hx(%s, %du)", v, swz);
    }
    if (abs_ && neg) sprintf(buf, "(-abs(%s))", t);
    else if (abs_) sprintf(buf, "abs(%s)", t);
    else if (neg) sprintf(buf, "(-%s)", t);
    else strcpy(buf, t);
    return buf;
}
#define HS(v, sw, n, ab) hsrc(c, (v), (sw), (n), (ab), alloca(400))

// Mixed F32/half operands use F32 arithmetic, then round the result to half.
// In particular, an F32 input must not be narrowed before a cancelling add/FMA.
// This is independent of whether native half arithmetic is available.
static const char *hsrc32(Ctx *c, const char *v, int swz, bool neg, bool abs_, char *buf) {
    char t[440];
    if (swz == 1) {
        int r = v[0] == 'r' && v[1] >= '0' && v[1] <= '9' ? atoi(v + 1) : -1;
        if (r >= 0) hmat(c, r);
        snprintf(t, sizeof t, c->glsl ? "vec2(uintBitsToFloat(%s))" : "float2(as_type<float>(%s))", v);
    } else {
        char h[400];
        snprintf(t, sizeof t, c->glsl ? "vec2(%s)" : "float2(%s)", hsrc(c, v, swz, false, false, h));
    }
    if (abs_ && neg) sprintf(buf, "(-abs(%s))", t);
    else if (abs_) sprintf(buf, "abs(%s)", t);
    else if (neg) sprintf(buf, "(-%s)", t);
    else strcpy(buf, t);
    return buf;
}
#define HS32(v, sw, n, ab) hsrc32(c, (v), (sw), (n), (ab), alloca(500))

static void hstore(Ctx *c, int d, const char *expr, int merge, bool sat) {
    if (d != RZ) c->image_origin[d] = 0; // an optimized half-shadow write also changes the architectural GPR
    char t[6144], e[4096];
    if (sat) sprintf(e, "saturate(%s)", expr); else strcpy(e, expr);
    if (d == RZ) return;
    c->reg_used[d] = 1;
    if (hshadow_on(c) && !c->in_guard) {  // into the shadow (merges: one lane of it), the uint only for an f32 result
        c->huse[d] = true;
        if (merge == 1) {
            sp(&c->body, "    r%d = floatBitsToUint(float((%s).x));\n", d, e);
            c->hstate[d] = 0;
            return;
        }
        if (merge == 0) {
            sp(&c->body, "    h%d = %s;\n", d, e);
        } else {
            if (!c->hstate[d]) sp(&c->body, "    h%d = unpackFloat2x16(r%d);\n", d, d);  // (the lane kept)
            sp(&c->body, "    { f16vec2 t_ = %s; h%d.%c = t_.%c; }\n", e, d, merge == 2 ? 'x' : 'y', merge == 2 ? 'x' : 'y');
        }
        c->hstate[d] = 1, c->hcstate[d] = 0;
        return;
    }
    hmat(c, d);  // (hpack reads the uint)
    sprintf(t, "hpack(%s, %du, r%d)", e, merge, d);
    set_reg(c, d, t);
}

static void hstore_mixed(Ctx *c, int d, const char *expr, int merge, bool sat) {
    char rounded[2048];
    const char *begin = maxwell_half_native ? "f16vec2(" : "unpackHalf2x16(packHalf2x16(";
    const char *end = maxwell_half_native ? ")" : "))";
    snprintf(rounded, sizeof rounded, "%s%s%s%s%s", begin, sat ? "saturate(" : "",
             expr, sat ? ")" : "", end);
    hstore(c, d, rounded, merge, false);
}

static void hstore_f32(Ctx *c, int d, const char *expr, bool sat) {
    if (d == RZ) return;
    char result[2048];
    // Do not pass through native hpack: its f16vec2 parameter would narrow the
    // all-F32 result even when shadows are disabled or this instruction is guarded.
    snprintf(result, sizeof result, "floatBitsToUint((%s%s%s).x)",
             sat ? "saturate(" : "", expr, sat ? ")" : "");
    c->image_origin[d] = 0;
    c->hstate[d] = 0; c->hcstate[d] = 0;
    set_reg(c, d, result);
}

// immediate halves: 9 bits of each half's top (the low 6 mantissa bits are zero) plus separate signs
static uint32_t himm(uint64_t w) {
    uint32_t lo = ((uint32_t)bits(w, 20, 9) << 6) | ((uint32_t)bits(w, 29, 1) << 15);
    uint32_t hi = ((uint32_t)bits(w, 30, 9) << 6) | ((uint32_t)bits(w, 56, 1) << 15);
    return lo | (hi << 16);
}

static bool emit_half(Ctx *c, uint64_t w) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8);
    char t[1800], ib[32], cb[256];
    uint16_t op = (uint16_t)(w >> 48);
    // ---- HADD2 / HMUL2 (reg 5d10/5d08, cbuf 7a8x/788x, imm 7a0x/780x, 32I 2c/2a)
    int kind = -1;  // 0 add, 1 mul
    int form = -1;  // 0 reg, 1 cbuf, 2 imm, 3 32i
    if ((op & 0xfff8) == 0x5d10) { kind = 0; form = 0; }
    else if ((op & 0xfff8) == 0x5d08) { kind = 1; form = 0; }
    else if ((op & 0xfe80) == 0x7a80) { kind = 0; form = 1; }
    else if ((op & 0xfe80) == 0x7a00) { kind = 0; form = 2; }
    else if ((op & 0xfe80) == 0x7880) { kind = 1; form = 1; }
    else if ((op & 0xfe80) == 0x7800) { kind = 1; form = 2; }
    else if ((op & 0xfe00) == 0x2c00) { kind = 0; form = 3; }
    else if ((op & 0xfe00) == 0x2a00) { kind = 1; form = 3; }
    if (kind >= 0) {
        int merge, swa, swb = 0;
        bool sat, nega, absa = false, negb = false, absb = false;
        const char *bv;
        if (form == 3) {
            merge = 0; sat = bits(w, 52, 1); swa = (int)bits(w, 53, 2); nega = kind == 0 ? bits(w, 56, 1) : false;
            sprintf(ib, "0x%08xu", (uint32_t)bits(w, 20, 32)); bv = ib;
        } else {
            // field layout per Ryujinx's InstDecoders (InstHadd2*/InstHmul2*)
            merge = (int)bits(w, 49, 2); swa = (int)bits(w, 47, 2); nega = bits(w, 43, 1); absa = bits(w, 44, 1);
            if (form == 0) {
                sat = bits(w, 32, 1); absb = bits(w, 30, 1); swb = (int)bits(w, 28, 2);
                if (kind == 0) negb = bits(w, 31, 1);
                else { nega = bits(w, 31, 1); negb = false; }  // HMUL2 reg: the negate bit is A's at 31
                bv = RH((int)bits(w, 20, 8));
            } else if (form == 1) {
                sat = bits(w, 52, 1); negb = kind == 0 ? bits(w, 56, 1) : false; absb = bits(w, 54, 1); swb = 1;
                bv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
            } else {
                sat = bits(w, 52, 1); swb = 0;
                sprintf(ib, "0x%08xu", himm(w)); bv = ib;
            }
        }
        bool all_f32 = swa == 1 && swb == 1;
        // All-F32 inputs with F32/lane destinations likewise retain their inputs.
        // Packed all-F32 is kept on the existing path: its semantics are not
        // established by the reference used for this bounded correction.
        bool mixed = c->glsl && (swa == 1 || swb == 1) && (!all_f32 || merge != 0);
        sprintf(t, kind == 0 ? "(%s + %s)" : "(%s * %s)",
                mixed ? HS32(RH(a), swa, nega, absa) : HS(RH(a), swa, nega, absa),
                mixed ? HS32(bv, swb, negb, absb) : HS(bv, swb, negb, absb));
        if (mixed && all_f32 && merge == 1) hstore_f32(c, d, t, sat);
        else if (mixed) hstore_mixed(c, d, t, merge, sat);
        else hstore(c, d, t, merge, sat);
        return true;
    }
    // ---- HFMA2 (reg 5d00, cr 70xx|0x80, rc 60xx|0x80, imm 70xx, 32I 28)
    form = -1;
    if ((op & 0xfff8) == 0x5d00) form = 0;
    else if ((op & 0xf880) == 0x7080) form = 1;  // b = cbuf, c = reg
    else if ((op & 0xf880) == 0x6080) form = 2;  // b = reg, c = cbuf
    else if ((op & 0xf880) == 0x7000) form = 3;  // b = imm, c = reg
    else if ((op & 0xfe00) == 0x2800) form = 4;  // 32I: c = d
    if (form >= 0) {
        int merge, swa = (int)bits(w, 47, 2), swb = 0, swc = 0;
        bool sat, negb, negc;
        const char *bv, *cv;
        char cb2[256];
        switch (form) {  // field layout per Ryujinx's InstDecoders (InstHfma2*); NegA is applied to B (same product)
        case 0:
            merge = (int)bits(w, 49, 2); sat = bits(w, 32, 1); negb = bits(w, 31, 1); negc = bits(w, 30, 1);
            swb = (int)bits(w, 28, 2); swc = (int)bits(w, 35, 2);
            bv = RH((int)bits(w, 20, 8)); cv = RH((int)bits(w, 39, 8));
            break;
        case 1:  // b = cbuf, c = reg
            merge = (int)bits(w, 49, 2); sat = bits(w, 52, 1); negb = bits(w, 56, 1); negc = bits(w, 51, 1);
            swb = 1; swc = (int)bits(w, 53, 2);
            bv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb); cv = RH((int)bits(w, 39, 8));
            break;
        case 2:  // b = reg (field at 39), c = cbuf
            merge = (int)bits(w, 49, 2); sat = bits(w, 52, 1); negb = bits(w, 56, 1); negc = bits(w, 51, 1);
            swb = (int)bits(w, 53, 2); swc = 1;
            bv = RH((int)bits(w, 39, 8)); cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb2);
            break;
        case 3:  // b = imm, c = reg
            merge = (int)bits(w, 49, 2); sat = bits(w, 52, 1); negb = false; negc = bits(w, 51, 1);
            swb = 0; swc = (int)bits(w, 53, 2);
            sprintf(ib, "0x%08xu", himm(w)); bv = ib; cv = RH((int)bits(w, 39, 8));
            break;
        default:  // 32I: c = d
            merge = 0; sat = false; negb = false; negc = bits(w, 51, 1);
            swa = (int)bits(w, 47, 2); swb = 0; swc = 0;
            sprintf(ib, "0x%08xu", (uint32_t)bits(w, 20, 32)); bv = ib; cv = RH(d);
            break;
        }
        bool all_f32 = swa == 1 && swb == 1 && swc == 1;
        bool mixed = c->glsl && (swa == 1 || swb == 1 || swc == 1) && (!all_f32 || merge != 0);
        sprintf(t, "fma(%s, %s, %s)",
                mixed ? HS32(RH(a), swa, false, false) : HS(RH(a), swa, false, false),
                mixed ? HS32(bv, swb, negb, false) : HS(bv, swb, negb, false),
                mixed ? HS32(cv, swc, negc, false) : HS(cv, swc, negc, false));
        if (mixed && all_f32 && merge == 1) hstore_f32(c, d, t, sat);
        else if (mixed) hstore_mixed(c, d, t, merge, sat);
        else hstore(c, d, t, merge, sat);
        return true;
    }
    // ---- HSETP2 / HSET2 (register, immediate and constant-buffer forms)
    // Cbuf field/semantic reference: yuzu Emulator Project (2021),
    // GPL-2.0-or-later, half_floating_point_set{,_predicate}.cpp and helper.
    // Independent emission from those instruction semantics; no reference implementation copied.
    bool himm_form = (op & 0xfe80) == 0x7e00 || (op & 0xfe80) == 0x7c00;
    bool hcbuf_form = (op & 0xfe80) == 0x7e80 || (op & 0xfe80) == 0x7c80;
    if ((op & 0xfff8) == 0x5d20 || (op & 0xfff8) == 0x5d18 || himm_form || hcbuf_form) {
        bool setp = (op & 0xfff8) == 0x5d20 || (op & 0xfe80) == 0x7e00 || (op & 0xfe80) == 0x7e80;
        if (hcbuf_form && (bits(w, 34, 5) >= 18 || bits(w, 45, 2) == 3)) {
            fail(c, "unsupported half cbuf binding/boolean operation"); return true;
        }
        // B is an F32 scalar splat: both lanes compare in F32, including when
        // native half arithmetic is enabled. Never narrow either F32 operand.
        const char *x = hcbuf_form ? HS32(RH(a), (int)bits(w, 47, 2), bits(w, 43, 1), bits(w, 44, 1))
                                  : HS(RH(a), (int)bits(w, 47, 2), bits(w, 43, 1), bits(w, 44, 1));
        char hi[32];
        if (himm_form) snprintf(hi, sizeof hi, "0x%08xu", himm(w));
        const char *y = hcbuf_form ? HS32(cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb),
                                        1, bits(w, 56, 1), setp && bits(w, 54, 1))
                         : himm_form ? HS(hi, 0, false, false)
                                 : HS(RH((int)bits(w, 20, 8)), (int)bits(w, 28, 2), bits(w, 31, 1), bits(w, 30, 1));
        int cop = (int)bits(w, himm_form || hcbuf_form ? 49 : 35, 4), bop = (int)bits(w, 45, 2);
        int flag_bit = himm_form || hcbuf_form ? 53 : 49;
        const char *pp = P((int)bits(w, 39, 3), bits(w, 42, 1));
        char lx[512], ly[512];
        sp(&c->body, "    { float2 hx_ = %s, hy_ = %s; bool pp = %s;\n", x, y, pp);
        if (hcbuf_form && bits(w, setp ? 6 : 54, 1)) {
            // FTZ applies after mixed operands widen. Signed zero is equivalent
            // for every comparison here; NaNs and normal/min-normal values stay intact.
            for (unsigned lane = 0; lane < 2; ++lane) {
                char component = lane ? 'y' : 'x';
                sp(&c->body, "      if (abs(hx_.%c) < 1.1754943508222875e-38f) hx_.%c = 0.0f;\n", component, component);
                sp(&c->body, "      if (abs(hy_.%c) < 1.1754943508222875e-38f) hy_.%c = 0.0f;\n", component, component);
            }
        }
        fcmp_expr(lx, cop, "hx_.x", "hy_.x");
        fcmp_expr(ly, cop, "hx_.y", "hy_.y");
        if (hcbuf_form && cop == 5) {
            // Ordered NE excludes NaNs; leave inherited non-cbuf paths unchanged.
            strcpy(lx, "(!isnan(hx_.x) && !isnan(hy_.x) && hx_.x != hy_.x)");
            strcpy(ly, "(!isnan(hx_.y) && !isnan(hy_.y) && hx_.y != hy_.y)");
        }
        sp(&c->body, "      bool l = %s, h = %s;\n", lx, ly);
        if (setp) {
            int pa = (int)bits(w, 3, 3), pb = (int)bits(w, 0, 3);
            if (bits(w, flag_bit, 1)) {
                // Keep the established register-form path; immediate fields/combination follow pinned EOW.
                if (himm_form) sp(&c->body, "      l = l && h; h = !l; l = l %s pp; h = h %s pp;\n", bool_op(bop), bool_op(bop));
                else sp(&c->body, "      l = (l %s pp) && (h %s pp); h = !l;\n", bool_op(bop), bool_op(bop));
                if (pa != PT) { c->pred_used[pa] = 1; sp(&c->body, "      p%d = l;\n", pa); }
                if (pb != PT) { c->pred_used[pb] = 1; sp(&c->body, "      p%d = h;\n", pb); }
            } else {
                if (pa != PT) { c->pred_used[pa] = 1; sp(&c->body, "      p%d = l %s pp;\n", pa, bool_op(bop)); }
                if (pb != PT) { c->pred_used[pb] = 1; sp(&c->body, "      p%d = h %s pp;\n", pb, bool_op(bop)); }
            }
        } else {
            bool bf = bits(w, flag_bit, 1);
            if (d != RZ) {
                c->reg_used[d] = 1;
                sp(&c->body, "      l = l %s pp; h = h %s pp;\n", bool_op(bop), bool_op(bop));
                sp(&c->body, bf ? "      r%d = (l ? 0x3c00u : 0u) | (h ? 0x3c000000u : 0u);\n"
                                : "      r%d = (l ? 0xFFFFu : 0u) | (h ? 0xFFFF0000u : 0u);\n", d);
            }
        }
        sp(&c->body, "    }\n");
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ textures
static int tex_slot(Ctx *c, uint32_t index, int type, bool shadow, bool array) {
    if (index >= 8192) return 0;
    if (c->tex_slot[index]) {
        int s = c->tex_slot[index] - 1;
        if (type >= 0 && c->tex_weak[s]) {
            ShaderTexture *t = &c->info->tex[s];
            t->type = (uint8_t)type;
            t->shadow = shadow;
            t->is_array = array;
            if (t->is_uint && shadow) {
                t->is_uint = false;
                t->legacy_stencil_compare = true;
            }
            c->tex_weak[s] = false;
        } else if (c->glsl && type >= 0 && (c->info->tex[s].type != type || c->info->tex[s].shadow != shadow)) {
            fail(c, "texture %u used as two types", index);  // one GLSL sampler type per binding
        }
        return s;
    }
    bool weak = type < 0;
    if (weak) type = TEXTYPE_2D;
    int s = c->info->ntex++;
    if (s >= 31) { fail(c, "too many textures"); return 0; }
    c->tex_slot[index] = s + 1;
    c->tex_weak[s] = weak;
    ShaderTexture *t = &c->info->tex[s];
    t->handle_offset = index * 4;
    t->type = (uint8_t)type;
    t->shadow = shadow;
    t->is_array = array;
    if (c->glsl && c->key && index >= 8 && !(index & 1) && index < 72)
        t->is_uint = (c->key->tex_uint & (1u << ((index - 8) / 2))) != 0;
    if (t->is_uint && shadow) {
        t->is_uint = false;
        t->legacy_stencil_compare = true;
    }
    return s;
}

// Storage-image SUST fields independently implemented from the MIT nxport decoder
// (3a5d23c67f23346a9bca0a03d4e03b01375090c4), not copied from GPL emulator code.
// One statically identified descriptor per binding; no runtime descriptor indexing assumed.
static int image_slot(Ctx *c, bool bindless, uint32_t cb, uint32_t offset, int type) {
    for (int i = 0; i < c->info->ntex; ++i) {
        ShaderTexture *t = &c->info->tex[i];
        if (!t->is_image || t->bindless != bindless) continue;
        if (bindless ? t->bindless_cbuf != cb || t->bindless_offset != offset : t->handle_offset != offset) continue;
        if (t->type != type) { fail(c, "storage image used with conflicting dimensions"); return 0; }
        return i;
    }
    if (c->info->ntex >= 31) { fail(c, "too many textures/images"); return 0; }
    int i = c->info->ntex++;
    ShaderTexture *t = &c->info->tex[i];
    t->type = (uint8_t)type; t->is_image = true; t->bindless = bindless;
    t->is_array = type == TEXTYPE_1D_ARRAY || type == TEXTYPE_2D_ARRAY;
    if (bindless) { t->bindless_cbuf = cb; t->bindless_offset = offset; }
    else t->handle_offset = offset;
    return i;
}

static void emit_sust(Ctx *c, uint64_t w) {
    if (!c->glsl || c->stage != STAGE_COMPUTE) { fail(c, "sust requires compute GLSL storage images"); return; }
    if (!maxwell_storage_image_write) { fail(c, "sust requires shaderStorageImageWriteWithoutFormat"); return; }
    bool bound = bits(w, 51, 1), raw = bits(w, 52, 1);
    int dim = (int)bits(w, 33, 3), mask = (int)bits(w, 20, 4), cache = (int)bits(w, 24, 2);
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8);
    if (raw) { fail(c, "sust.d raw formats are unsupported"); return; }
    if (mask != 15) { fail(c, "sust partial component mask %#x", mask); return; }
    if (bits(w, 49, 2)) { fail(c, "sust unsupported bounds mode"); return; }
    if (cache > 1) { fail(c, "sust unsupported cache mode %d", cache); return; }
    static const int types[] = {TEXTYPE_1D, -1, TEXTYPE_1D_ARRAY, TEXTYPE_2D, TEXTYPE_2D_ARRAY, TEXTYPE_3D};
    if (dim >= 6 || types[dim] < 0) { fail(c, "sust unsupported dimension %d", dim); return; }
    int type = types[dim], n = dim == 0 ? 1 : dim == 2 || dim == 3 ? 2 : 3;
    if ((d != RZ && ((d & 3) || d + 3 >= RZ)) || (a != RZ && a + n > RZ)) {
        fail(c, "sust register group alignment/range"); return;
    }
    uint32_t offset = (uint32_t)bits(w, 36, 13) * 4, cb = 0;
    if (!bound) {
        int hr = (int)bits(w, 39, 8);
        uint32_t origin = c->image_origin[hr];
        if (!origin) { fail(c, "sust bindless handle R%d has no proven constant-buffer source", hr); return; }
        cb = (origin >> 16) - 1; offset = origin & 0xffffu;
    }
    int slot = image_slot(c, !bound, cb, offset, type);
    if (c->fail) return;
    char co[160], values[512];
    if (n == 1) snprintf(co, sizeof co, "int(%s)", R(a));
    else {
        const char *x = R(a), *y = R(a == RZ ? RZ : a + 1);
        if (n == 2) snprintf(co, sizeof co, dim == 2 ? "ivec2(int(%s), int(%s & 0xffffu))" : "ivec2(int(%s), int(%s))", x, y);
        else snprintf(co, sizeof co, dim == 4 ? "ivec3(int(%s), int(%s), int(%s & 0xffffu))" : "ivec3(int(%s), int(%s), int(%s))",
                      x, y, R(a == RZ ? RZ : a + 2));
    }
    // Typed/packed SUST source channels carry float bits for normalized/float images.
    // The backend validates the actual storage view's FLOAT numeric class; integer formats fail closed.
    snprintf(values, sizeof values, "vec4(as_type<float>(%s), as_type<float>(%s), as_type<float>(%s), as_type<float>(%s))",
             R(d), R(d == RZ ? RZ : d + 1), R(d == RZ ? RZ : d + 2), R(d == RZ ? RZ : d + 3));
    if (n == 1) sp(&c->body, "    { int ic_ = %s, sz_ = imageSize(t%d); if (ic_ >= 0 && ic_ < sz_) imageStore(t%d, ic_, %s); }\n", co, slot, slot, values);
    else sp(&c->body, "    { ivec%d ic_ = %s, sz_ = imageSize(t%d); if (all(greaterThanEqual(ic_, ivec%d(0))) && all(lessThan(ic_, sz_))) imageStore(t%d, ic_, %s); }\n",
            n, co, slot, n, slot, values);
}

static const char *tex_type_name(int t) {
    switch (t) {
    case TEXTYPE_1D: return "1d";
    case TEXTYPE_3D: return "3d";
    case TEXTYPE_CUBE: return "cube";
    case TEXTYPE_2D_ARRAY: return "2d_array";
    case TEXTYPE_CUBE_ARRAY: return "cube_array";
    case TEXTYPE_1D_ARRAY: return "1d_array";
    default: return "2d";
    }
}

// Write texture results: component mask selects which of r,g,b,a go to consecutive registers.
static void write_tex_result(Ctx *c, int slot, const char *call, uint32_t mask, int d0, int d1, bool texs, bool f16) {
    bool uint_result = c->glsl && c->info->tex[slot].is_uint;
    if (uint_result && f16) { fail(c, "integer texture cannot use F16 results"); return; }
    sp(&c->body, "    { %s t = %s;\n", uint_result ? "uvec4" : "float4", call);
    if (f16) {
        // TEXS/TLDS .F16: components (in mask order) packed as half pairs into d0 (lo, hi) and d1 (lo, hi)
        int comps[4], n = 0;
        for (int comp = 0; comp < 4; comp++) if (mask & (1u << comp)) comps[n++] = comp;
        char v[4][16];
        for (int i = 0; i < 4; i++) { if (i < n) sprintf(v[i], "t[%d]", comps[i]); else strcpy(v[i], "0.0f"); }
        const char *pk = c->glsl ? "      r%d = packHalf2x16(vec2(%s, %s));\n" : "      r%d = as_type<uint>(half2(half(%s), half(%s)));\n";
        if (d0 != RZ) { c->reg_used[d0] = 1; sp(&c->body, pk, d0, v[0], v[1]); }
        if (d1 != RZ) { c->reg_used[d1] = 1; sp(&c->body, pk, d1, v[2], v[3]); }
        sp(&c->body, "    }\n");
        return;
    }
    int regs[4], n = 0;
    if (texs) {
        // TEXS: up to two components in d0/d0+1, two more in d1/d1+1
        int out[4] = {d0, d0 == RZ ? RZ : d0 + 1, d1, d1 == RZ ? RZ : d1 + 1};
        for (int i = 0; i < 4; i++) regs[i] = out[i];
    } else {
        for (int i = 0; i < 4; i++) regs[i] = d0 == RZ ? RZ : d0 + i;
    }
    for (int comp = 0; comp < 4; comp++) {
        if (!(mask & (1u << comp))) continue;
        int r = regs[n++];
        if (r != RZ) {
            c->reg_used[r] = 1;
            sp(&c->body, uint_result ? "      r%d = t[%d];\n" : "      r%d = as_type<uint>(t[%d]);\n", r, comp);
        }
    }
    sp(&c->body, "    }\n");
}

// Integer texel coordinates in guest units -> the (possibly larger) allocation of texture slot `slot`.
static const char *RSC2(Ctx *c, int slot, const char *fmt, ...) {
    static char bufs[4][1024];
    static int bi;
    char *o = bufs[bi++ & 3];
    char inner[700];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(inner, sizeof inner, fmt, ap);
    va_end(ap);
    if (c->stage == STAGE_VERTEX) { snprintf(o, 1024, "%s", inner); return o; }
    c->uses_rscale = true;
    snprintf(o, 1024, "rsc(%s, rscale[%d])", inner, slot + 1);
    return o;
}

// TEXS / TLDS: texture_info selects target and coordinate layout
static const char *alayer(Ctx *c, const char *r, int slot, char *buf) {
    if (c->glsl) sprintf(buf, "min(%s, uint(tsize%d(0).z) - 1u)", r, slot);
    else sprintf(buf, "min(%s, t%d.get_array_size() - 1u)", r, slot);
    return buf;
}
#define ALAYER(r, slot) alayer(c, (r), (slot), alloca(96))

// 2D normalized coordinates: a scaled texture whose contents occupy its top-left part (Dynamic resolution) is sampled
// there (prelude uvs(); rscale[1 + slot].zw). Vertex shaders have no rscale table: their lookups stay as they are.
static const char *uv2(Ctx *c, int slot, const char *x, const char *y, char *buf) {
    if (c->stage == STAGE_VERTEX) { snprintf(buf, 700, "%s2(%s, %s)", c->glsl ? "vec" : "float", x, y); return buf; }
    c->uses_rscale = true;
    snprintf(buf, 700, "uvs(%s2(%s, %s), rscale[%d])", c->glsl ? "vec" : "float", x, y, slot + 1);
    return buf;
}
#define UV2(slot, x, y) uv2(c, (slot), (x), (y), alloca(700))

// GLSL texture lookup on slot `slot` (its type is final here: a conflicting use fails in tex_slot).
// co: coordinate expressions (1 for 1D, 2 for 2D, 3 for 3D / cube), layer: uint expression or NULL, dref: depth
// reference or NULL, lod_kind 0 implicit / 1 level / 2 bias (lod), off: ivec2 texel offset or NULL.
// GLSL offsets must be constants, so register offsets move the coordinates by offset / size of the level
// (level 0 for implicit-LOD lookups). Non-fragment stages have no implicit LOD: level 0 (+ bias).
static void glsl_sample(Ctx *c, char *out, int slot, const char *const *co, const char *layer, const char *dref,
                        int lod_kind, const char *lod, const char *off) {
    if (c->fail) { strcpy(out, "vec4(0.0f)"); return; }  // e.g. a type conflict: tex[slot] is not this lookup's type
    const ShaderTexture *t = &c->info->tex[slot];
    int type = t->type;
    bool cube = type == TEXTYPE_CUBE || type == TEXTYPE_CUBE_ARRAY;
    int n = (type == TEXTYPE_1D || type == TEXTYPE_1D_ARRAY) ? 1 : (type == TEXTYPE_2D || type == TEXTYPE_2D_ARRAY) ? 2 : 3;
    if (c->stage != STAGE_FRAGMENT && lod_kind != 1) {
        if (lod_kind == 0) lod = "0.0f";
        lod_kind = 1;
    }
    if (t->shadow && type == TEXTYPE_3D) { fail(c, "3d shadow texture"); strcpy(out, "vec4(0.0f)"); return; }
    if (layer && type != TEXTYPE_1D_ARRAY && type != TEXTYPE_2D_ARRAY && type != TEXTYPE_CUBE_ARRAY) {
        fail(c, "array lookup on a 3d texture");  // TEX .3D with the array bit
        strcpy(out, "vec4(0.0f)");
        return;
    }
    char L[600], xs[1600], P[2000];
    if (lod_kind == 1) snprintf(L, sizeof L, "int(%s)", lod); else strcpy(L, "0");
    if (n == 1) {
        if (off) snprintf(xs, sizeof xs, "(%s + float((%s).x) / float(tsize%d(%s).x))", co[0], off, slot, L);
        else snprintf(xs, sizeof xs, "%s", co[0]);
    } else if (n == 2) {
        if (off) snprintf(xs, sizeof xs, "(vec2(%s, %s) + vec2(%s) / vec2(tsize%d(%s).xy))", co[0], co[1], off, slot, L);
        else snprintf(xs, sizeof xs, "vec2(%s, %s)", co[0], co[1]);
        if (c->stage != STAGE_VERTEX) { char w[1700]; snprintf(w, sizeof w, "uvs(%s, rscale[%d])", xs, slot + 1); strcpy(xs, w); c->uses_rscale = true; }
    } else {
        if (off && !cube) snprintf(xs, sizeof xs, "vec3(vec2(%s, %s) + vec2(%s) / vec2(tsize%d(%s).xy), %s)", co[0], co[1], off, slot, L, co[2]);
        else snprintf(xs, sizeof xs, "vec3(%s, %s, %s)", co[0], co[1], co[2]);  // cube lookups take no offset
    }
    const char *extra = NULL;  // separate compare argument (cube array shadow)
    if (!t->shadow) {
        if (layer) snprintf(P, sizeof P, "vec%d(%s, float(%s))", n + 1, xs, layer);
        else snprintf(P, sizeof P, "%s", xs);
    } else if (type == TEXTYPE_CUBE_ARRAY) {
        snprintf(P, sizeof P, "vec4(%s, float(%s))", xs, layer ? layer : "0u");
        extra = dref;
    } else if (type == TEXTYPE_1D) {
        snprintf(P, sizeof P, "vec3(%s, 0.0f, %s)", xs, dref);
    } else if (layer) {
        snprintf(P, sizeof P, "vec%d(%s, float(%s), %s)", n + 2, xs, layer, dref);
    } else {
        snprintf(P, sizeof P, "vec%d(%s, %s)", n + 1, xs, dref);
    }
    char e[2000], ex[300] = "";
    if (extra) snprintf(ex, sizeof ex, ", %s", extra);
    if (lod_kind == 0) snprintf(e, sizeof e, "texture(t%d, %s%s)", slot, P, ex);
    else if (lod_kind == 1) snprintf(e, sizeof e, "textureLod(t%d, %s%s, %s)", slot, P, ex, lod);
    else snprintf(e, sizeof e, "texture(t%d, %s%s, %s)", slot, P, ex, lod);
    if (t->shadow) sprintf(out, "vec4(%s)", e);
    else strcpy(out, e);
}

// TEXS / TLDS in GLSL (same slot allocation order and result layout as the MSL switch in emit_texs)
static bool glsl_texs(Ctx *c, uint64_t w, bool fetch, int *slot_out, char *call, const char *A, const char *A1,
                      const char *Bv, const char *B1) {
    uint32_t index = (uint32_t)bits(w, 36, 13);
    int info = (int)bits(w, 53, 4);
    int slot;
    const char *co[3];
    char fa[64], fa1[64], fb[64], fb1[64];
    sprintf(fa, "as_type<float>(%s)", A); sprintf(fa1, "as_type<float>(%s)", A1);
    sprintf(fb, "as_type<float>(%s)", Bv); sprintf(fb1, "as_type<float>(%s)", B1);
    if (!fetch) {
        switch (info) {
        case 0: slot = tex_slot(c, index, TEXTYPE_1D, false, false); co[0] = fa;
            glsl_sample(c, call, slot, co, NULL, NULL, 0, NULL, NULL); break;
        case 1: slot = tex_slot(c, index, TEXTYPE_2D, false, false); co[0] = fa; co[1] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 0, NULL, NULL); break;
        case 2: slot = tex_slot(c, index, TEXTYPE_2D, false, false); co[0] = fa; co[1] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 1, "0.0f", NULL); break;
        case 3: slot = tex_slot(c, index, TEXTYPE_2D, false, false); co[0] = fa; co[1] = fa1;
            glsl_sample(c, call, slot, co, NULL, NULL, 1, fb, NULL); break;
        case 4: slot = tex_slot(c, index, TEXTYPE_2D, true, false); co[0] = fa; co[1] = fa1;
            glsl_sample(c, call, slot, co, NULL, fb, 0, NULL, NULL); break;
        case 5: slot = tex_slot(c, index, TEXTYPE_2D, true, false); co[0] = fa; co[1] = fa1;
            glsl_sample(c, call, slot, co, NULL, fb1, 1, fb, NULL); break;
        case 6: slot = tex_slot(c, index, TEXTYPE_2D, true, false); co[0] = fa; co[1] = fa1;
            glsl_sample(c, call, slot, co, NULL, fb, 1, "0.0f", NULL); break;
        case 7: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true); co[0] = fa1; co[1] = fb;
            glsl_sample(c, call, slot, co, ALAYER(A, slot), NULL, 0, NULL, NULL); break;
        case 8: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true); co[0] = fa1; co[1] = fb;
            glsl_sample(c, call, slot, co, ALAYER(A, slot), NULL, 1, "0.0f", NULL); break;
        case 9: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, true, true); co[0] = fa1; co[1] = fb;
            glsl_sample(c, call, slot, co, A, fb1, 1, "0.0f", NULL); break;
        case 10: slot = tex_slot(c, index, TEXTYPE_3D, false, false); co[0] = fa; co[1] = fa1; co[2] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 0, NULL, NULL); break;
        case 11: slot = tex_slot(c, index, TEXTYPE_3D, false, false); co[0] = fa; co[1] = fa1; co[2] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 1, "0.0f", NULL); break;
        case 12: slot = tex_slot(c, index, TEXTYPE_CUBE, false, false); co[0] = fa; co[1] = fa1; co[2] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 0, NULL, NULL); break;
        case 13: slot = tex_slot(c, index, TEXTYPE_CUBE, false, false); co[0] = fa; co[1] = fa1; co[2] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 1, fb1, NULL); break;
        default: fail(c, "texs info %d", info); return false;
        }
    } else {
        switch (info) {
        case 0: slot = tex_slot(c, index, TEXTYPE_1D, false, false);
            sprintf(call, "texelFetch(t%d, int(%s), 0)", slot, A); break;
        case 1: slot = tex_slot(c, index, TEXTYPE_1D, false, false);
            sprintf(call, "texelFetch(t%d, int(%s), int(%s))", slot, A, Bv); break;
        case 2: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "texelFetch(t%d, ivec2(%s), 0)", slot, RSC2(c, slot, "uint2(%s, %s)", A, Bv)); break;
        case 4: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "texelFetch(t%d, ivec2(%s), 0)", slot, RSC2(c, slot, "uint2(int2(%s, %s) + int2(int(%s << 28) >> 28, int(%s << 24) >> 28))", A, A1, Bv, Bv)); break;
        case 5: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "texelFetch(t%d, ivec2(%s), int(%s))", slot, RSC2(c, slot, "uint2(%s, %s)", A, A1), Bv); break;
        case 7: slot = tex_slot(c, index, TEXTYPE_3D, false, false);
            sprintf(call, "texelFetch(t%d, ivec3(%s, %s, %s), 0)", slot, A, A1, Bv); break;
        case 8: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true);
            sprintf(call, "texelFetch(t%d, ivec3(ivec2(%s), int(%s)), 0)", slot, RSC2(c, slot, "uint2(%s, %s)", A1, Bv), A); break;
        case 12: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "texelFetch(t%d, ivec2(%s), int(%s))", slot, RSC2(c, slot, "uint2(int2(%s, %s) + int2(int(%s << 28) >> 28, int(%s << 24) >> 28))", A, A1, B1, B1), Bv); break;
        default: fail(c, "tlds info %d", info); return false;
        }
    }
    *slot_out = slot;
    return true;
}

static void emit_texs(Ctx *c, uint64_t w, bool fetch) {
    int d0 = (int)bits(w, 0, 8), d1 = (int)bits(w, 28, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8);
    uint32_t index = (uint32_t)bits(w, 36, 13);
    int info = (int)bits(w, 53, 4);
    int msel = (int)bits(w, 50, 3);
    uint32_t mask;
    if (d1 == RZ) {
        static const uint32_t m2[] = {1, 2, 4, 8, 3, 9, 10, 12};
        mask = m2[msel];
    } else {
        static const uint32_t m4[] = {7, 11, 13, 14, 15, 15, 15, 15};
        mask = m4[msel];
    }
    char ra[16], ra1[16], rb[16], rb1[16];
    const char *A = reg(c, a, ra), *A1 = reg(c, a == RZ ? RZ : a + 1, ra1), *Bv = reg(c, b, rb),
               *B1 = reg(c, b == RZ ? RZ : b + 1, rb1);
    char call[1024];
    int slot;
    if (c->glsl) {
        char gcall[4096];
        if (!glsl_texs(c, w, fetch, &slot, gcall, A, A1, Bv, B1)) return;
        write_tex_result(c, slot, gcall, mask, d0, d1, true, !bits(w, 59, 1));
        return;
    }
    if (!fetch) {
        switch (info) {
        case 0: slot = tex_slot(c, index, TEXTYPE_1D, false, false);
            sprintf(call, "t%d.sample(s%d, %s)", slot, slot, F(A)); break;
        case 1: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.sample(s%d, %s)", slot, slot, UV2(slot, F(A), F(Bv))); break;
        case 2: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.sample(s%d, %s, level(0))", slot, slot, UV2(slot, F(A), F(Bv))); break;
        case 3: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.sample(s%d, %s, level(%s))", slot, slot, UV2(slot, F(A), F(A1)), F(Bv)); break;
        case 4: slot = tex_slot(c, index, TEXTYPE_2D, true, false);
            sprintf(call, "float4(t%d.sample_compare(s%d, %s, %s))", slot, slot, UV2(slot, F(A), F(A1)), F(Bv)); break;
        case 5: slot = tex_slot(c, index, TEXTYPE_2D, true, false);
            sprintf(call, "float4(t%d.sample_compare(s%d, %s, %s, level(%s)))", slot, slot, UV2(slot, F(A), F(A1)), F(B1), F(Bv)); break;
        case 6: slot = tex_slot(c, index, TEXTYPE_2D, true, false);
            sprintf(call, "float4(t%d.sample_compare(s%d, %s, %s, level(0)))", slot, slot, UV2(slot, F(A), F(A1)), F(Bv)); break;
        case 7: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true);
            sprintf(call, "t%d.sample(s%d, %s, %s)", slot, slot, UV2(slot, F(A1), F(Bv)), ALAYER(A, slot)); break;
        case 8: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true);
            sprintf(call, "t%d.sample(s%d, %s, %s, level(0))", slot, slot, UV2(slot, F(A1), F(Bv)), ALAYER(A, slot)); break;
        case 9: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, true, true);
            sprintf(call, "float4(t%d.sample_compare(s%d, %s, %s, %s, level(0)))", slot, slot, UV2(slot, F(A1), F(Bv)), A, F(B1)); break;
        case 10: slot = tex_slot(c, index, TEXTYPE_3D, false, false);
            sprintf(call, "t%d.sample(s%d, float3(%s, %s, %s))", slot, slot, F(A), F(A1), F(Bv)); break;
        case 11: slot = tex_slot(c, index, TEXTYPE_3D, false, false);
            sprintf(call, "t%d.sample(s%d, float3(%s, %s, %s), level(0))", slot, slot, F(A), F(A1), F(Bv)); break;
        case 12: slot = tex_slot(c, index, TEXTYPE_CUBE, false, false);
            sprintf(call, "t%d.sample(s%d, float3(%s, %s, %s))", slot, slot, F(A), F(A1), F(Bv)); break;
        case 13: slot = tex_slot(c, index, TEXTYPE_CUBE, false, false);
            sprintf(call, "t%d.sample(s%d, float3(%s, %s, %s), level(%s))", slot, slot, F(A), F(A1), F(Bv), F(B1)); break;
        default: fail(c, "texs info %d", info); return;
        }
    } else {
        switch (info) {
        case 0: slot = tex_slot(c, index, TEXTYPE_1D, false, false);
            sprintf(call, "t%d.read(uint(%s))", slot, A); break;
        case 1: slot = tex_slot(c, index, TEXTYPE_1D, false, false);
            sprintf(call, "t%d.read(uint(%s), uint(%s))", slot, A, Bv); break;
        case 2: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.read(%s)", slot, RSC2(c, slot, "uint2(%s, %s)", A, Bv)); break;
        case 4: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.read(%s)", slot, RSC2(c, slot, "uint2(int2(%s, %s) + int2(int(%s << 28) >> 28, int(%s << 24) >> 28))", A, A1, Bv, Bv)); break;
        case 5: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.read(%s, uint(%s))", slot, RSC2(c, slot, "uint2(%s, %s)", A, A1), Bv); break;
        case 7: slot = tex_slot(c, index, TEXTYPE_3D, false, false);
            sprintf(call, "t%d.read(uint3(%s, %s, %s))", slot, A, A1, Bv); break;
        case 8: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true);
            sprintf(call, "t%d.read(%s, uint(%s))", slot, RSC2(c, slot, "uint2(%s, %s)", A1, Bv), A); break;
        case 12: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.read(%s, uint(%s))", slot, RSC2(c, slot, "uint2(int2(%s, %s) + int2(int(%s << 28) >> 28, int(%s << 24) >> 28))", A, A1, B1, B1), Bv); break;
        default: fail(c, "tlds info %d", info); return;
        }
    }
    write_tex_result(c, slot, call, mask, d0, d1, true, !bits(w, 59, 1));  // 0xd0/0xd2: .F16 results
}

// A statically proven CB handle needs a normal sampler binding, not runtime
// descriptor indexing. Keep it separate from bound slots and storage images.
static int bindless_tex_slot(Ctx *c, uint32_t cb, uint32_t offset, int type, bool shadow, bool array) {
    for (int i = 0; i < c->info->ntex; i++) {
        ShaderTexture *t = &c->info->tex[i];
        if (!t->bindless || t->is_image || t->bindless_cbuf != cb || t->bindless_offset != offset) continue;
        if (t->type != type || t->shadow != shadow) fail(c, "bindless texture used as two types");
        return i;
    }
    if (c->info->ntex >= 31) { fail(c, "too many textures/images"); return 0; }
    int slot = c->info->ntex++;
    ShaderTexture *t = &c->info->tex[slot];
    t->bindless = true; t->bindless_cbuf = cb; t->bindless_offset = offset;
    t->type = (uint8_t)type; t->shadow = shadow; t->is_array = array;
    // Float sampler only: the backend must reject an incompatible integer view.
    return slot;
}

// Bound TLD4/TLD4S fields independently decoded from MIT nxport
// 3a5d23c67f23346a9bca0a03d4e03b01375090c4, texture.rs/inst_decoders.rs.
// A gather selects one texture channel, returns four neighbouring texels, then
// applies its destination mask. This slice admits2D, no offsets/depth compare,
// no sparse predicate and F32 results; unsupported variants fail explicitly.
static void emit_gather(Ctx *c, uint64_t w, bool swizzled) {
    if (!c->glsl) { fail(c, "texture gather requires GLSL"); return; }
    int d0 = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8);
    int d1 = swizzled ? (int)bits(w, 28, 8) : RZ;
    uint32_t index = (uint32_t)bits(w, 36, 13);
    uint32_t component = (uint32_t)bits(w, swizzled ? 52 : 56, 2);
    uint32_t mask = swizzled ? 15 : (uint32_t)bits(w, 31, 4);
    if (bits(w, 50, 1) || (swizzled ? bits(w, 51, 1) || bits(w, 55, 1)
        : bits(w, 28, 3) != 2 || bits(w, 54, 2) || bits(w, 35, 1) || bits(w, 51, 3) != PT)) {
        fail(c, "texture gather unsupported dimension/modifiers"); return;
    }
    int outputs = swizzled ? 2 : __builtin_popcount(mask);
    if ((!swizzled && a+2 > RZ) || (d0 != RZ && d0+outputs > RZ) ||
        (swizzled && d1 != RZ && d1+2 > RZ)) {
        fail(c, "texture gather register group range"); return;
    }
    if ((!mask || d0 == RZ) && (!swizzled || d1 == RZ)) return;
    int slot = tex_slot(c, index, TEXTYPE_2D, false, false);
    if (c->fail) return;
    hflush_all(c);
    char ra[16], rb[16], x[64], y[64], call[1024];
    snprintf(x, sizeof x, "as_type<float>(%s)", reg(c, a, ra));
    snprintf(y, sizeof y, "as_type<float>(%s)", reg(c, swizzled ? b : a+1, rb));
    snprintf(call, sizeof call, "textureGather(t%d, %s, %u)", slot, UV2(slot, x, y), component);
    // The vector temporary in write_tex_result captures every input before
    // writing possibly overlapping coordinate/destination register groups.
    write_tex_result(c, slot, call, mask, d0, d1, swizzled, false);
}

// TEX (full form): coordinates from consecutive registers at gpr8, extra parameters at gpr20.
// TEX.B consumes its handle from gpr20 FIRST. Independently decoded from MIT
// nxport 3a5d23c67f23346a9bca0a03d4e03b01375090c4,
// crates/sass2spv/src/instructions/texture.rs (tex_b / emit_tex).
static void emit_tex(Ctx *c, uint64_t w, bool bindless) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8);
    uint32_t index = bindless ? 0 : (uint32_t)bits(w, 36, 13);
    int target = (int)bits(w, 29, 2);
    bool array = bits(w, 28, 1), dc = bits(w, 50, 1), aoffi = bindless ? bits(w, 36, 1) : bits(w, 54, 1);
    uint32_t mask = (uint32_t)bits(w, 31, 4);
    int lod_mode = bindless ? (int)bits(w, 37, 3) : (int)bits(w, 55, 3);  // 0 none, 1 lz, 2 lb, 3 ll, 6 lba, 7 lla
    uint32_t origin = 0;
    if (bindless) {
        if (!c->glsl) { fail(c, "bindless tex requires GLSL"); return; }
        if ((target != 1 && target != 2) || (target == 2 && (array || dc)) ||
            aoffi || bits(w, 40, 1) || bits(w, 35, 1) || bits(w, 51, 3) != PT ||
            lod_mode == 4 || lod_mode == 5 || lod_mode >= 6) {
            fail(c, "bindless tex unsupported dimension/modifiers"); return;
        }
        origin = c->image_origin[b];
        if (!origin) { fail(c, "bindless tex handle R%d has no proven constant-buffer source", b); return; }
        int nmeta = (lod_mode == 2 || lod_mode == 3) + (int)dc;
        int ncoords = (target == 1 ? 2 : 3) + (int)array;
        int nout = __builtin_popcount(mask);
        if (a + ncoords > RZ || b + 1 + nmeta > RZ || (d != RZ && d + nout > RZ)) {
            fail(c, "bindless tex register group range"); return;
        }
        b++;  // remaining LOD / depth metadata follows the handle
    }
    int type = target == 0 ? (array ? TEXTYPE_1D_ARRAY : TEXTYPE_1D)
             : target == 1 ? (array ? TEXTYPE_2D_ARRAY : TEXTYPE_2D)
             : target == 2 ? TEXTYPE_3D
                           : (array ? TEXTYPE_CUBE_ARRAY : TEXTYPE_CUBE);
    int ncoord = target == 0 ? 1 : target == 1 ? 2 : 3;
    int slot = bindless ? bindless_tex_slot(c, (origin >> 16) - 1, origin & 0xffffu, type, dc, array)
                        : tex_slot(c, index, type, dc, array);
    if (c->fail) return;
    if (c->glsl) {  // same register layout as below
        char cs[3][40], lay[96], lodv[48], offv[96], drv[48], gcall[4096];
        const char *co[3], *layp = NULL, *lodp = NULL, *offp = NULL, *drp = NULL;
        int ar2 = a, p2 = b, lk = 0;
        hflush_all(c);  // (the registers read below, not through reg)
        if (array) { sprintf(lay, bindless ? "min(uint(r%d & 0xffffu), uint(tsize%d(0).z) - 1u)" : "min(uint(r%d), uint(tsize%d(0).z) - 1u)", a, slot); c->reg_used[a] = 1; ar2 = a + 1; layp = lay; }
        for (int i = 0; i < ncoord; i++) { sprintf(cs[i], "as_type<float>(r%d)", ar2 + i); c->reg_used[ar2 + i] = 1; co[i] = cs[i]; }
        if (lod_mode == 2 || lod_mode == 3 || lod_mode == 6 || lod_mode == 7) {
            c->reg_used[p2] = 1;
            sprintf(lodv, "as_type<float>(r%d)", p2);
            lodp = lodv;
            lk = lod_mode == 2 || lod_mode == 6 ? 2 : 1;
            p2++;
        } else if (lod_mode == 1) {
            lk = 1;
            lodp = "0.0f";
        }
        if (aoffi) { c->reg_used[p2] = 1; sprintf(offv, "int2(int(r%d << 28) >> 28, int(r%d << 24) >> 28)", p2, p2); offp = offv; p2++; }
        if (dc) { c->reg_used[p2] = 1; sprintf(drv, "as_type<float>(r%d)", p2); drp = drv; p2++; }
        glsl_sample(c, gcall, slot, co, layp, drp, lk, lodp, offp);
        write_tex_result(c, slot, gcall, mask, d, RZ, false, false);
        return;
    }
    char coords[256] = "", layer[64] = "", tmp[64];
    int ar = a;
    // the hardware clamps the array index to the layer count
    if (array) { sprintf(layer, "min(uint(r%d), t%d.get_array_size() - 1u)", a, slot); c->reg_used[a] = 1; ar = a + 1; }
    for (int i = 0; i < ncoord; i++) {
        sprintf(tmp, "%sas_type<float>(r%d)", i ? ", " : "", ar + i);
        c->reg_used[ar + i] = 1;
        strcat(coords, tmp);
    }
    int pr = b;
    char lod[96] = "", dref[64] = "", off[128] = "";
    if (lod_mode == 2 || lod_mode == 3 || lod_mode == 6 || lod_mode == 7) {
        c->reg_used[pr] = 1;
        sprintf(lod, lod_mode == 2 || lod_mode == 6 ? ", bias(as_type<float>(r%d))" : ", level(as_type<float>(r%d))", pr);
        pr++;
    } else if (lod_mode == 1) {
        strcpy(lod, ", level(0)");
    }
    if (aoffi) {
        c->reg_used[pr] = 1;
        sprintf(off, ", int2(int(r%d << 28) >> 28, int(r%d << 24) >> 28)", pr, pr);
        pr++;
    }
    if (dc) { c->reg_used[pr] = 1; sprintf(dref, "as_type<float>(r%d)", pr); pr++; }
    const char *vec = ncoord == 1 ? "" : ncoord == 2 ? "float2" : "float3";
    char cexpr[420], call[1024];
    if (ncoord == 2 && c->stage != STAGE_VERTEX) { snprintf(cexpr, sizeof cexpr, "uvs(float2(%s), rscale[%d])", coords, slot + 1); c->uses_rscale = true; }
    else snprintf(cexpr, sizeof cexpr, "%s(%s)", vec, coords);
    if (dc)
        sprintf(call, "float4(t%d.sample_compare(s%d, %s%s%s, %s%s%s))", slot, slot, cexpr, array ? ", " : "",
                layer, dref, lod, off);
    else
        sprintf(call, "t%d.sample(s%d, %s%s%s%s%s)", slot, slot, cexpr, array ? ", " : "", layer, lod, off);
    write_tex_result(c, slot, call, mask, d, RZ, false, false);
}

// ------------------------------------------------------------------ instruction translation
// The game's light-culling compute shader (a 30x17 grid of 64-pixel tiles for the tiled lighting of interiors and
// caves) turns a tile's pixel column into a view direction with the screen height alone: x = px * 2 / height -
// aspect, which assumes square pixels (width / height = aspect). With a widened camera the image keeps its 1920x1080
// target, so pixels are no longer square: at 32:9 the tiles right of the screen centre got the lights meant for
// half as far out, and the lighting changed along a hard vertical line where a light's tiles ended (the floor
// right of it lighter, MarthaUkuku at 2/3 of the window). The two column steps (pc 0x198: tile x, 0x1a8: tile x +
// 1) are scaled by aspect * height / width, which is 1 at 16:9: the original shader's result there.
static void light_tiles_fix(Ctx *c, const uint64_t *code, size_t end, uint32_t pc) {
    static const uint64_t sig[][2] = {{0x188, 0x5c68100000970309ull}, {0x198, 0x5c68100000d7030dull},
                                      {0x1a8, 0x5c6810000087030bull}, {0x1b0, 0x5c68100000470303ull}};
    if (c->stage != STAGE_COMPUTE || (pc != 0x198 && pc != 0x1a8) || end <= 0x1b0 / 8) return;
    for (size_t i = 0; i < sizeof sig / sizeof sig[0]; i++)
        if (code[sig[i][0] / 8] != sig[i][1]) return;
    int d = (int)(code[pc / 8] & 0xff);  // (the destination: r13 / r11)
    hmat(c, d);
    char a[48], h[48], w[48];
    cbuf_ref(c, 4, 0x1bc, NULL, a);  // camera aspect
    cbuf_ref(c, 4, 0x1c4, NULL, h);  // target height
    cbuf_ref(c, 4, 0x1c0, NULL, w);  // target width
    sp(&c->body, "    r%d = as_type<uint>(as_type<float>(r%d) * (as_type<float>(%s) * as_type<float>(%s) / as_type<float>(%s)));\n", d, d, a, h, w);  // (GLSL: as_type is rewritten)
    c->hstate[d] = 0, c->hcstate[d] = 0, c->reg_tz[d] = 0;  // (the uint is the value now: no shadow, no known zeros)
}

/* Guest addresses are a low/high register pair even when the host GPU cannot
 * advertise shaderInt64. The mirrored heap fits in a uint32 byte offset; keep
 * the high word for a lexicographic window check before taking that offset.
 * Both the read-only snapshot and writable mirror use this same address check. */
static void glsl_gmem_address32(Ctx *c, int areg, bool wide, int64_t off) {
    if (maxwell_gmem_size > UINT32_MAX) {
        fail(c, "uint32 shader address path requires a heap smaller than4GiB");
        return;
    }
    if (maxwell_gmem_base > UINT64_MAX - maxwell_gmem_size) {
        fail(c, "uint32 shader heap window wraps the guest address space");
        return;
    }
    uint64_t end = maxwell_gmem_base + maxwell_gmem_size;
    sp(&c->body, "    { uint al = %s, ah = %s;\n",
       areg == RZ ? "0u" : R(areg), areg == RZ || !wide ? "0u" : R(areg + 1));
    if (!off) sp(&c->body, "      uint lo = al, hi = ah;\n");
    else {
        sp(&c->body, "      uint lo = al + 0x%08xu;\n", (uint32_t)off);
        sp(&c->body, "      uint hi = ah + %uu + uint(lo < al);\n", off < 0 ? UINT32_MAX : 0u);
    }
    // Compare the complete pair before taking its modular low-word offset.
    // The heap can straddle a 32-bit boundary; neither half is sufficient alone.
    sp(&c->body, "      uint ga = lo - 0x%08xu;\n", (uint32_t)maxwell_gmem_base);
    sp(&c->body, "      bool ok = (hi > 0x%08xu || (hi == 0x%08xu && lo >= 0x%08xu)) && "
       "(hi < 0x%08xu || (hi == 0x%08xu && lo < 0x%08xu));\n",
       (uint32_t)(maxwell_gmem_base >> 32), (uint32_t)(maxwell_gmem_base >> 32), (uint32_t)maxwell_gmem_base,
       (uint32_t)(end >> 32), (uint32_t)(end >> 32), (uint32_t)end);
}

// 64-bit guest address path (shaderInt64 available): the ulong address and the half-open heap
// membership check shared by ATOM.ADD, RED and LDG/STG's int64 branches.
static void glsl_gmem_address64(Ctx *c, int areg, bool wide, int64_t off) {
    char addr[256];
    if (areg == RZ) snprintf(addr, sizeof addr, "ulong(%lld)", (long long)off);
    else if (wide) snprintf(addr, sizeof addr, "((ulong(%s) << 32 | ulong(%s)) + ulong(%lld))", R(areg + 1), R(areg), (long long)off);
    else snprintf(addr, sizeof addr, "(ulong(%s) + ulong(%lld))", R(areg), (long long)off);
    sp(&c->body, "    { ulong ga = %s - 0x%llxul; bool ok = ga < 0x%llxul;\n", addr,
       (unsigned long long)maxwell_gmem_base, (unsigned long long)maxwell_gmem_size);
}

// Scalar video operands select/sign-extend a byte or halfword before the two
// integer min/max operations. A full-word operand keeps its exact bit pattern.
static void video_operand(char *out, size_t size, const char *value, unsigned width, unsigned selector, bool sign) {
    unsigned bits_ = width == 3 ? 32 : width == 2 ? 16 : 8;
    unsigned shift = bits_ == 32 ? 0 : selector * bits_;
    if (bits_ == 32) snprintf(out, size, "%s", value);
    else if (sign) snprintf(out, size, "uint(int(%s << %uu) >> %uu)", value, 32 - bits_ - shift, 32 - bits_);
    else snprintf(out, size, "((%s >> %uu) & 0x%xu)", value, shift, (1u << bits_) - 1);
}
// A word never possibly stored remains zero. Inspect all store sites, including
// predicated/unreachable ones; any dynamic address keeps the legacy array.
static void local_store_footprint(Ctx *c, const uint64_t *code, size_t end) {
    if (!c->glsl) return;
    c->local_sparse = true;
    for (size_t i = 0; i < end; i++) {
        if (i % 4 == 0 || (code[i] & UINT64_C(0xfff8000000000000)) != UINT64_C(0xef50000000000000)) continue;
        uint64_t w = code[i];
        if (bits(w, 8, 8) != RZ) { c->local_sparse = false; return; }
        uint32_t off = (uint32_t)(int32_t)sbits(w, 20, 24);
        unsigned size = (unsigned)bits(w, 48, 3), words = size == 5 ? 2 : size >= 6 ? 4 : 1;
        for (unsigned k = 0; k < words; k++) {
            unsigned slot = ((off >> 2) + k) & (LMEM_WORDS - 1);
            if (!c->local_written[slot]) {
                c->local_written[slot] = 1;
                if (++c->local_count > LMEM_SCALAR_MAX) { c->local_sparse = false; return; }
            }
        }
    }
}

static const char *local_sparse_read(Ctx *c, char *out) {
    size_t at = 0;
    for (unsigned i = 0; i < LMEM_WORDS; i++) if (c->local_written[i])
        at += (size_t)sprintf(out + at, "(li == %uu ? lm%u : ", i, i);
    at += (size_t)sprintf(out + at, "0u");
    for (unsigned i = 0; i < c->local_count; i++) out[at++] = ')';
    out[at] = 0;
    return out;
}

static void emit_insn(Ctx *c, uint64_t w, uint32_t pc) {
    char t[2048], u[2048];
    c->image_origin_dst = -1; c->image_origin_new = 0;
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8);
    int pr = (int)bits(w, 16, 3);
    bool prn = bits(w, 19, 1);
    uint16_t op13 = (uint16_t)(w >> 51);  // top 13 bits
    // predicate guard. SSY/PBK/PCNT/PRET take no predicate: those bits are zero in the encoding,
    // which would otherwise read as @P0.
    uint64_t top12 = w & 0xfff0000000000000ull;
    if (top12 == 0xe290000000000000ull || top12 == 0xe2a0000000000000ull || top12 == 0xe2b0000000000000ull ||
        top12 == 0xe270000000000000ull) {
        pr = PT;
        prn = false;
    }
    bool guarded = !(pr == PT && !prn);
    if (pr == PT && prn) return;  // @!pt: never executes
    size_t text0 = c->body.len;
    c->tz_dst = -1;
    c->in_guard = guarded;
    if (guarded) hflush_all(c);  // (a shadow packed inside the guard would be missed on the other path)
    if (guarded) sp(&c->body, "  if (%s) {\n", P(pr, prn));

#define OPM(val, mask) ((w & (uint64_t)(mask)) == (uint64_t)(val))
    if (OPM(0xe300000000000000ull, 0xfff0000000000000ull)) {  // EXIT
        sp(&c->body, c->branchy ? "    exit_now = true; break;\n" : "    break;\n");
    } else if (OPM(0xe330000000000000ull, 0xfff0000000000000ull)) {  // KIL
        c->info->uses_discard = true;
        if (getenv("LARECOMP_NO_DISCARD")) {
        } else if (!c->glsl) sp(&c->body, "    discard_fragment();\n");
        else if (c->stage == STAGE_FRAGMENT) sp(&c->body, "    discard;\n");
        else sp(&c->body, "    /* kil */\n");  // GLSL has discard in fragment shaders only
    } else if (OPM(0xe240000000000000ull, 0xfff0000000000020ull)) {  // BRA
        uint32_t target = norm_target((int64_t)pc + 8 + sbits(w, 20, 24));
        c->uses_branches = true;
        sp(&c->body, "    pc = %uu; continue;\n", target);
    } else if (c->static_stacks && (OPM(0xe290000000000000ull, 0xfff0000000000020ull) || OPM(0xe2a0000000000000ull, 0xfff0000000000020ull) ||
                                    OPM(0xe2b0000000000000ull, 0xfff0000000000020ull))) {  // SSY / PBK / PCNT, resolved
        c->uses_branches = true;
    } else if (c->static_stacks && (OPM(0xf0f8000000000000ull, 0xfff8000000000000ull) || OPM(0xe340000000000000ull, 0xfff0000000000000ull) ||
                                    OPM(0xe350000000000000ull, 0xfff0000000000000ull))) {  // SYNC / BRK / CONT, resolved
        int32_t t = c->flow_to[pc / 8];
        if (t >= 0) sp(&c->body, "    pc = %uu; continue;\n", (uint32_t)t);
        else sp(&c->body, "    exit_now = true; break;\n");  // (never reached)
    } else if (OPM(0xe290000000000000ull, 0xfff0000000000020ull)) {  // SSY
        c->uses_branches = true;
        sp(&c->body, "    ssy[ssy_n++] = %uu;\n", norm_target((int64_t)pc + 8 + sbits(w, 20, 24)));
    } else if (OPM(0xe2a0000000000000ull, 0xfff0000000000020ull)) {  // PBK
        c->uses_branches = true;
        sp(&c->body, "    pbk[pbk_n++] = %uu;\n", norm_target((int64_t)pc + 8 + sbits(w, 20, 24)));
    } else if (OPM(0xe2b0000000000000ull, 0xfff0000000000020ull)) {  // PCNT
        c->uses_branches = true;
        sp(&c->body, "    pcnt[pcnt_n++] = %uu;\n", norm_target((int64_t)pc + 8 + sbits(w, 20, 24)));
    } else if (OPM(0xf0f8000000000000ull, 0xfff8000000000000ull)) {  // SYNC
        sp(&c->body, "    pc = ssy[--ssy_n]; continue;\n");
    } else if (OPM(0xe340000000000000ull, 0xfff0000000000000ull)) {  // BRK
        sp(&c->body, "    pc = pbk[--pbk_n]; continue;\n");
    } else if (OPM(0xe350000000000000ull, 0xfff0000000000000ull)) {  // CONT
        sp(&c->body, "    pc = pcnt[pcnt_n - 1]; continue;\n");
    } else if (OPM(0xf0f0000000000000ull, 0xfff8000000000000ull) || OPM(0x50b0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0xef98000000000000ull, 0xfff8000000000000ull)) {
        // DEPBAR / NOP / MEMBAR
    } else if (OPM(0xe00000000000ff00ull, 0xff0000400000ff00ull) || OPM(0xe000004000000000ull, 0xff00004000000000ull)) {
        // IPA: interpolate fragment input
        bool idx = bits(w, 38, 1);
        uint32_t addr = (uint32_t)bits(w, 28, 10);
        int mode = (int)bits(w, 54, 2);  // 0 pass, 1 multiply, 2 constant, 3 sc
        if (idx) { fail(c, "ipa idx"); }
        if (addr >= 0x80 && addr < 0x280) {
            int at = (addr - 0x80) >> 4;
            if (mode == 2) c->flat_mask |= 1u << at;
        }
        c->ipa_mode = mode;
        c->ipa_mul = (int)bits(w, 20, 8);
        attr_load(c, d, addr, NULL);
        c->ipa_mode = -1;
        if (bits(w, 51, 1) && d != RZ) sp(&c->body, "    r%d = as_type<uint>(saturate(as_type<float>(r%d)));\n", d, d);
    } else if (OPM(0xefd8000000000000ull, 0xfff8000000000000ull)) {  // LD attribute
        int n = 1 + (int)bits(w, 47, 2);
        uint32_t addr = (uint32_t)bits(w, 20, 10);
        int ia = (int)bits(w, 8, 8);
        if (ia != RZ) { fail(c, "indexed ld a[]"); }
        for (int i = 0; i < n; i++) attr_load(c, d + i, addr + 4 * i, NULL);
    } else if (OPM(0xeff0000000000000ull, 0xfff8000000000000ull)) {  // ST attribute
        int n = 1 + (int)bits(w, 47, 2);
        uint32_t addr = (uint32_t)bits(w, 20, 10);
        for (int i = 0; i < n; i++) attr_store(c, d + i, addr + 4 * i);
    } else if (OPM(0xef90000000000000ull, 0xfff8000000000000ull)) {  // LD c[idx][reg + imm]
        int idx = (int)bits(w, 36, 5);
        // The decoder field is unsigned, but LDC's address addition sign-extends
        // its 16-bit byte displacement (MIT nxport instructions/memory.rs).
        int64_t off = (int64_t)bits(w, 20, 16);
        if (off & 0x8000) off -= 0x10000;
        // Indexed-slot modes need separate slot/address extraction; do not
        // silently execute them as this supported ordinary address mode.
        if (bits(w, 44, 2) != 0) { fail(c, "unsupported LDC address mode"); return; }
        int sz = (int)bits(w, 48, 3);
        int n = sz == 5 ? 2 : 1;
        char rb[16];
        const char *dyn = a == RZ ? NULL : reg(c, a, rb);
        // RZ plus a negative displacement is a wrapped unsigned address, not a
        // negative constant GLSL array index. Keep the normal bounded CB path.
        bool rz_negative = !dyn && off < 0;
        if (rz_negative) {
            // A non-const local also avoids a compile-time out-of-range array
            // subscript when the robust GLSL path omits its index clamp.
            sp(&c->body, "    { uint ldc_addr = 0u;\n");
            dyn = "ldc_addr";
        }
        // LARECOMP_VK_ALIGNED_CBUF=0: every run-time indexed load through cbr (comparisons)
        static int aligned_on = -1;
        if (aligned_on < 0) aligned_on = getenv("LARECOMP_VK_ALIGNED_CBUF") ? atoi(getenv("LARECOMP_VK_ALIGNED_CBUF")) != 0 : 1;
        bool aligned = dyn && c->glsl && aligned_on && c->reg_tz[a] >= 4;  // (a multiple of 16: cbuf_ref_aligned)
        // LDC.64 R4, c[i][R4 + x]: the address register is also a destination. Read it once before the writes, or
        // the second word comes from c[i][<first word> + x + 4] (fix from letsgo-recomp)
        bool overlap = dyn && n > 1 && a >= d && a < d + n;
        if (overlap) { sp(&c->body, "    { uint ldc_addr = %s;\n", dyn); dyn = "ldc_addr"; }
        for (int i = 0; i < n; i++) {
            char cb[256];
            if (aligned) cbuf_ref_aligned(c, idx, off + 4 * i, dyn, cb);
            else cbuf_ref(c, idx, off + 4 * i, dyn, cb);
            if (dyn) c->info->cbuf_size[idx] = 0x10000;
            set_reg(c, d + i, cb);
        }
        if (overlap || rz_negative) sp(&c->body, "    }\n");
        if (!dyn && sz == 4 && idx < 18 && off <= 0xfffc) {
            c->image_origin_dst = d; c->image_origin_new = ((uint32_t)(idx + 1) << 16) | (uint32_t)off;
        }
    } else if (OPM(0xeb20000000000000ull, 0xffe0000000000000ull)) {  // SUST(.B)(.D), explicit unsupported variants rejected
        emit_sust(c, w);
    } else if (OPM(0xd000000000000000ull, 0xf600000000000000ull)) {  // TEXS
        emit_texs(c, w, false);
    } else if (OPM(0xd200000000000000ull, 0xf600000000000000ull)) {  // TLDS
        emit_texs(c, w, true);
    } else if (OPM(0xdf00000000000000ull, 0xff40000000000000ull)) {  // TLD4S, F16 rejected explicitly
        emit_gather(c, w, true);
    } else if (OPM(0xc800000000000000ull, 0xfc00000000000000ull)) {  // TLD4
        emit_gather(c, w, false);
    } else if (OPM(0xc038000000000000ull, 0xfc38000000000000ull)) {  // TEX
        emit_tex(c, w, false);
    } else if (OPM(0xdeb8000000000000ull, 0xfff8000000000000ull)) {  // TEX.B (bindless)
        emit_tex(c, w, true);
    } else if (OPM(0xdf48000000000000ull, 0xfff8000000000000ull)) {  // TXQ
        uint32_t index = (uint32_t)bits(w, 36, 13);
        int qt = (int)bits(w, 22, 6);
        uint32_t mask = (uint32_t)bits(w, 31, 4);
        int slot = tex_slot(c, index, -1, false, false);  // the type comes from a sample, if any
        if (qt == 1 && c->glsl) {  // tsize<slot>() is emitted with the slot's final type
            if (c->stage != STAGE_VERTEX) {
                c->uses_rscale = true;
                sp(&c->body, "    { uint lv = %s; uvec4 q = uvec4(uint(floor(float(tsize%d(int(lv)).x) * rscale[%d].z / rscale[%d].x + 0.5f)), "
                   "uint(floor(float(tsize%d(int(lv)).y) * rscale[%d].w / rscale[%d].y + 0.5f)), 1u, uint(textureQueryLevels(t%d)));\n",
                   R(a), slot, slot + 1, slot + 1, slot, slot + 1, slot + 1, slot);
            } else {
                sp(&c->body, "    { uint lv = %s; uvec4 q = uvec4(uint(tsize%d(int(lv)).x), uint(tsize%d(int(lv)).y), 1u, uint(textureQueryLevels(t%d)));\n",
                   R(a), slot, slot, slot);
            }
            int n = 0;
            for (int i = 0; i < 4; i++)
                if (mask & (1u << i)) { c->reg_used[d + n] = 1; sp(&c->body, "      r%d = q[%d];\n", d + n, i); n++; }
            sp(&c->body, "    }\n");
        } else if (qt == 1) {  // dimension
            if (c->stage != STAGE_VERTEX) {  // report the guest size of scaled allocations
                c->uses_rscale = true;
                sp(&c->body, "    { uint lv = %s; uint4 q = uint4(uint(round(float(t%d.get_width(lv)) * rscale[%d].z / rscale[%d].x)), "
                   "uint(round(float(t%d.get_height(lv)) * rscale[%d].w / rscale[%d].y)), 1, t%d.get_num_mip_levels());\n",
                   R(a), slot, slot + 1, slot + 1, slot, slot + 1, slot + 1, slot);
            } else {
                sp(&c->body, "    { uint lv = %s; uint4 q = uint4(t%d.get_width(lv), t%d.get_height(lv), 1, t%d.get_num_mip_levels());\n",
                   R(a), slot, slot, slot);
            }
            int n = 0;
            for (int i = 0; i < 4; i++)
                if (mask & (1u << i)) { c->reg_used[d + n] = 1; sp(&c->body, "      r%d = q[%d];\n", d + n, i); n++; }
            sp(&c->body, "    }\n");
        } else {
            fail(c, "txq type %d", qt);
        }
    } else if (OPM(0x5080000000000000ull, 0xfff8000000000000ull)) {  // MUFU
        int fn = (int)bits(w, 20, 4);
        const char *x = NA(F(R(a)), bits(w, 48, 1), bits(w, 46, 1));
        switch (fn) {
        case 0: sprintf(t, "cos(%s)", x); break;
        case 1: sprintf(t, "sin(%s)", x); break;
        case 2: sprintf(t, "exp2(%s)", x); break;
        case 3: sprintf(t, "log2(%s)", x); break;
        case 4: sprintf(t, "(1.0f / %s)", x); break;
        case 5: sprintf(t, "rsqrt(%s)", x); break;
        case 8: sprintf(t, "sqrt(%s)", x); break;
        default: fail(c, "mufu %d", fn); return;
        }
        set_reg(c, d, FRES(t, bits(w, 50, 1)));
    } else if (OPM(0x5c90000000000000ull, 0xfff8000000000000ull) || OPM(0x4c90000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3890000000000000ull, 0xfef8000000000000ull)) {  // RRO: range reduction (no-op for us)
        const char *x = NA(F(B(true)), bits(w, 45, 1), bits(w, 49, 1));
        set_reg(c, d, FRES(x, false));
    } else if (OPM(0x5c58000000000000ull, 0xfff8000000000000ull) || OPM(0x4c58000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3858000000000000ull, 0xfef8000000000000ull)) {  // FADD
        sprintf(t, "(%s + %s)", NA(F(R(a)), bits(w, 48, 1), bits(w, 46, 1)), NA(F(B(true)), bits(w, 45, 1), bits(w, 49, 1)));
        set_reg(c, d, FRES(t, bits(w, 50, 1)));
    } else if (OPM(0x0800000000000000ull, 0xfc00000000000000ull)) {  // FADD32I
        sprintf(u, "as_type<float>(0x%08xu)", (uint32_t)bits(w, 20, 32));
        sprintf(t, "(%s + %s)", NA(F(R(a)), bits(w, 56, 1), bits(w, 54, 1)), NA(u, bits(w, 53, 1), bits(w, 57, 1)));
        set_reg(c, d, FRES(t, false));
    } else if (OPM(0x5c68000000000000ull, 0xfff8000000000000ull) || OPM(0x4c68000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3868000000000000ull, 0xfef8000000000000ull)) {  // FMUL
        static const char *const scale[] = {"", " * 0.5f", " * 0.25f", " * 0.125f", " * 8.0f", " * 4.0f", " * 2.0f", ""};
        sprintf(t, "(%s * %s%s)", F(R(a)), NA(F(B(true)), bits(w, 48, 1), false), scale[bits(w, 41, 3)]);
        set_reg(c, d, FRES(t, bits(w, 50, 1)));
    } else if (OPM(0x1e00000000000000ull, 0xff00000000000000ull)) {  // FMUL32I
        sprintf(t, "(%s * as_type<float>(0x%08xu))", F(R(a)), (uint32_t)bits(w, 20, 32));
        set_reg(c, d, FRES(t, bits(w, 55, 1)));
    } else if (OPM(0x5980000000000000ull, 0xff80000000000000ull) || OPM(0x4980000000000000ull, 0xff80000000000000ull) ||
               OPM(0x5180000000000000ull, 0xff80000000000000ull) || OPM(0x3280000000000000ull, 0xfe80000000000000ull)) {  // FFMA
        const char *bv;
        char cbuf_c[256], bbuf[256];
        const char *cv = operand_c_for_ffma(c, w, cbuf_c, &bv, bbuf);
        sprintf(t, "fma(%s, %s, %s)", F(R(a)), NA(F(bv), bits(w, 48, 1), false), NA(F(cv), bits(w, 49, 1), false));
        set_reg(c, d, FRES(t, bits(w, 50, 1)));
    } else if (OPM(0x0c00000000000000ull, 0xfc00000000000000ull)) {  // FFMA32I
        sprintf(u, "as_type<float>(0x%08xu)", (uint32_t)bits(w, 20, 32));
        sprintf(t, "fma(%s, %s, %s)", NA(F(R(a)), bits(w, 56, 1), false), u, NA(F(R(d)), bits(w, 57, 1), false));
        set_reg(c, d, FRES(t, bits(w, 55, 1)));
    } else if (OPM(0x5c60000000000000ull, 0xfff8000000000000ull) || OPM(0x4c60000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3860000000000000ull, 0xfef8000000000000ull)) {  // FMNMX
        const char *x = NA(F(R(a)), bits(w, 48, 1), bits(w, 46, 1)), *y = NA(F(B(true)), bits(w, 45, 1), bits(w, 49, 1));
        sprintf(t, "(%s ? fmin(%s, %s) : fmax(%s, %s))", P((int)bits(w, 39, 3), bits(w, 42, 1)), x, y, x, y);
        set_reg(c, d, FRES(t, false));
    } else if (OPM(0x5bb0000000000000ull, 0xfff0000000000000ull) || OPM(0x4bb0000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x36b0000000000000ull, 0xfef0000000000000ull)) {  // FSETP
        const char *x = NA(F(R(a)), bits(w, 43, 1), bits(w, 7, 1)), *y = NA(F(B(true)), bits(w, 6, 1), bits(w, 44, 1));
        char cmp[512];
        fcmp_expr(cmp, (int)bits(w, 48, 4), x, y);
        int p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3), bo = (int)bits(w, 45, 2);
        const char *pc_ = P((int)bits(w, 39, 3), bits(w, 42, 1));
        if (p != PT) { c->pred_used[p] = 1; sp(&c->body, "    { bool cmp = %s; bool pp = %s;\n      p%d = cmp %s pp;\n", cmp, pc_, p, bool_op(bo)); }
        else sp(&c->body, "    { bool cmp = %s; bool pp = %s;\n", cmp, pc_);
        if (q != PT) { c->pred_used[q] = 1; sp(&c->body, "      p%d = !cmp %s pp;\n", q, bool_op(bo)); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5800000000000000ull, 0xff00000000000000ull) || OPM(0x4800000000000000ull, 0xfe00000000000000ull) ||
               OPM(0x3000000000000000ull, 0xfe00000000000000ull)) {  // FSET
        const char *x = NA(F(R(a)), bits(w, 43, 1), bits(w, 54, 1)), *y = NA(F(B(true)), bits(w, 53, 1), bits(w, 44, 1));
        char cmp[512];
        fcmp_expr(cmp, (int)bits(w, 48, 4), x, y);
        sprintf(t, "((%s) %s %s)", cmp, bool_op((int)bits(w, 45, 2)), P((int)bits(w, 39, 3), bits(w, 42, 1)));
        sprintf(u, bits(w, 52, 1) ? "(%s ? 0x3f800000u : 0u)" : "(%s ? 0xFFFFFFFFu : 0u)", t);
        set_reg(c, d, u);
    } else if (OPM(0x5ba0000000000000ull, 0xfff0000000000000ull) || OPM(0x4ba0000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x36a0000000000000ull, 0xfef0000000000000ull) || OPM(0x53a0000000000000ull, 0xfff0000000000000ull)) {  // FCMP
        char cmp[512], cb[256], bb[256];
        const char *bv, *cv;
        if (OPM(0x53a0000000000000ull, 0xfff0000000000000ull)) {
            bv = R((int)bits(w, 39, 8));
            cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
        } else {
            bv = operand_b(c, w, true, bb);
            cv = R((int)bits(w, 39, 8));
        }
        fcmp_expr(cmp, (int)bits(w, 48, 4), F(cv), "0.0f");
        sprintf(t, "(%s ? %s : %s)", cmp, R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0x5c98000000000000ull, 0xfff8000000000000ull) || OPM(0x4c98000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3898000000000000ull, 0xfef8000000000000ull)) {  // MOV
        set_reg(c, d, B(false));
        if (b_form(w) == B_CBUF && bits(w, 34, 5) < 18) {
            c->image_origin_dst = d;
            c->image_origin_new = ((uint32_t)(bits(w, 34, 5) + 1) << 16) | ((uint32_t)bits(w, 20, 14) * 4);
        } else if (b_form(w) == B_REG) {
            c->image_origin_dst = d; c->image_origin_new = c->image_origin[bits(w, 20, 8)];
        }
        tz_result(c, d, tz_of_b(c, w));
    } else if (OPM(0x0100000000000000ull, 0xfff0000000000000ull)) {  // MOV32I
        sprintf(t, "0x%08xu", (uint32_t)bits(w, 20, 32));
        set_reg(c, d, t);
        tz_result(c, d, ctz_u32((uint32_t)bits(w, 20, 32)));
    } else if (OPM(0x5ca0000000000000ull, 0xfff8000000000000ull) || OPM(0x4ca0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38a0000000000000ull, 0xfef8000000000000ull)) {  // SEL
        sprintf(t, "(%s ? %s : %s)", P((int)bits(w, 39, 3), bits(w, 42, 1)), R(a), B(false));
        set_reg(c, d, t);
    } else if (OPM(0x5cb0000000000000ull, 0xfff8000000000000ull) || OPM(0x4cb0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38b0000000000000ull, 0xfef8000000000000ull)) {  // F2I (src fmt bits 10-11: 1 F16; dst size bits 8-9)
        char srcv[512];
        if (bits(w, 10, 2) == 1 && c->glsl) sprintf(srcv, "h2f((%s >> %du) & 0xFFFFu)", B(false), 16 * (int)bits(w, 41, 1));
        else if (bits(w, 10, 2) == 1) sprintf(srcv, "float(as_type<half2>(%s)[%d])", B(false), (int)bits(w, 41, 1));
        else strcpy(srcv, F(B(true)));
        const char *x = NA(srcv, bits(w, 45, 1), bits(w, 49, 1));
        int rnd = (int)bits(w, 39, 2);
        bool sgn = bits(w, 12, 1), i16 = bits(w, 8, 2) == 1;
        static const char *const rf[] = {"rint", "floor", "ceil", "trunc"};
        if (i16)
            sprintf(t, sgn ? "uint(int(clamp(%s(%s), -32768.0f, 32767.0f)))" : "uint(clamp(%s(%s), 0.0f, 65535.0f))", rf[rnd], x);
        else
            sprintf(t, sgn ? "uint(int(clamp(%s(%s), -2147483648.0f, 2147483520.0f)))" : "uint(clamp(%s(%s), 0.0f, 4294967040.0f))", rf[rnd], x);
        set_reg(c, d, t);
    } else if (OPM(0x5cb8000000000000ull, 0xfff8000000000000ull) || OPM(0x4cb8000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38b8000000000000ull, 0xfef8000000000000ull)) {  // I2F
        bool sgn = bits(w, 13, 1);
        int sz = (int)bits(w, 10, 2), sel = (int)bits(w, 41, 2);
        char src[256];
        const char *bv = B(false);
        if (sz == 0) sprintf(src, sgn ? "int(char((%s >> %d) & 0xFF))" : "((%s >> %d) & 0xFFu)", bv, sel * 8);
        else if (sz == 1) sprintf(src, sgn ? "int(short((%s >> %d) & 0xFFFF))" : "((%s >> %d) & 0xFFFFu)", bv, sel * 8);
        else sprintf(src, sgn ? "int(%s)" : "%s", bv);
        sprintf(u, "float(%s)", src);
        if (bits(w, 8, 2) == 1) {  // F16 result in the low half
            char h[700];
            snprintf(h, sizeof h, c->glsl ? "(packHalf2x16(vec2(%s, 0.0f)))" : "(uint(as_type<ushort>(half(%s))))",
                     NA(u, bits(w, 45, 1), bits(w, 49, 1)));
            set_reg(c, d, h);
        } else {
            set_reg(c, d, FRES(NA(u, bits(w, 45, 1), bits(w, 49, 1)), false));
        }
    } else if (OPM(0x5ca8000000000000ull, 0xfff8000000000000ull) || OPM(0x4ca8000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38a8000000000000ull, 0xfef8000000000000ull)) {  // F2F (formats: 1 F16, 2 F32; bit 41 = high half)
        int dfmt = (int)bits(w, 8, 2), sfmt = (int)bits(w, 10, 2);
        char srcv[512];
        int mode = (int)((bits(w, 39, 2)) | (bits(w, 42, 1) << 2));
        static const char *const rf[] = {"rint", "floor", "ceil", "trunc"};
        static int f2f_on = -1;  // LARECOMP_VK_HALF_F2F=0: halves converted through the uint, as before (comparisons)
        if (f2f_on < 0) f2f_on = getenv("LARECOMP_VK_HALF_F2F") ? atoi(getenv("LARECOMP_VK_HALF_F2F")) != 0 : 1;
        if (f2f_on && hshadow_on(c) && !c->in_guard && b_form(w) == B_REG && (sfmt == 1 || dfmt == 1) && (sfmt == 1 || sfmt == 2) &&
            (dfmt == 1 || dfmt == 2)) {
            // A half in or out through the shadows: the operand's half (or the float, rounded then converted), rounded
            // as float16_t, into h<d> with the high half zero as the instruction leaves it - no pack, unpack or
            // conversion to f32 and back; an F32 result into the uint
            int rb = (int)bits(w, 20, 8);
            char sx[800], v[900];
            if (sfmt == 1) {
                sprintf(sx, "(%s).x", HS(RH(rb), bits(w, 41, 1) ? 3 : 2, bits(w, 45, 1), bits(w, 49, 1)));
                if (mode >= 4) sprintf(v, "%s(%s)", rf[mode - 4], sx); else strcpy(v, sx);
            } else {
                const char *fx = NA(F(R(rb)), bits(w, 45, 1), bits(w, 49, 1));
                if (mode >= 4) sprintf(sx, "%s(%s)", rf[mode - 4], fx); else strcpy(sx, fx);
                sprintf(v, "float16_t(%s)", sx);
            }
            if (dfmt == 1) {
                if (d != RZ) {
                    c->reg_used[d] = 1, c->huse[d] = true;
                    sp(&c->body, bits(w, 50, 1) ? "    h%d = saturate(f16vec2(%s, 0.0hf));\n" : "    h%d = f16vec2(%s, 0.0hf);\n", d, v);
                    c->hstate[d] = 1, c->hcstate[d] = 0;
                    c->image_origin[d] = 0;
                }
            } else {
                sprintf(t, "float(%s)", v);
                set_reg(c, d, FRES(t, bits(w, 50, 1)));
            }
            goto insn_done;
        }
        if (sfmt == 1 && c->glsl) sprintf(srcv, "h2f((%s >> %du) & 0xFFFFu)", B(false), 16 * (int)bits(w, 41, 1));
        else if (sfmt == 1) sprintf(srcv, "float(as_type<half2>(%s)[%d])", B(false), (int)bits(w, 41, 1));
        else strcpy(srcv, F(B(true)));
        const char *x = NA(srcv, bits(w, 45, 1), bits(w, 49, 1));
        if (mode >= 4) sprintf(t, "%s(%s)", rf[mode - 4], x);
        else sprintf(t, "%s", x);
        if (dfmt == 1) {  // F16 result in the low half, high half zero
            char h[1100];
            snprintf(h, sizeof h, c->glsl ? "(packHalf2x16(vec2(%s, 0.0f)))" : "(uint(as_type<ushort>(half(%s))))",
                     bits(w, 50, 1) ? (sprintf(u, "saturate(%s)", t), u) : t);
            set_reg(c, d, h);
        } else {
            set_reg(c, d, FRES(t, bits(w, 50, 1)));
        }
    } else if (OPM(0x5c10000000000000ull, 0xfff8000000000000ull) || OPM(0x4c10000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3810000000000000ull, 0xfef8000000000000ull)) {  // IADD
        if (bits(w, 43, 1) || bits(w, 47, 1)) c->uses_cc = true;
        tz_result(c, d, bits(w, 43, 1) ? 0 : TZMIN(TZR(a), tz_of_b(c, w)));  // (negation keeps trailing zeros)
        sprintf(t, "(%s%s + %s%s%s)", bits(w, 49, 1) ? "-" : "", R(a), bits(w, 48, 1) ? "-" : "", B(false),
                bits(w, 43, 1) ? " + cc" : "");
        if (bits(w, 47, 1)) {
            sp(&c->body, "    { uint s = %s%s; uint t2 = %s%s; uint res = s + t2%s; cc = (res < s) ? 1u : 0u;\n", bits(w, 49, 1) ? "-" : "",
               R(a), bits(w, 48, 1) ? "-" : "", B(false), bits(w, 43, 1) ? " + cc" : "");
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = res;\n", d); }
            sp(&c->body, "    }\n");
        } else {
            set_reg(c, d, t);
        }
    } else if (OPM(0x1c00000000000000ull, 0xfe00000000000000ull)) {  // IADD32I (CC bit 52, X bit 53)
        bool wcc = bits(w, 52, 1), x = bits(w, 53, 1);
        if (wcc || x) c->uses_cc = true;
        tz_result(c, d, x ? 0 : TZMIN(TZR(a), ctz_u32((uint32_t)bits(w, 20, 32))));
        if (c->glsl && !maxwell_shader_int64)
            sp(&c->body, "    { uint carry; uint sum = add32(%s%s, 0x%08xu, %s, carry);%s\n",
               bits(w, 56, 1) ? "-" : "", R(a), (uint32_t)bits(w, 20, 32), x ? "cc" : "0u", wcc ? " cc = carry;" : "");
        else sp(&c->body, "    { ulong sum = ulong(%s%s) + ulong(0x%08xu)%s;%s\n", bits(w, 56, 1) ? "-" : "", R(a), (uint32_t)bits(w, 20, 32),
                x ? " + ulong(cc)" : "", wcc ? " cc = uint(sum >> 32) & 1u;" : "");
        if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = uint(sum);\n", d); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5cc0000000000000ull, 0xfff0000000000000ull) || OPM(0x4cc0000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x38c0000000000000ull, 0xfef0000000000000ull)) {  // IADD3
        bool wcc3 = bits(w, 47, 1), x3 = bits(w, 48, 1);
        tz_result(c, d, x3 ? 0 : TZMIN(TZMIN(TZR(a), tz_of_b(c, w)), TZR((int)bits(w, 39, 8))));
        if (wcc3 || x3) {  // carry in/out (64-bit address math)
            c->uses_cc = true;
            if (c->glsl && !maxwell_shader_int64)
                sp(&c->body, "    { uint carry; uint sum = add3_32(%s%s, %s%s, %s%s, %s, carry);\n",
                   bits(w, 51, 1) ? "-" : "", R(a), bits(w, 50, 1) ? "-" : "", B(false),
                   bits(w, 49, 1) ? "-" : "", R((int)bits(w, 39, 8)), x3 ? "cc" : "0u");
            else sp(&c->body, "    { ulong sum = ulong(%s%s) + ulong(%s%s) + ulong(%s%s)%s;\n", bits(w, 51, 1) ? "-" : "", R(a),
                    bits(w, 50, 1) ? "-" : "", B(false), bits(w, 49, 1) ? "-" : "", R((int)bits(w, 39, 8)), x3 ? " + ulong(cc)" : "");
            if (wcc3) sp(&c->body, c->glsl && !maxwell_shader_int64 ? "      cc = carry;\n" : "      cc = uint(sum >> 32) & 1u;\n");
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = uint(sum);\n", d); }
            sp(&c->body, "    }\n");
        } else {
            sprintf(t, "(%s%s + %s%s + %s%s)", bits(w, 51, 1) ? "-" : "", R(a), bits(w, 50, 1) ? "-" : "", B(false),
                    bits(w, 49, 1) ? "-" : "", R((int)bits(w, 39, 8)));
            set_reg(c, d, t);
        }
    } else if (OPM(0x5c18000000000000ull, 0xfff8000000000000ull) || OPM(0x4c18000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3818000000000000ull, 0xfef8000000000000ull)) {  // ISCADD
        sprintf(t, "((%s%s << %du) + %s%s)", bits(w, 49, 1) ? "-" : "", R(a), (int)bits(w, 39, 5), bits(w, 48, 1) ? "-" : "",
                B(false));
        tz_result(c, d, TZMIN(TZADD(TZR(a), (int)bits(w, 39, 5)), tz_of_b(c, w)));
        if (bits(w, 47, 1)) {  // .CC: carry out for a following IADD.X (64-bit address math)
            c->uses_cc = true;
            sp(&c->body, "    { uint s = %s%s << %du; uint t2 = %s%s; uint res = s + t2; cc = (res < s) ? 1u : 0u;\n",
               bits(w, 49, 1) ? "-" : "", R(a), (int)bits(w, 39, 5), bits(w, 48, 1) ? "-" : "", B(false));
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = res;\n", d); }
            sp(&c->body, "    }\n");
        } else {
            set_reg(c, d, t);
        }
    } else if (OPM(0x1400000000000000ull, 0xfc00000000000000ull)) {  // ISCADD32I
        sprintf(t, "((%s << %du) + 0x%08xu)", R(a), (int)bits(w, 53, 5), (uint32_t)bits(w, 20, 32));
        set_reg(c, d, t);
        tz_result(c, d, TZMIN(TZADD(TZR(a), (int)bits(w, 53, 5)), ctz_u32((uint32_t)bits(w, 20, 32))));
    } else if (OPM(0x5b60000000000000ull, 0xfff0000000000000ull) || OPM(0x4b60000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x3660000000000000ull, 0xfef0000000000000ull)) {  // ISETP
        char cmp[512];
        icmp_expr(cmp, (int)bits(w, 49, 3), R(a), B(false), bits(w, 48, 1));
        int p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3), bo = (int)bits(w, 45, 2);
        const char *pp = P((int)bits(w, 39, 3), bits(w, 42, 1));
        sp(&c->body, "    { bool cmp = %s; bool pp = %s;\n", cmp, pp);
        if (p != PT) { c->pred_used[p] = 1; sp(&c->body, "      p%d = cmp %s pp;\n", p, bool_op(bo)); }
        if (q != PT) { c->pred_used[q] = 1; sp(&c->body, "      p%d = !cmp %s pp;\n", q, bool_op(bo)); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5b50000000000000ull, 0xfff0000000000000ull) || OPM(0x4b50000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x3650000000000000ull, 0xfef0000000000000ull)) {  // ISET
        char cmp[512];
        icmp_expr(cmp, (int)bits(w, 49, 3), R(a), B(false), bits(w, 48, 1));
        sprintf(t, "((%s) %s %s)", cmp, bool_op((int)bits(w, 45, 2)), P((int)bits(w, 39, 3), bits(w, 42, 1)));
        sprintf(u, bits(w, 44, 1) ? "(%s ? 0x3f800000u : 0u)" : "(%s ? 0xFFFFFFFFu : 0u)", t);
        set_reg(c, d, u);
    } else if (OPM(0x3a00000000000000ull, 0xfe00000000000000ull)) {  // VMNMX: scalar video min/max, then MIN/MAX with C
        unsigned mode = (unsigned)bits(w, 51, 3);
        if (bits(w, 47, 1) || bits(w, 55, 1) || (mode != 5 && mode != 6)) {
            fail(c, "unsupported VMNMX CC/SAT/merge/accumulate mode"); return;
        }
        char av[256], bv[256], raw_b[64];
        video_operand(av, sizeof av, R(a), (unsigned)bits(w, 37, 2), (unsigned)bits(w, 36, 2), bits(w, 48, 1));
        bool breg = bits(w, 50, 1);
        if (breg) snprintf(raw_b, sizeof raw_b, "%s", R((int)bits(w, 20, 8)));
        else snprintf(raw_b, sizeof raw_b, "0x%04xu", (unsigned)bits(w, 20, 16));
        video_operand(bv, sizeof bv, raw_b, breg ? (unsigned)bits(w, 29, 2) : 2,
                      breg ? (unsigned)bits(w, 28, 2) : 0, bits(w, 49, 1));
        const char *first = bits(w, 56, 1) ? "max" : "min";
        if (bits(w, 49, 1)) snprintf(t, sizeof t, "uint(%s(int(%s), int(%s)))", first, av, bv);
        else snprintf(t, sizeof t, "%s(%s, %s)", first, av, bv);
        const char *second = mode == 6 ? "max" : "min";
        if (bits(w, 54, 1)) snprintf(u, sizeof u, "uint(%s(int(%s), int(%s)))", second, t, R((int)bits(w, 39, 8)));
        else snprintf(u, sizeof u, "%s(%s, %s)", second, t, R((int)bits(w, 39, 8)));
        set_reg(c, d, u);
    } else if (OPM(0x5c20000000000000ull, 0xfff8000000000000ull) || OPM(0x4c20000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3820000000000000ull, 0xfef8000000000000ull)) {  // IMNMX
        bool sgn = bits(w, 48, 1);
        const char *x = R(a), *y = B(false), *p = P((int)bits(w, 39, 3), bits(w, 42, 1));
        if (sgn) sprintf(t, "uint(%s ? min(int(%s), int(%s)) : max(int(%s), int(%s)))", p, x, y, x, y);
        else sprintf(t, "(%s ? min(%s, %s) : max(%s, %s))", p, x, y, x, y);
        set_reg(c, d, t);
    } else if (OPM(0x5c48000000000000ull, 0xfff8000000000000ull) || OPM(0x4c48000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3848000000000000ull, 0xfef8000000000000ull)) {  // SHL
        const char *bv = B(false);
        if (bits(w, 39, 1)) sprintf(t, "(%s << (%s & 31u))", R(a), bv);
        else sprintf(t, "((%s) >= 32u ? 0u : (%s << (%s)))", bv, R(a), bv);
        set_reg(c, d, t);
        if (b_form(w) == B_IMM) {
            uint32_t s = (uint32_t)bits(w, 20, 19) | ((uint32_t)bits(w, 56, 1) << 19);
            tz_result(c, d, bits(w, 39, 1) ? TZADD(TZR(a), (int)(s & 31)) : s >= 32 ? 32 : TZADD(TZR(a), (int)s));
        }
    } else if (OPM(0x5c28000000000000ull, 0xfff8000000000000ull) || OPM(0x4c28000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3828000000000000ull, 0xfef8000000000000ull)) {  // SHR
        const char *bv = B(false);
        bool sgn = bits(w, 48, 1);
        if (sgn) sprintf(t, "uint(int(%s) >> min(%s, 31u))", R(a), bv);
        else sprintf(t, "((%s) >= 32u ? 0u : (%s >> (%s)))", bv, R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0x5c40000000000000ull, 0xfff8000000000000ull) || OPM(0x4c40000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3840000000000000ull, 0xfef8000000000000ull)) {  // LOP
        char x[64], y[256];
        sprintf(x, bits(w, 39, 1) ? "(~%s)" : "%s", R(a));
        sprintf(y, bits(w, 40, 1) ? "(~%s)" : "%s", B(false));
        static const char *const ops[] = {"&", "|", "^", ""};
        int op = (int)bits(w, 41, 2);
        if (op == 3) sprintf(t, "%s", y);
        else sprintf(t, "(%s %s %s)", x, ops[op], y);
        if (!bits(w, 39, 1) && !bits(w, 40, 1)) {
            int ta = TZR(a), tb = tz_of_b(c, w);
            tz_result(c, d, op == 0 ? (ta > tb ? ta : tb) : op == 3 ? tb : TZMIN(ta, tb));
        }
        int pd = (int)bits(w, 48, 3);
        if (pd != PT) {
            c->pred_used[pd] = 1;
            int pop = (int)bits(w, 44, 2);
            sp(&c->body, "    { uint res = %s;\n", t);
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = res;\n", d); }
            sp(&c->body, "      p%d = %s; }\n", pd, pop == 2 ? "(res == 0u)" : pop == 3 ? "(res != 0u)" : pop == 1 ? "true" : "false");
        } else {
            set_reg(c, d, t);
        }
    } else if (OPM(0x0400000000000000ull, 0xfc00000000000000ull)) {  // LOP32I
        char x[64], y[64];
        sprintf(x, bits(w, 55, 1) ? "(~%s)" : "%s", R(a));
        sprintf(y, bits(w, 56, 1) ? "(~0x%08xu)" : "0x%08xu", (uint32_t)bits(w, 20, 32));
        static const char *const ops[] = {"&", "|", "^", ""};
        int op = (int)bits(w, 53, 2);
        if (op == 3) sprintf(t, "%s", y);
        else sprintf(t, "(%s %s %s)", x, ops[op], y);
        set_reg(c, d, t);
        if (!bits(w, 55, 1) && !bits(w, 56, 1)) {
            int ta = TZR(a), tb = ctz_u32((uint32_t)bits(w, 20, 32));
            tz_result(c, d, op == 0 ? (ta > tb ? ta : tb) : op == 3 ? tb : TZMIN(ta, tb));
        }
    } else if (OPM(0x5be0000000000000ull, 0xfff8000000000000ull) || OPM(0x3c00000000000000ull, 0xfc00000000000000ull) ||
               OPM(0x0200000000000000ull, 0xfe00000000000000ull)) {  // LOP3
        uint32_t lut;
        const char *bv, *cv;
        char bb[256];
        if ((w >> 56) == 0x5b) { lut = (uint32_t)bits(w, 28, 8); bv = R((int)bits(w, 20, 8)); cv = R((int)bits(w, 39, 8)); }
        else if ((w >> 58) == 0xf) { lut = (uint32_t)bits(w, 48, 8); sprintf(bb, "0x%08xu", (uint32_t)((uint32_t)bits(w, 20, 19) | ((uint32_t)bits(w, 56, 1) << 19) | (bits(w, 56, 1) ? 0xFFF00000u : 0))); bv = bb; cv = R((int)bits(w, 39, 8)); }
        else { lut = (uint32_t)bits(w, 48, 8); bv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, bb); cv = R((int)bits(w, 39, 8)); }
        char e[1024] = "0u";
        // sum of minterms
        int first = 1;
        for (int i = 0; i < 8; i++) {
            if (!(lut & (1u << i))) continue;
            char term[256];
            sprintf(term, "(%s%s & %s%s & %s%s)", (i & 4) ? "" : "~", R(a), (i & 2) ? "" : "~", bv, (i & 1) ? "" : "~", cv);
            if (first) { strcpy(e, term); first = 0; }
            else { strcat(e, " | "); strcat(e, term); }
        }
        sprintf(t, "(%s)", e);
        set_reg(c, d, t);
    } else if (OPM(0x5c00000000000000ull, 0xfff8000000000000ull) || OPM(0x4c00000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3800000000000000ull, 0xfef8000000000000ull)) {  // BFE
        const char *bv = B(false);
        bool sgn = bits(w, 48, 1);
        // .BREV reverses the source before extraction (EoWRecomp / Ryujinx EmitBfe).
        if (bits(w, 40, 1)) sprintf(t, sgn ? "bfe_s(brev32(%s), %s)" : "bfe_u(brev32(%s), %s)", R(a), bv);
        else sprintf(t, sgn ? "bfe_s(%s, %s)" : "bfe_u(%s, %s)", R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0x5bf0000000000000ull, 0xfff8000000000000ull) || OPM(0x4bf0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x36f0000000000000ull, 0xfef8000000000000ull) || OPM(0x53f0000000000000ull, 0xfff8000000000000ull)) {  // BFI
        const char *bv, *cv;
        char cb[256];
        if (OPM(0x53f0000000000000ull, 0xfff8000000000000ull)) {
            bv = R((int)bits(w, 39, 8)); cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
            // bfi with cbuf form: B = REG_39 as position/length, C = cbuf as base
            sprintf(t, "bfi(%s, %s, %s)", R(a), bv, cv);
        } else {
            bv = B(false); cv = R((int)bits(w, 39, 8));
            sprintf(t, "bfi(%s, %s, %s)", R(a), bv, cv);
        }
        set_reg(c, d, t);
    } else if (OPM(0x5b00000000000000ull, 0xffc0000000000000ull) || OPM(0x4e00000000000000ull, 0xfe00000000000000ull) ||
               OPM(0x3600000000000000ull, 0xfec0000000000000ull) || OPM(0x5100000000000000ull, 0xff80000000000000ull)) {  // XMAD
        // per Ryujinx's InstXmad* decoders and EmitXmad
        const char *bv, *cv;
        char cb[256], bb[256];
        bool hb, psl, mrg, ext, ha = bits(w, 53, 1);
        int cmode;
        uint64_t top = w >> 56;
        if (top == 0x5b) {  // reg, reg
            bv = R((int)bits(w, 20, 8)); cv = R((int)bits(w, 39, 8));
            hb = bits(w, 35, 1); psl = bits(w, 36, 1); mrg = bits(w, 37, 1); ext = bits(w, 38, 1); cmode = (int)bits(w, 50, 3);
        } else if ((top & 0xfe) == 0x4e) {  // cbuf B
            bv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb); cv = R((int)bits(w, 39, 8));
            hb = bits(w, 52, 1); psl = bits(w, 55, 1); mrg = bits(w, 56, 1); ext = bits(w, 54, 1); cmode = (int)bits(w, 50, 2);
        } else if ((top & 0xfe) == 0x36) {  // imm16 B
            sprintf(bb, "0x%xu", (uint32_t)bits(w, 20, 16)); bv = bb; cv = R((int)bits(w, 39, 8));
            hb = false; psl = bits(w, 36, 1); mrg = bits(w, 37, 1); ext = bits(w, 38, 1); cmode = (int)bits(w, 50, 3);
        } else {  // 0x51: B = reg39, C = cbuf
            bv = R((int)bits(w, 39, 8)); cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
            hb = bits(w, 52, 1); psl = false; mrg = false; ext = bits(w, 54, 1); cmode = (int)bits(w, 50, 2);
        }
        bool sa = bits(w, 48, 1), sb = bits(w, 49, 1), wcc = bits(w, 47, 1);
        if (ext || wcc) c->uses_cc = true;
        // a.lo16 * imm (cfull, C = RZ), then ((a.hi16 * imm) << 16) + that: a * imm mod 2^32 - one multiply (three
        // instructions on Adreno) instead of the second half's expansion. Unsigned halves only (sign-extended ones
        // give another sum), nothing merged, no carry, one block, no guard.
        static int xmul_on = -1;  // LARECOMP_VK_XMAD_MUL=0: both halves expanded, as before (comparisons)
        if (xmul_on < 0) xmul_on = getenv("LARECOMP_VK_XMAD_MUL") ? atoi(getenv("LARECOMP_VK_XMAD_MUL")) != 0 : 1;
        if (xmul_on && c->glsl && (top & 0xfe) == 0x36 && !sa && !sb && !mrg && !ext && !wcc && !guarded && cmode == 0) {
            uint32_t imm = (uint32_t)bits(w, 20, 16);
            int cr = (int)bits(w, 39, 8);
            if (!ha && !psl && cr == RZ) {
                c->xmad_lo_pc = 0, c->xmad_new = true, c->xmad_a = a, c->xmad_d = d, c->xmad_imm = imm;
            } else if (ha && psl && c->xmad_lo_pc && c->xmad_a == a && c->xmad_imm == imm && cr == c->xmad_d) {
                c->xmad_lo_pc = 0;
                sprintf(t, "(%s * 0x%xu)", R(a), imm);
                set_reg(c, d, t);
                tz_result(c, d, TZADD(TZR(a), ctz_u32(imm)));
                goto insn_done;
            }
        }
        {   // trailing zeros of the result: the halves' add up in the product (sign extension keeps them; PSL adds 16),
            // the C term's per cmode; a merge or a carry in: nothing known
            int cr = top == 0x51 ? -1 : (int)bits(w, 39, 8);
            int tb = (top & 0xfe) == 0x36 ? ctz_u32((uint32_t)bits(w, 20, 16)) : top == 0x5b ? tz_half(TZR((int)bits(w, 20, 8)), hb) : 0;
            int tp = TZADD(tz_half(TZR(a), ha), tb);
            if (psl) tp = TZADD(tp, 16);
            int tc = cr < 0 ? 0 : cmode == 0 ? TZR(cr) : cmode == 1 ? tz_half(TZR(cr), false) : cmode == 2 ? tz_half(TZR(cr), true) : 0;
            tz_result(c, d, ext || mrg ? 0 : TZMIN(tp, tc));
        }
        sp(&c->body, "    { uint xa = (%s >> %d) & 0xFFFFu; uint xb = (%s >> %d) & 0xFFFFu; uint xbu = %s;\n", R(a), ha ? 16 : 0, bv, hb ? 16 : 0, bv);
        if (sa) sp(&c->body, "      xa = uint(int(short(xa)));\n");
        if (sb) sp(&c->body, "      xb = uint(int(short(xb)));\n");
        sp(&c->body, "      uint prod = xa * xb;%s\n", psl ? " prod <<= 16;" : "");
        switch (cmode) {
        case 1: sp(&c->body, "      uint cc_ = %s & 0xFFFFu;\n", cv); break;                                  // clo
        case 2: sp(&c->body, "      uint cc_ = %s >> 16;\n", cv); break;                                      // chi
        case 3: sp(&c->body, "      uint cc_ = %s - (((xa >> 31) << 16) + ((xb >> 31) << 16));\n", cv); break;  // csfu
        case 4: sp(&c->body, "      uint cc_ = %s + (xbu << 16);\n", cv); break;                              // cbcc
        default: sp(&c->body, "      uint cc_ = %s;\n", cv);                                                  // cfull
        }
        if (ext) sp(&c->body, "      uint res = prod + (cc & 1u);\n");
        else sp(&c->body, "      uint res = prod + cc_;\n");
        if (wcc) sp(&c->body, "      cc = (res < prod) ? 1u : 0u;\n");
        if (mrg) sp(&c->body, "      res = (res & 0xFFFFu) | (xbu << 16);\n");
        if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = res;\n", d); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5090000000000000ull, 0xfff8000000000000ull)) {  // PSETP
        const char *pa = P((int)bits(w, 12, 3), bits(w, 15, 1)), *pb = P((int)bits(w, 29, 3), bits(w, 32, 1));
        const char *pcc = P((int)bits(w, 39, 3), bits(w, 42, 1));
        int bo1 = (int)bits(w, 24, 2), bo2 = (int)bits(w, 45, 2);
        int p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3);
        sp(&c->body, "    { bool v = (%s %s %s); bool pp = %s;\n", pa, bool_op(bo1), pb, pcc);
        if (p != PT) { c->pred_used[p] = 1; sp(&c->body, "      p%d = v %s pp;\n", p, bool_op(bo2)); }
        if (q != PT) { c->pred_used[q] = 1; sp(&c->body, "      p%d = !v %s pp;\n", q, bool_op(bo2)); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5088000000000000ull, 0xfff8000000000000ull)) {  // PSET
        const char *pa = P((int)bits(w, 12, 3), bits(w, 15, 1)), *pb = P((int)bits(w, 29, 3), bits(w, 32, 1));
        sprintf(t, "(((%s %s %s)) %s %s)", pa, bool_op((int)bits(w, 24, 2)), pb, bool_op((int)bits(w, 45, 2)),
                P((int)bits(w, 39, 3), bits(w, 42, 1)));
        sprintf(u, bits(w, 44, 1) ? "(%s ? 0x3f800000u : 0u)" : "(%s ? 0xFFFFFFFFu : 0u)", t);
        set_reg(c, d, u);
    } else if (OPM(0xf0c8000000000000ull, 0xfff8000000000000ull)) {  // S2R
        int sr = (int)bits(w, 20, 8);
        switch (sr) {
        case 0x12: set_reg(c, d, "0xbf800000u"); break;  // y_direction: -1 (lower-left)
        case 0x21: set_reg(c, d, "tid.x"); break;
        case 0x22: set_reg(c, d, "tid.y"); break;
        case 0x23: set_reg(c, d, "tid.z"); break;
        case 0x25: set_reg(c, d, "ctaid.x"); break;
        case 0x26: set_reg(c, d, "ctaid.y"); break;
        case 0x27: set_reg(c, d, "ctaid.z"); break;
        case 0x00: set_reg(c, d, "0u"); break;
        default: sprintf(t, "0u /* sr %#x */", sr); set_reg(c, d, t);
        }
    } else if (OPM(0x5c30000000000000ull, 0xfff8000000000000ull) || OPM(0x4c30000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3830000000000000ull, 0xfef8000000000000ull)) {  // FLO
        // FLO[.U32/.S32][.SH] Rd, ~?B: the most significant 1 (signed: of a negative value, the most significant 0), or
        // 0xFFFFFFFF if none; .SH (bit 41) gives the shift instead, 31 - position (Ryujinx EmitFlo). .INV (bit 40)
        // complements B first. (.SH was missing: Echoes of Wisdom walks its light masks with FLO.SH of a BREV - the
        // lowest set bit - and the wrong bit was cleared: the loop ran far too long and lit wrongly.)
        char vb[300];
        sprintf(vb, bits(w, 40, 1) ? "(~%s)" : "%s", B(false));
        char msb[1024];
        if (bits(w, 48, 1)) sprintf(msb, "(int(%s) < 0 ? (31u - clz(~%s)) : (31u - clz(%s)))", vb, vb, vb);
        else sprintf(msb, "(31u - clz(%s))", vb);
        if (bits(w, 41, 1)) sprintf(t, "flo_sh(%s)", msb);
        else strcpy(t, msb);
        set_reg(c, d, t);
    } else if (OPM(0x5c08000000000000ull, 0xfff8000000000000ull) || OPM(0x4c08000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3808000000000000ull, 0xfef8000000000000ull)) {  // POPC
        sprintf(t, bits(w, 40, 1) ? "popcount(~%s)" : "popcount(%s)", B(false));
        set_reg(c, d, t);
    } else if (OPM(0x5c38000000000000ull, 0xfff8000000000000ull) || OPM(0x4c38000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3838000000000000ull, 0xfef8000000000000ull)) {  // IMUL
        if (bits(w, 39, 1)) sprintf(t, bits(w, 40, 1) ? "uint(mulhi(int(%s), int(%s)))" : "mulhi(%s, %s)", R(a), B(false));
        else sprintf(t, "(%s * %s)", R(a), B(false));
        set_reg(c, d, t);
        if (!bits(w, 39, 1)) tz_result(c, d, TZADD(TZR(a), tz_of_b(c, w)));
    } else if (OPM(0x1f00000000000000ull, 0xff00000000000000ull)) {  // IMUL32I
        sprintf(t, "(%s * 0x%08xu)", R(a), (uint32_t)bits(w, 20, 32));
        set_reg(c, d, t);
        tz_result(c, d, TZADD(TZR(a), ctz_u32((uint32_t)bits(w, 20, 32))));
    } else if (OPM(0x5ce0000000000000ull, 0xfff8000000000000ull) || OPM(0x4ce0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38e0000000000000ull, 0xfef8000000000000ull)) {  // I2I
        int dt = (int)((bits(w, 8, 2)) | (bits(w, 12, 1) << 2)), st = (int)((bits(w, 10, 2)) | (bits(w, 13, 1) << 2));
        int sel = (int)bits(w, 41, 2);
        char src[256];
        const char *bv = B(false);
        int ssz = st & 3;
        bool ssg = st & 4;
        if (ssz == 0) sprintf(src, ssg ? "int(char((%s >> %d) & 0xFFu))" : "int((%s >> %d) & 0xFFu)", bv, sel * 8);
        else if (ssz == 1) sprintf(src, ssg ? "int(short((%s >> %d) & 0xFFFFu))" : "int((%s >> %d) & 0xFFFFu)", bv, sel * 8);
        else sprintf(src, "int(%s)", bv);
        if (bits(w, 49, 1)) { sprintf(u, "abs(%s)", src); strcpy(src, u); }
        if (bits(w, 45, 1)) { sprintf(u, "(-%s)", src); strcpy(src, u); }
        int dsz = dt & 3;
        bool dsg = dt & 4, sat = bits(w, 50, 1);
        if (sat && !ssg) {  // .SAT from an unsigned source: compare unsigned (a U32 value above 0x7FFFFFFF isn't negative)
            static const char *const hi[2][3] = {{"0xFFu", "0xFFFFu", ""}, {"0x7Fu", "0x7FFFu", "0x7FFFFFFFu"}};
            if (dsz < 2 || dsg) sprintf(t, "min(uint(%s), %s)", src, hi[dsg][dsz < 2 ? dsz : 2]);
            else sprintf(t, "uint(%s)", src);
        } else if (dsz == 0) sprintf(t, sat ? (dsg ? "uint(clamp(%s, -128, 127))" : "uint(clamp(%s, 0, 255))") : "(uint(%s) & 0xFFu)", src);
        else if (dsz == 1) sprintf(t, sat ? (dsg ? "uint(clamp(%s, -32768, 32767))" : "uint(clamp(%s, 0, 65535))") : "(uint(%s) & 0xFFFFu)", src);
        else if (sat && !dsg) sprintf(t, "uint(max(%s, 0))", src);  // S32 -> U32 .SAT: negatives become 0
        else sprintf(t, "uint(%s)", src);
        set_reg(c, d, t);
        if (bits(w, 47, 1)) {  // .CC: zero/sign flags of the result
            c->uses_cc = true;
            sp(&c->body, "    { uint ccv = %s; ccz = ccv == 0u; ccs = int(ccv) < 0; }\n", t);
        }
    } else if (OPM(0x5bd8000000000000ull, 0xfff8000000000000ull)) {  // LEA.HI Rd, A, B, C, shift (register form)
        // Rd = high word of (C:A << shift) + B. Fields per Ryujinx's InstLeaHiR (shift 28-32, C 39-46, NegA 37, X 38,
        // CC 47). Unsupported negate/carry/condition-code variants fail explicitly.
        int sh = (int)bits(w, 28, 5);
        if (bits(w, 37, 1) || bits(w, 38, 1) || bits(w, 47, 1)) fail(c, "lea.hi neg/x/cc");
        const char *cv = R((int)bits(w, 39, 8));
        if (sh == 0) sprintf(t, "(%s + %s)", cv, R((int)bits(w, 20, 8)));
        else sprintf(t, "(((%s >> %du) | (%s << %du)) + %s)", R(a), 32 - sh, cv, sh, R((int)bits(w, 20, 8)));
        set_reg(c, d, t);
    } else if (OPM(0x5bc0000000000000ull, 0xfff0000000000000ull) || OPM(0x4bc0000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x36c0000000000000ull, 0xfef0000000000000ull) || OPM(0x53c0000000000000ull, 0xfff0000000000000ull)) {  // PRMT
        const char *bv, *cv;
        char cb[256];
        if (OPM(0x53c0000000000000ull, 0xfff0000000000000ull)) { bv = R((int)bits(w, 39, 8)); cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb); }
        else { bv = B(false); cv = R((int)bits(w, 39, 8)); }
        if (bits(w, 48, 4)) fail(c, "prmt mode");
        sprintf(t, "prmt(%s, %s, %s)", R(a), cv, bv);
        set_reg(c, d, t);
    } else if (OPM(0x5cf8000000000000ull, 0xfff8000000000000ull) || OPM(0x38f8000000000000ull, 0xfef8000000000000ull) ||
               OPM(0x5bf8000000000000ull, 0xfff8000000000000ull) || OPM(0x36f8000000000000ull, 0xfef8000000000000ull)) {  // SHF
        // funnel shift of hi:lo (C:A). .W (bit 50) wraps the shift amount, otherwise it's clamped to the width;
        // bits 37-38: 2 .U64 / 3 .S64 shift the full 64 bits (width 64), else 32. The immediate is 6 bits (20-25:
        // bits 37-38 of the 19-bit B immediate are the width field). .CC / .X / .HI (47-49) aren't modelled.
        bool right = (w >> 56) == 0x5c || (w >> 57) == (0x38 >> 1);
        if (bits(w, 47, 3)) { fail(c, "shf cc/x/hi"); return; }
        char shb[16];
        const char *sh = ((w >> 60) == 3) ? (sprintf(shb, "%uu", (unsigned)bits(w, 20, 6)), shb) : R((int)bits(w, 20, 8));
        const char *hi = R((int)bits(w, 39, 8));
        int width = bits(w, 38, 1) ? 64 : 32;
        bool s64 = bits(w, 37, 2) == 3, wrap = bits(w, 50, 1);
        if (c->glsl && !maxwell_shader_int64) {
            sp(&c->body, wrap ? "    { uint s = %s & %du;\n" : "    { uint s = min(%s, %du);\n", sh, wrap ? width - 1 : width);
            if (d != RZ) { c->reg_used[d] = 1;
                sp(&c->body, "      r%d = shf32(%s, %s, s, %s, %s);\n", d, R(a), hi,
                   right ? "true" : "false", right && width == 64 && s64 ? "true" : "false"); }
        } else {
        sp(&c->body, wrap ? "    { uint s = %s & %du; ulong v = (ulong(%s) << 32) | ulong(%s);\n"
                          : "    { uint s = min(%s, %du); ulong v = (ulong(%s) << 32) | ulong(%s);\n",
           sh, wrap ? width - 1 : width, hi, R(a));
        if (right && width == 64)
            sprintf(t, "s >= 64u ? %s : uint(%s >> s)", s64 ? "uint(int(v >> 32) >> 31)" : "0u", s64 ? "long(v)" : "v");
        else if (right) sprintf(t, "uint(v >> s)");
        else if (width == 64) sprintf(t, "s >= 64u ? 0u : uint((v << s) >> 32)");
        else sprintf(t, "uint((v << s) >> 32)");
        if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = %s;\n", d, t); }
        }
        sp(&c->body, "    }\n");
    } else if (OPM(0x50d8000000000000ull, 0xfff8000000000000ull)) {  // VOTE.{ALL,ANY,EQ}; EOW dd9c79d6 field layout
        int mode = (int)bits(w, 48, 2), vp = (int)bits(w, 45, 3);
        if (mode > 2) { fail(c, "vote mode %d", mode); return; }
        if (!c->glsl || (c->stage != STAGE_COMPUTE && c->stage != STAGE_FRAGMENT) ||
            !(maxwell_vote_stages & (1u << c->stage)) ||
            (c->stage == STAGE_FRAGMENT && !maxwell_frag_subgroups)) {
            fail(c, "vote requires supported 32-lane ballot groups for shader stage %d", c->stage); return;
        }
        c->uses_vote = true;
        // A host subgroup may contain several guest warps. Choose this invocation's32-bit ballot word.
        sp(&c->body, "    { bool vp_ = %s; uvec4 vote_ = subgroupBallot(vp_), active_ = subgroupBallot(true);\n"
                     "      uint word_ = gl_SubgroupInvocationID >> 5u, mask_ = vote_[word_], active_mask_ = active_[word_];\n",
           P((int)bits(w, 39, 3), bits(w, 42, 1)));
        if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = mask_;\n", d); }
        static const char *const vote[] = {"mask_ == active_mask_", "mask_ != 0u", "(mask_ == 0u || mask_ == active_mask_)"};
        if (vp != PT) { c->pred_used[vp] = 1; sp(&c->body, "      p%d = %s;\n", vp, vote[mode]); }
        sp(&c->body, "    }\n");
    } else if (OPM(0xed00000000000000ull, 0xff00000000000000ull)) {  // ATOM.ADD U32/S32: return the old word
        unsigned size = (unsigned)bits(w, 49, 3), operation = (unsigned)bits(w, 52, 4);
        bool wide = bits(w, 48, 1);
        int source = (int)bits(w, 20, 8);
        int64_t off = a == RZ ? (int64_t)bits(w, 28, 20) : sbits(w, 28, 20);
        if (operation != 0 || size > 1) { fail(c, "unsupported ATOM operation/type %u/%u", operation, size); return; }
        if (!c->glsl || maxwell_gmem_readonly) { fail(c, "ATOM requires writable GLSL global storage"); return; }
        c->info->uses_gmem = c->info->uses_gmem_store = c->uses_gmem_store = c->uses_red = true;
        if (!maxwell_shader_int64) {
            if (!maxwell_gmem_mirror || maxwell_gmem_chunk_shift) {
                fail(c, "uint32 ATOM requires the storage-buffer mirror"); return;
            }
            glsl_gmem_address32(c, a, wide, off);
        } else {
            glsl_gmem_address64(c, a, wide, off);
        }
        sp(&c->body, "      ok = ok && (uint(ga) & 3u) == 0u && %s;\n", maxwell_gmem_size < 4 ? "false" : "true");
        if (maxwell_gmem_size >= 4)
            sp(&c->body, "      ok = ok && ga <= 0x%llx%s;\n", (unsigned long long)(maxwell_gmem_size - 4), maxwell_shader_int64 ? "ul" : "u");
        sp(&c->body, "      uint atom_value_ = %s, atom_old_ = 0u;\n", R(source));
        if (c->stage == STAGE_COMPUTE || maxwell_gmem_mirror) {
            c->info->uses_gmem_map = true;
            sp(&c->body, "      uint fw = ok ? gfind(uint(ga), 4u) : 0xFFFFFFFFu;\n"
                         "      if (fw != 0xFFFFFFFFu) { uint o_ = atomicCompSwap(gfast_.w[fw], 0u, 0u);\n"
                         "        for (;;) { uint p_ = atomicCompSwap(gfast_.w[fw], o_, o_ + atom_value_);"
                         " if (p_ == o_) { atom_old_ = o_; break; } o_ = p_; } }\n      else ");
        } else sp(&c->body, "      ");
        // Preserve the existing global-memory miss policy, never atomically
        // updating the zero mirror's dummy word. Valid mapped words use CAS.
        if (maxwell_gmem_mirror && maxwell_gmem_zero) sp(&c->body, "{ }\n");
        else sp(&c->body, "if (ok) { uint wi_ = uint(ga >> 2), o_ = GMCAS(wi_, 0u, 0u);\n"
                          "        for (;;) { uint p_ = GMCAS(wi_, o_, o_ + atom_value_);"
                          " if (p_ == o_) { atom_old_ = o_; break; } o_ = p_; } }\n");
        set_reg(c, d, "atom_old_");
        sp(&c->body, "    }\n");
    } else if (OPM(0xebf8000000000000ull, 0xfff8000000000000ull)) {  // RED, no returned value; EOW dd9c79d6
        int size = (int)bits(w, 20, 3), red = (int)bits(w, 23, 3);
        bool wide = bits(w, 48, 1);
        int64_t off = sbits(w, 28, 20);
        if (size > 1) { fail(c, "red size %d", size); return; }
        if (c->glsl && maxwell_gmem_readonly) {
            c->info->uses_gmem_store = true;
            fail(c, "read-only Android mirror does not support shader global atomics"); return;
        }
        if (!c->glsl) { fail(c, "red requires GLSL global atomic storage"); return; }
        c->info->uses_gmem = true;
        c->uses_gmem_store = c->info->uses_gmem_store = c->uses_red = true;
        if (!maxwell_shader_int64) {
            if (!maxwell_gmem_mirror || maxwell_gmem_chunk_shift) {
                fail(c, "uint32 shader global memory requires the storage-buffer mirror"); return;
            }
            glsl_gmem_address32(c, a, wide, off);
        } else {
            glsl_gmem_address64(c, a, wide, off);
        }
        // Entire aligned word must lie inside the guest heap before selecting either backing.
        sp(&c->body, "      ok = ok && (uint(ga) & 3u) == 0u && %s;\n",
           maxwell_gmem_size < 4 ? "false" : "true");
        if (maxwell_gmem_size >= 4)
            sp(&c->body, "      ok = ok && ga <= 0x%llx%s;\n", (unsigned long long)(maxwell_gmem_size - 4), maxwell_shader_int64 ? "ul" : "u");
        const char *value[] = {"(o_ + v_)", size ? "uint(min(int(o_), int(v_)))" : "min(o_, v_)",
                              size ? "uint(max(int(o_), int(v_)))" : "max(o_, v_)",
                              "(o_ >= v_ ? 0u : o_ + 1u)", "((o_ == 0u || o_ > v_) ? v_ : o_ - 1u)",
                              "(o_ & v_)", "(o_ | v_)", "(o_ ^ v_)"};
        if (c->stage == STAGE_COMPUTE || maxwell_gmem_mirror) {
            c->info->uses_gmem_map = true;
            sp(&c->body, "      uint fw = ok ? gfind(uint(ga), 4u) : 0xFFFFFFFFu;\n"
                         "      if (fw != 0xFFFFFFFFu) { uint v_ = %s, o_ = atomicCompSwap(gfast_.w[fw], 0u, 0u);\n"
                         "        for (;;) { uint p_ = atomicCompSwap(gfast_.w[fw], o_, %s); if (p_ == o_) break; o_ = p_; } }\n"
                         "      else ", R(d), value[red]);
        } else sp(&c->body, "      ");
        // Zero mirror misses have no real backing. Never atomically update its shared dummy word.
        if (maxwell_gmem_mirror && maxwell_gmem_zero) sp(&c->body, "{ }\n");
        else sp(&c->body, "if (ok) { uint wi_ = uint(ga >> 2), v_ = %s, o_ = GMCAS(wi_, 0u, 0u);\n"
                          "        for (;;) { uint p_ = GMCAS(wi_, o_, %s); if (p_ == o_) break; o_ = p_; } }\n", R(d), value[red]);
        sp(&c->body, "    }\n");
    } else if (OPM(0xeed0000000000000ull, 0xfff8000000000000ull) || OPM(0xeed8000000000000ull, 0xfff8000000000000ull)) {  // LDG / STG
        bool store = bits(w, 51, 1);
        bool wide = bits(w, 45, 1);
        int size = (int)bits(w, 48, 3);
        int64_t off = sbits(w, 20, 24);
        c->info->uses_gmem = true;
        if (c->glsl && maxwell_gmem_readonly) {
            if (store) { c->info->uses_gmem_store = true; fail(c, "read-only Android mirror does not support shader global stores"); return; }
            c->info->uses_gmem_readonly = true;
            glsl_gmem_address32(c, a, wide, off);
            if (size < 4) {
                static const char *const small[] = {"(v >> sh) & 0xFFu", "uint(sx8(v >> sh))", "(v >> sh) & 0xFFFFu", "uint(sx16(v >> sh))"};
                if (d != RZ) { c->reg_used[d] = 1;
                    sp(&c->body, "      uint v = ok ? gload(ga) : 0u; uint sh = (ga & %uu) * 8u; r%d = %s;\n",
                       size < 2 ? 3u : 2u, d, small[size]); }
            } else {
                int words = size == 5 ? 2 : size >= 6 ? 4 : 1;
                for (int k = 0; k < words; ++k) if (d != RZ && d + k < RZ) {
                    c->reg_used[d + k] = 1;
                    sp(&c->body, "      r%d = ok ? gload(ga + %uu) : 0u;\n", d + k, (unsigned)k * 4);
                }
            }
            sp(&c->body, "    }\n");
            goto insn_done;
        }
        if (c->glsl && !maxwell_shader_int64) {
            if (!maxwell_gmem_mirror || maxwell_gmem_chunk_shift) {
                fail(c, "uint32 shader global memory requires the storage-buffer mirror");
                return;
            }
            glsl_gmem_address32(c, a, wide, off);
        } else {
            glsl_gmem_address64(c, a, wide, off);
        }
        static const char *const ld_small[] = {"uint(gmem[ga])", "uint(int(char(gmem[ga])))",
                                               "uint(*(device ushort *)(gmem + ga))", "uint(int(*(device short *)(gmem + ga)))"};
        int nw = size == 5 ? 2 : size >= 6 ? 4 : 1;
        bool lazy_wide = c->glsl && maxwell_gmem_chunk_shift && maxwell_gmem_lazy && nw > 1;  // (one chunk check per access)
        if (c->glsl && (c->stage == STAGE_COMPUTE || maxwell_gmem_mirror)) {
            // Compute (Vulkan): the guest heap is host memory across PCIe on a discrete GPU, and a shader storing to
            // it in a loop crawls (the little chain chomps in Madam MeowMeow's house: ~4 ms a dispatch, ~0.07 ms without
            // the heap stores). The dispatch's storage buffers are copied to a device-local buffer (gfast_, address
            // table gmap: exec.c gfast_prepare) and back after it: loads and stores inside them use that copy.
            // Anything outside the table goes to the heap as before.
            c->info->uses_gmem_map = true;
            static const char *const gl_small[] = {
                "(%s >> ((uint(ga) & 3u) * 8u)) & 0xFFu", "uint(sx8(%s >> ((uint(ga) & 3u) * 8u)))",
                "(%s >> ((uint(ga) & 2u) * 8u)) & 0xFFFFu", "uint(sx16(%s >> ((uint(ga) & 2u) * 8u)))"};
            // Subword accesses read/modify their containing word. Wider
            // accesses must fit the entire range and backing buffer, not just
            // the first word, before using the fast-copy table.
            sp(&c->body, "      uint fw = ok ? gfind(uint(ga) & ~3u, %uu) : 0xFFFFFFFFu;\n", (unsigned)nw * 4);
            if (!store && maxwell_gmem_zero) {
                // (phone) a miss reads 0, as the heap binding's zero buffer would: one load, unconditional (index 0 on a
                // miss: in the buffer), then a select - no branch per word
                sp(&c->body, "      uint fwi = fw != 0xFFFFFFFFu ? fw : 0u;\n");
                if (size < 4) {
                    if (d != RZ) {
                        char fast[160];
                        snprintf(fast, sizeof fast, gl_small[size], "gv");
                        c->reg_used[d] = 1;
                        sp(&c->body, "      { uint gv = gfast_.w[fwi]; r%d = fw != 0xFFFFFFFFu ? %s : 0u; }\n", d, fast);
                    }
                } else {
                    for (int k = 0; k < nw; k++)
                        if (d != RZ && d + k < RZ) {
                            c->reg_used[d + k] = 1;
                            sp(&c->body, "      { uint gv = gfast_.w[fwi + %du]; r%d = fw != 0xFFFFFFFFu ? gv : 0u; }\n", k, d + k);
                        }
                }
            } else if (!store) {
                if (size < 4) {
                    if (d != RZ) {
                        char fast[160], slow[160];
                        snprintf(fast, sizeof fast, gl_small[size], "gfast_.w[fw]");
                        snprintf(slow, sizeof slow, gl_small[size], "GMR(uint(ga >> 2))");
                        c->reg_used[d] = 1;
                        sp(&c->body, "      r%d = fw != 0xFFFFFFFFu ? %s : ok ? %s : 0u;\n", d, fast, slow);
                    }
                } else if (lazy_wide) {
                    sp(&c->body, "      { uvec%d t = fw != 0xFFFFFFFFu ? uvec%d(", nw, nw);
                    for (int k = 0; k < nw; k++) sp(&c->body, "%sgfast_.w[fw + %du]", k ? ", " : "", k);
                    sp(&c->body, ") : ok ? GMR%d(uint(ga >> 2)) : uvec%d(0u);\n", nw, nw);
                    for (int k = 0; k < nw; k++)
                        if (d != RZ && d + k < RZ) { c->reg_used[d + k] = 1; sp(&c->body, "        r%d = t[%d];\n", d + k, k); }
                    sp(&c->body, "      }\n");
                } else {
                    for (int k = 0; k < nw; k++)
                        if (d != RZ && d + k < RZ) {
                            c->reg_used[d + k] = 1;
                            sp(&c->body, "      r%d = fw != 0xFFFFFFFFu ? gfast_.w[fw + %du] : ok ? GMR(uint(ga >> 2) + %du) : 0u;\n",
                               d + k, k, k);
                        }
                }
            } else {
                c->uses_gmem_store = true;
                c->info->uses_gmem_store = true;
                if (size < 4) {
                    uint32_t m = size < 2 ? 0xFFu : 0xFFFFu;
                    const char *bm = size < 2 ? "3u" : "2u";
                    sp(&c->body, "      if (ok) { uint sh = (uint(ga) & %s) * 8u; uint v = (%s & 0x%xu) << sh;\n", bm, R(d), m);
                    sp(&c->body, "        if (fw != 0xFFFFFFFFu) { atomicAnd(gfast_.w[fw], ~(0x%xu << sh)); atomicOr(gfast_.w[fw], v); }\n", m);
                    sp(&c->body, "        else { GMAND(uint(ga >> 2), ~(0x%xu << sh)); GMOR(uint(ga >> 2), v); } }\n", m);
                } else if (lazy_wide) {
                    for (int k = 0; k < nw; k++) sp(&c->body, "      if (fw != 0xFFFFFFFFu) gfast_.w[fw + %du] = %s;\n", k, R(d == RZ ? RZ : d + k));
                    sp(&c->body, "      if (fw == 0xFFFFFFFFu && ok) GMW%d(uint(ga >> 2), uvec%d(", nw, nw);
                    for (int k = 0; k < nw; k++) sp(&c->body, "%s%s", k ? ", " : "", R(d == RZ ? RZ : d + k));
                    sp(&c->body, "));\n");
                } else {
                    for (int k = 0; k < nw; k++)
                        sp(&c->body, "      if (fw != 0xFFFFFFFFu) gfast_.w[fw + %du] = %s; else if (ok) GMW(uint(ga >> 2) + %du, %s);\n", k,
                           R(d == RZ ? RZ : d + k), k, R(d == RZ ? RZ : d + k));
                }
            }
        } else if (c->glsl) {  // 32-bit words (GMR / GMW / GMAND / GMOR: the prelude); sub-word accesses within the containing word
            static const char *const gl_small[] = {
                "(GMR(uint(ga >> 2)) >> ((uint(ga) & 3u) * 8u)) & 0xFFu",
                "uint(sx8(GMR(uint(ga >> 2)) >> ((uint(ga) & 3u) * 8u)))",
                "(GMR(uint(ga >> 2)) >> ((uint(ga) & 2u) * 8u)) & 0xFFFFu",
                "uint(sx16(GMR(uint(ga >> 2)) >> ((uint(ga) & 2u) * 8u)))"};
            if (!store) {
                if (size < 4) {
                    if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      if (ok) r%d = %s; else r%d = 0u;\n", d, gl_small[size], d); }
                } else if (lazy_wide) {  // (one bounds check and address for the whole aligned access)
                    sp(&c->body, "      { uvec%d t = ok ? GMR%d(uint(ga >> 2)) : uvec%d(0u);\n", nw, nw, nw);
                    for (int k = 0; k < nw; k++)
                        if (d != RZ && d + k < RZ) { c->reg_used[d + k] = 1; sp(&c->body, "        r%d = t[%d];\n", d + k, k); }
                    sp(&c->body, "      }\n");
                } else {
                    for (int k = 0; k < nw; k++)
                        if (d != RZ && d + k < RZ) {
                            c->reg_used[d + k] = 1;
                            sp(&c->body, "      if (ok) r%d = GMR(uint(ga >> 2) + %du); else r%d = 0u;\n", d + k, k, d + k);
                        }
                }
            } else {
                c->uses_gmem_store = true;
                c->info->uses_gmem_store = true;  // (the draw marks the pages it stores to: vertex cache copies go stale)
                if (size < 4)
                    sp(&c->body, "      if (ok) { uint wi = uint(ga >> 2); uint sh = (uint(ga) & %s) * 8u; GMAND(wi, ~(0x%xu << sh)); "
                       "GMOR(wi, (%s & 0x%xu) << sh); }\n", size < 2 ? "3u" : "2u", size < 2 ? 0xFFu : 0xFFFFu, R(d),
                       size < 2 ? 0xFFu : 0xFFFFu);
                else if (lazy_wide) {
                    sp(&c->body, "      if (ok) GMW%d(uint(ga >> 2), uvec%d(", nw, nw);
                    for (int k = 0; k < nw; k++) sp(&c->body, "%s%s", k ? ", " : "", R(d == RZ ? RZ : d + k));
                    sp(&c->body, "));\n");
                } else
                    for (int k = 0; k < nw; k++)
                        sp(&c->body, "      if (ok) GMW(uint(ga >> 2) + %du, %s);\n", k, R(d == RZ ? RZ : d + k));
            }
        } else if (!store) {
            if (size < 4) {
                if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = ok ? %s : 0u;\n", d, ld_small[size]); }
            } else {
                for (int k = 0; k < nw; k++)
                    if (d != RZ && d + k < RZ) { c->reg_used[d + k] = 1; sp(&c->body, "      r%d = ok ? *(device uint *)(gmem + ga + %du) : 0u;\n", d + k, k * 4); }
            }
        } else {
            if (size == 0 || size == 1) sp(&c->body, "      if (ok) gmem[ga] = uchar(%s);\n", R(d));
            else if (size == 2 || size == 3) sp(&c->body, "      if (ok) *(device ushort *)(gmem + ga) = ushort(%s);\n", R(d));
            else
                for (int k = 0; k < nw; k++)
                    sp(&c->body, "      if (ok) *(device uint *)(gmem + ga + %du) = %s;\n", k * 4, R(d == RZ ? RZ : d + k));
        }
        sp(&c->body, "    }\n");
    } else if (emit_half(c, w)) {
    } else if (OPM(0x50f8000000000000ull, 0xfff8000000000000ull)) {  // FSWZADD
        // per quad lane (laneid & 3), 2 bits of the swizzle pick: a+b, b-a, a-b, b
        uint32_t swz = (uint32_t)bits(w, 28, 8);
        if (c->stage != STAGE_FRAGMENT) {
            sprintf(t, "as_type<uint>(%s + %s)", F(R(a)), F(R((int)bits(w, 20, 8))));
        } else {
            c->uses_lane = true;
            sprintf(t, "as_type<uint>(fswzadd(%s, %s, 0x%02xu, lane))", F(R(a)), F(R((int)bits(w, 20, 8))), swz);
        }
        set_reg(c, d, t);
    } else if (OPM(0x5b40000000000000ull, 0xfff0000000000000ull) || OPM(0x4b40000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x3640000000000000ull, 0xfef0000000000000ull) || OPM(0x5340000000000000ull, 0xfff0000000000000ull)) {  // ICMP
        char cmp[512], cb[256], bb[256];
        const char *bv, *cv;
        if (OPM(0x5340000000000000ull, 0xfff0000000000000ull)) {
            bv = R((int)bits(w, 39, 8));
            cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
        } else {
            bv = operand_b(c, w, false, bb);
            cv = R((int)bits(w, 39, 8));
        }
        icmp_expr(cmp, (int)bits(w, 49, 3), cv, "0u", bits(w, 48, 1));
        sprintf(t, "(%s ? %s : %s)", cmp, R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0xef40000000000000ull, 0xfff8000000000000ull) || OPM(0xef50000000000000ull, 0xfff8000000000000ull)) {  // LDL / STL
        // thread-private local memory, modelled as a word array (byte/short accesses within a word)
        bool store = bits(w, 52, 1);
        int size = (int)bits(w, 48, 3);
        int32_t off = (int32_t)sbits(w, 20, 24);
        c->uses_local = true;
        char addr[128];
        if (a == RZ) sprintf(addr, "%du", off);
        else sprintf(addr, "(%s + %du)", R(a), (uint32_t)off);
        int nw = size == 5 ? 2 : size >= 6 ? 4 : 1;
        sp(&c->body, "    { uint la = %s;\n", addr);
        if (c->local_sparse) {
            if (!store) {
                char selected[1024]; local_sparse_read(c, selected);
                if (size < 4) {
                    static const char *const ext[] = {"(v >> sh) & 0xFFu", "uint(int(char((v >> sh) & 0xFFu)))", "(v >> sh) & 0xFFFFu",
                                                      "uint(int(short((v >> sh) & 0xFFFFu)))"};
                    if (d != RZ) {
                        c->reg_used[d] = 1;
                        sp(&c->body, "      uint li = (la >> 2) & %du; uint v = %s; uint sh = (la & 3u) * 8u; r%d = %s;\n",
                           LMEM_WORDS - 1, selected, d, ext[size]);
                    }
                } else for (int k = 0; k < nw; k++) if (d != RZ && d + k < RZ) {
                    c->reg_used[d + k] = 1;
                    sp(&c->body, "      { uint li = ((la >> 2) + %du) & %du; r%d = %s; }\n", k, LMEM_WORDS - 1, d + k, selected);
                }
            } else {
                unsigned slot = ((uint32_t)off >> 2) & (LMEM_WORDS - 1);
                if (size < 4) {
                    uint32_t mask = size < 2 ? 0xffu : 0xffffu;
                    sp(&c->body, "      uint sh = (la & 3u) * 8u; lm%u = (lm%u & ~(0x%xu << sh)) | ((%s & 0x%xu) << sh);\n",
                       slot, slot, mask, R(d), mask);
                } else for (int k = 0; k < nw; k++)
                    sp(&c->body, "      lm%u = %s;\n", (slot + (unsigned)k) & (LMEM_WORDS - 1), R(d == RZ ? RZ : d + k));
            }
        } else if (!store) {
            if (size < 4) {
                static const char *const ext[] = {"(v >> sh) & 0xFFu", "uint(int(char((v >> sh) & 0xFFu)))", "(v >> sh) & 0xFFFFu",
                                                  "uint(int(short((v >> sh) & 0xFFFFu)))"};
                if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      uint v = lmem[(la >> 2) & %du]; uint sh = (la & 3u) * 8u; r%d = %s;\n", LMEM_WORDS - 1, d, ext[size]); }
            } else {
                for (int k = 0; k < nw; k++)
                    if (d != RZ && d + k < RZ) { c->reg_used[d + k] = 1; sp(&c->body, "      r%d = lmem[((la >> 2) + %du) & %du];\n", d + k, k, LMEM_WORDS - 1); }
            }
        } else {
            if (size < 4) {
                uint32_t m = size < 2 ? 0xFFu : 0xFFFFu;
                sp(&c->body, "      uint sh = (la & 3u) * 8u; uint i = (la >> 2) & %du; lmem[i] = (lmem[i] & ~(0x%xu << sh)) | ((%s & 0x%xu) << sh);\n",
                   LMEM_WORDS - 1, m, R(d), m);
            } else {
                for (int k = 0; k < nw; k++)
                    sp(&c->body, "      lmem[((la >> 2) + %du) & %du] = %s;\n", k, LMEM_WORDS - 1, R(d == RZ ? RZ : d + k));
            }
        }
        sp(&c->body, "    }\n");
    } else if (OPM(0xef10000000000000ull, 0xfff8000000000000ull)) {  // SHFL
        int mode = (int)bits(w, 30, 2);
        char bb[32];
        const char *bv = bits(w, 28, 1) ? (sprintf(bb, "%uu", (unsigned)bits(w, 20, 5)), bb) : R((int)bits(w, 20, 8));
        int pd = (int)bits(w, 48, 3);
        if (c->stage == STAGE_VERTEX) {
            set_reg(c, d, R(a));  // no SIMD-group functions in vertex functions: lane-local value
        } else if (c->glsl && c->stage == STAGE_FRAGMENT && !maxwell_frag_subgroups) {
            // GPUs with subgroup operations in compute shaders only (Adreno; using them in a fragment shader hung the
            // GPU): the quad xor, the shaders' way of reading a neighbouring pixel, through fine derivatives
            // (quad_xor); other shuffles keep the lane's own value
            bool quad = mode == 3 && bits(w, 28, 1) && bits(w, 20, 5) < 4;
            if (quad) sprintf(t, "quad_xor(%s, %uu)", R(a), (unsigned)bits(w, 20, 5));
            else sprintf(t, "%s", R(a));
            set_reg(c, d, t);
        } else if (c->glsl) {  // a quad xor (lane mask < 4) is the subgroup xor within the quad
            static const char *const fn[] = {"subgroupShuffle", "subgroupShuffleUp", "subgroupShuffleDown", "subgroupShuffleXor"};
            sprintf(t, "%s(%s, uint(%s))", fn[mode], R(a), bv);
            set_reg(c, d, t);
        } else if (mode == 3) {
            bool quad = bits(w, 28, 1) && bits(w, 20, 5) < 4;
            sprintf(t, quad ? "quad_shuffle_xor(%s, ushort(%s))" : "simd_shuffle_xor(%s, ushort(%s))", R(a), bv);
            set_reg(c, d, t);
        } else {
            static const char *const fn[] = {"simd_shuffle", "simd_shuffle_up", "simd_shuffle_down"};
            sprintf(t, "%s(%s, ushort(%s))", fn[mode], R(a), bv);
            set_reg(c, d, t);
        }
        if (pd != PT) { c->pred_used[pd] = 1; sp(&c->body, "    p%d = true;\n", pd); }
    } else if (OPM(0x38f0000000000000ull, 0xfef8000000000000ull) || OPM(0x5cf0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x4cf0000000000000ull, 0xfff8000000000000ull)) {  // R2P
        if (bits(w, 40, 1)) { fail(c, "r2p cc"); }
        int shift = (int)bits(w, 41, 2) * 8;
        const char *mask = B(false);
        for (int i = 0; i < 7; i++) {
            c->pred_used[i] = 1;
            sp(&c->body, c->glsl ? "    if (((%s >> %du) & 1u) != 0u) p%d = ((%s >> %du) & 1u) != 0u;\n"
                                 : "    if ((%s >> %du) & 1u) p%d = ((%s >> %du) & 1u) != 0u;\n", mask, i, i, R(a), shift + i);
        }
    } else if (OPM(0x38e8000000000000ull, 0xfef8000000000000ull) || OPM(0x5ce8000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x4ce8000000000000ull, 0xfff8000000000000ull)) {  // P2R
        int shift = (int)bits(w, 41, 2) * 8;
        const char *mask = B(false);
        if (bits(w, 40, 1)) {  // .CC: the flags Z, S, C, O as bits 0-3 (overflow isn't tracked: 0)
            c->uses_cc = true;
            sp(&c->body, "    { uint pr = (ccz ? 1u : 0u) | (ccs ? 2u : 0u) | ((cc & 1u) << 2);\n");
            sp(&c->body, "      uint m = (%s & 0xFu) << %du;\n", mask, shift);
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = (%s & ~m) | ((pr << %du) & m);\n", d, R(a), shift); }
            sp(&c->body, "    }\n");
            goto p2r_done;
        }
        sp(&c->body, "    { uint pr = 0u;\n");
        for (int i = 0; i < 7; i++) { c->pred_used[i] = 1; sp(&c->body, "      pr |= (p%d ? 1u : 0u) << %du;\n", i, i); }
        sp(&c->body, "      uint m = (%s & 0x7Fu) << %du;\n", mask, shift);
        if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = (%s & ~m) | ((pr << %du) & m);\n", d, R(a), shift); }
        sp(&c->body, "    }\n");
    p2r_done:;
    } else if (OPM(0xe250000000000000ull, 0xfff0000000000020ull)) {  // BRX reg + imm (relative)
        // computed targets can name a scheduling word; execution continues at the next slot (as for BRA)
        sp(&c->body, "    { uint t = %uu + %s + 0x%08xu; if (((t >> 3) & 3u) == 0u) t += 8u; pc = t; continue; }\n", pc + 8, R(a),
           (uint32_t)sbits(w, 20, 24));
    } else if (OPM(0x50a0000000000000ull, 0xfff8000000000000ull)) {  // CSETP (zero/sign flags only)
        int test = (int)bits(w, 8, 5);
        const char *cond;
        switch (test) {
        case 0: cond = "false"; break;
        case 15: cond = "true"; break;
        case 2: case 10: cond = "ccz"; break;
        case 5: case 13: cond = "!ccz"; break;
        case 1: case 9: cond = "ccs"; break;
        case 6: case 14: cond = "!ccs"; break;
        case 4: case 12: cond = "(!ccz && !ccs)"; break;
        case 3: case 11: cond = "(ccz || ccs)"; break;
        default: cond = "false"; fail(c, "csetp test %d", test);
        }
        c->uses_cc = true;
        int p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3), bo = (int)bits(w, 45, 2);
        const char *pc_ = P((int)bits(w, 39, 3), bits(w, 42, 1));
        sp(&c->body, "    { bool cmp = %s; bool pp = %s;\n", cond, pc_);
        if (p != PT) { c->pred_used[p] = 1; sp(&c->body, "      p%d = cmp %s pp;\n", p, bool_op(bo)); }
        if (q != PT) { c->pred_used[q] = 1; sp(&c->body, "      p%d = !cmp %s pp;\n", q, bool_op(bo)); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5098000000000000ull, 0xfff8000000000000ull)) {  // CSET
        fail(c, "cset");
    } else if (OPM(0xf0a8000000000000ull, 0xfff8000000000000ull)) {  // BAR
        if (!c->glsl) sp(&c->body, "    threadgroup_barrier(mem_flags::mem_threadgroup);\n");
        else if (c->stage == STAGE_COMPUTE) sp(&c->body, "    barrier();\n");
        else sp(&c->body, "    /* bar */\n");  // GLSL barrier() is compute / tessellation only
    } else {
        fail(c, "unknown instruction %016llx at %#x", (unsigned long long)w, pc);
    }
insn_done:
    (void)op13;
    if (guarded) sp(&c->body, "  }\n");
    if (c->glsl) writes_after_insn(c, text0, guarded);
    if (c->xmad_new) c->xmad_lo_pc = pc + 1, c->xmad_new = false;
    c->in_guard = false;
}

// ------------------------------------------------------------------ program assembly

// ------------------------------------------------------------------ structured control flow (GLSL)
// Shaders with branches ran as a `switch (pc)` state machine with the SSY / PBK / PCNT stacks in arrays. Phone GPUs
// take that badly: arrays indexed at run time live in scratch memory, every taken branch goes back through the
// dispatch loop, and the compiler can't schedule across it - a 320x180 blur of the title screen's beach took 33 ms of
// a 39 ms frame on an Adreno 650. Instead the program is rebuilt as structured code: the stacks are resolved where
// they are fixed at each instruction (as in shaders a compiler made), the blocks run behind flags (`bool f<n>`: a
// branch sets its target's flag) in an order where every forward jump goes down, and each natural loop becomes a
// `for` running while its header's flag is set. The same instructions run in the same order. Irreducible flow, stacks
// that differ between paths into one instruction, indexed branches (BRX): the state machine, as before.
// LARECOMP_SHADER_PC_MACHINE=1 keeps the state machine for every shader (comparisons).
enum { FLOW_NONE, FLOW_BRA, FLOW_EXIT, FLOW_SYNC, FLOW_BRK, FLOW_CONT, FLOW_SSY, FLOW_PBK, FLOW_PCNT, FLOW_BRX };
static int flow_kind(uint64_t w) {
    if ((w & 0xfff0000000000000ull) == 0xe300000000000000ull) return FLOW_EXIT;
    if ((w & 0xfff0000000000020ull) == 0xe240000000000000ull) return FLOW_BRA;
    if ((w & 0xfff0000000000020ull) == 0xe290000000000000ull) return FLOW_SSY;
    if ((w & 0xfff0000000000020ull) == 0xe2a0000000000000ull) return FLOW_PBK;
    if ((w & 0xfff0000000000020ull) == 0xe2b0000000000000ull) return FLOW_PCNT;
    if ((w & 0xfff8000000000000ull) == 0xf0f8000000000000ull) return FLOW_SYNC;
    if ((w & 0xfff0000000000000ull) == 0xe340000000000000ull) return FLOW_BRK;
    if ((w & 0xfff0000000000000ull) == 0xe350000000000000ull) return FLOW_CONT;
    if ((w & 0xfff0000000000000ull) == 0xe250000000000000ull) return FLOW_BRX;
    return FLOW_NONE;
}
static bool flow_leaves(int k) { return k == FLOW_BRA || k == FLOW_EXIT || k == FLOW_SYNC || k == FLOW_BRK || k == FLOW_CONT; }

// (from mamcjr/LASRecompAndroid, MIT; the table read as upstream's #237 brx_table_labels does) A BRX whose register
// was just loaded by an LDC c[0x1][Rx + table] - the compiler's constants, stored after the shader code (blob +
// max_size) - jumps through that table: each target is pc + 8 + imm + one of its words. The LDC must be a 32-bit load
// from cb1 within the 32 instructions before, with no branch and nothing else writing the register between; the
// entries are read from its offset until one doesn't land on an instruction of this shader after the BRX (the end of
// the table), at most 256. The targets become labels and c->brx (resolve_stacks, st_switch). The earlier version took
// every word of the first 4 KB of cb1 that looked like a target: words past the table made false cases, and it read
// past the constants. False: another shape - the caller labels a window, or every instruction.
static bool brx_table(Ctx *c, const uint64_t *code, size_t i, size_t end, const uint32_t *cb1, size_t cb1_bytes, uint8_t *label) {
    if (!cb1 || c->nbrx == 4) return false;
    uint64_t w = code[i];
    int ra = (int)bits(w, 8, 8);
    if (ra == RZ) return false;
    int64_t table = -1;
    for (size_t k = i, seen = 0; k-- > 0 && seen < 32;) {
        if (k % 4 == 0) continue;  // (scheduling words)
        seen++;
        uint64_t wk = code[k];
        uint32_t top = (uint32_t)(wk >> 52);
        if ((top >= 0xe20 && top <= 0xe35) || (wk & 0xfff8000000000000ull) == 0xf0f8000000000000ull) return false;
        if ((wk & 0xfff8000000000000ull) == 0xef90000000000000ull && (int)bits(wk, 0, 8) == ra) {  // LDC ra, c[idx][Rx + off]
            if (bits(wk, 36, 5) != 1 || bits(wk, 48, 3) != 4) return false;  // (cb1, 32-bit)
            table = (int64_t)bits(wk, 20, 16);
            break;
        }
        if ((int)bits(wk, 0, 8) == ra) return false;  // (something else may write ra: unknown)
    }
    if (table < 0 || (table & 3)) return false;
    int64_t imm = sbits(w, 20, 24);
    int n = 0;
    uint32_t *t = c->brx[c->nbrx].t;
    for (int j = 0; j < 256; j++) {
        size_t offset = (size_t)table + (size_t)j * 4;
        // The declared byte span can end exactly with the last jump-table
        // entry; a sentinel is not required inside c1. Accept only complete
        // entries already checked below. Never inspect code-block padding to
        // find a sentinel, or accept an empty/out-of-range/partial table.
        if (offset == cb1_bytes && n) break;
        if (offset > cb1_bytes || cb1_bytes - offset < sizeof(uint32_t)) return false;
        uint32_t entry;
        memcpy(&entry, (const uint8_t *)cb1 + offset, sizeof entry);
        int64_t tt = (int64_t)i * 8 + 8 + (int64_t)(int32_t)entry + imm;
        if (tt <= (int64_t)i * 8 || (tt & 7) || (size_t)(tt / 8) >= end) break;  // (the end of the table)
        tt = norm_target(tt);  // (as the BRX itself does at run time)
        if ((size_t)(tt / 8) >= end) break;
        bool dup = false;
        for (int q = 0; q < n && !dup; q++) dup = t[q] == (uint32_t)tt;
        if (dup) continue;
        int q = n++;
        while (q > 0 && t[q - 1] > (uint32_t)tt) { t[q] = t[q - 1]; q--; }  // (ascending)
        t[q] = (uint32_t)tt;
    }
    if (!n) return false;
    c->brx[c->nbrx].at = (uint32_t)i, c->brx[c->nbrx].n = n;
    c->nbrx++;
    for (int q = 0; q < n; q++) label[t[q] / 8] = 1;
    return true;
}
static int brx_of(const Ctx *c, size_t i) {
    for (int b = 0; b < c->nbrx; b++) if (c->brx[b].at == i) return b;
    return -1;
}

// (from mamcjr/LASRecompAndroid, MIT) Follows every path from the entry with the SSY / PBK / PCNT stacks' contents, as
// the state machine runs them per invocation: a SYNC / BRK goes to the target it pops, a CONT to its top (kept), and
// their guarded forms may also fall through with the stack as it was. flow_to[i]: the target of the SYNC / BRK /
// CONT at i. False (keep the stacks) when an instruction is reached with two different stacks, a stack under- or
// overflows, or the program has a jump table without a table or a call / return.
// LARECOMP_LOG_FLOW=1: why structure() gave a shader up (and its blocks), to stderr
static int flow_log(void) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_LOG_FLOW") ? atoi(getenv("LARECOMP_LOG_FLOW")) : 0;
    return on;
}
typedef struct { uint8_t n[3], seen; uint32_t v[3][16]; } FlowSt;
static bool resolve_stacks(const Ctx *c, const uint64_t *code, size_t end, int32_t *flow_to) {
    FlowSt *st = calloc(end, sizeof *st);
    uint32_t *work = malloc(end * 2 * sizeof *work + 16);
    size_t nwork = 0;
    bool ok = true;
    for (size_t i = 0; i < end; i++) flow_to[i] = -1;
#define FLOW_NEXT(i) ((i) + 1 + (((i) + 1) % 4 == 0))
#define FLOW_GO(j, S) do { size_t j_ = (j); if (j_ >= end) break; \
        if (!st[j_].seen) { st[j_] = (S); st[j_].seen = 1; work[nwork++] = (uint32_t)j_; } \
        else if (memcmp(st[j_].n, (S).n, 3) || memcmp(st[j_].v, (S).v, sizeof (S).v)) { \
            if (flow_log()) fprintf(stderr, "[flow] stacks: pc %zx reached from pc %zx with %d/%d/%d top %x/%x/%x, before %d/%d/%d top %x/%x/%x\n", \
                                    j_ * 8, i * 8, (S).n[0], (S).n[1], (S).n[2], (S).n[0] ? (S).v[0][(S).n[0] - 1] : 0, \
                                    (S).n[1] ? (S).v[1][(S).n[1] - 1] : 0, (S).n[2] ? (S).v[2][(S).n[2] - 1] : 0, \
                                    st[j_].n[0], st[j_].n[1], st[j_].n[2], st[j_].n[0] ? st[j_].v[0][st[j_].n[0] - 1] : 0, \
                                    st[j_].n[1] ? st[j_].v[1][st[j_].n[1] - 1] : 0, st[j_].n[2] ? st[j_].v[2][st[j_].n[2] - 1] : 0); \
            ok = false; } } while (0)
    if (end > 1) { size_t i = 0; FlowSt e = {{0}}; FLOW_GO(1, e); (void)i; }
    while (nwork && ok) {
        size_t i = work[--nwork];
        FlowSt S = st[i];
        uint64_t w = code[i];
        uint32_t pc = (uint32_t)(i * 8);
        uint32_t top = (uint32_t)(w >> 52);
        int pr = (int)bits(w, 16, 3);
        bool prn = bits(w, 19, 1);
        bool push = (w & 0xfff0000000000020ull) == 0xe290000000000000ull || (w & 0xfff0000000000020ull) == 0xe2a0000000000000ull ||
                    (w & 0xfff0000000000020ull) == 0xe2b0000000000000ull;
        if (push || (w & 0xfff0000000000000ull) == 0xe270000000000000ull) pr = PT, prn = false;  // (as emit_insn)
        if (pr == PT && prn) { FLOW_GO(FLOW_NEXT(i), S); continue; }  // (never executes)
        bool cond = !(pr == PT && !prn);
        int64_t t = norm_target((int64_t)pc + 8 + sbits(w, 20, 24));
        if ((w & 0xfff0000000000020ull) == 0xe240000000000000ull) {  // BRA
            if (t >= 0) FLOW_GO((size_t)t / 8, S);
            if (cond) FLOW_GO(FLOW_NEXT(i), S);
        } else if (push) {  // SSY / PBK / PCNT
            int k = top == 0xe29 ? 0 : top == 0xe2a ? 1 : 2;
            if (S.n[k] == 16 || t < 0) { if (flow_log()) fprintf(stderr, "[flow] stacks: pc %x push overflow\n", pc); ok = false; break; }
            S.v[k][S.n[k]++] = (uint32_t)t;
            FLOW_GO(FLOW_NEXT(i), S);
        } else if ((w & 0xfff8000000000000ull) == 0xf0f8000000000000ull || top == 0xe34 || top == 0xe35) {  // SYNC BRK CONT
            int k = top == 0xe34 ? 1 : top == 0xe35 ? 2 : 0;
            if (!S.n[k]) { if (flow_log()) fprintf(stderr, "[flow] stacks: pc %x pops an empty stack %d\n", pc, k); ok = false; break; }
            uint32_t to = S.v[k][S.n[k] - 1];
            flow_to[i] = (int32_t)to;
            if (cond) FLOW_GO(FLOW_NEXT(i), S);
            if (k != 2) S.v[k][--S.n[k]] = 0;  // (cleared: states are compared whole)
            FLOW_GO(to / 8, S);
        } else if (top == 0xe30) {  // EXIT
            if (cond) FLOW_GO(FLOW_NEXT(i), S);
        } else if (top == 0xe25 && brx_of(c, i) >= 0) {  // BRX through a table: each target, with this stack
            int b = brx_of(c, i);
            for (int q = 0; q < c->brx[b].n; q++) FLOW_GO(c->brx[b].t[q] / 8, S);
            if (cond) FLOW_GO(FLOW_NEXT(i), S);
        } else if (top >= 0xe20 && top <= 0xe3f && top != 0xe33) {  // BRX, CAL, RET, PRET, ... (KIL: 0xe33)
            if (flow_log()) fprintf(stderr, "[flow] stacks: pc %x: %016llx (brx without a table / cal / ret)\n", pc, (unsigned long long)w);
            ok = false;
        } else {
            FLOW_GO(FLOW_NEXT(i), S);
        }
    }
#undef FLOW_GO
#undef FLOW_NEXT
    free(work);
    free(st);
    return ok;
}

#define FLOW_DEPTH 16
#define FLOW_SUCC 128  // successors of a block: 2, or a BRX's possible targets
typedef struct { uint8_t n[3]; uint32_t v[3][FLOW_DEPTH]; } FlowStacks;  // ssy, pbk, pcnt
static bool stacks_eq(const FlowStacks *a, const FlowStacks *b) {  // (the live entries: popped ones stay behind in v)
    for (int s = 0; s < 3; s++)
        if (a->n[s] != b->n[s] || memcmp(a->v[s], b->v[s], sizeof a->v[s][0] * a->n[s])) return false;
    return true;
}
typedef struct {
    size_t first, last;   // instruction words of the block (last: its terminator, if it has one)
    int succ[FLOW_SUCC], nsucc;  // blocks after it (-1: the end); guarded: [0] taken, [1] not; BRX: its targets
    int pr;               // the terminator's guard
    int brx;              // BRX: its register (the targets: succ[0..nsucc), plus the fallthrough last when guarded)
    bool prn, seen;
    FlowStacks in;        // the stacks on entry
    int loop;             // innermost loop holding it, -1 none
} FlowBlock;
typedef struct { int header, parent, size; uint8_t *in; } FlowLoop;
typedef struct {
    Ctx *c;
    const uint64_t *code;
    const size_t *off0, *off1;  // each instruction's text in c->body
    FlowBlock *b;
    int nb, nloops;
    FlowLoop *loops;
    Str o;
    bool fail;
} Flow;

// The node block i belongs to in region r (a loop, -1: the program): nb + the loop just inside r holding it, or i;
// -1 when i isn't in r
static int flow_node(Flow *f, int i, int r) {
    int l = f->b[i].loop, n = -1;
    while (l >= 0 && l != r) n = l, l = f->loops[l].parent;
    if (l != r) return -1;
    return n >= 0 ? f->nb + n : i;
}

static void flow_jump(Flow *f, int to, int ind) {
    if (to >= 0) sp(&f->o, "%*sf%d = true;\n", ind, "", to);
}

static void flow_emit_region(Flow *f, int r, int ind) {
    int nn = f->nb + f->nloops;
    int *indeg = calloc(nn, sizeof(int)), *ord = malloc(sizeof(int) * nn), no = 0;
    uint8_t *present = calloc(nn, 1), *edge = calloc((size_t)nn * nn, 1);
    for (int i = 0; i < f->nb; i++) {
        if (!f->b[i].seen) continue;
        int u = flow_node(f, i, r);
        if (u < 0) continue;
        present[u] = 1;
        for (int k = 0; k < f->b[i].nsucc; k++) {
            int s = f->b[i].succ[k];
            if (s < 0 || (r >= 0 && s == f->loops[r].header)) continue;  // (the end; the region's back edges)
            int v = flow_node(f, s, r);
            if (v < 0 || v == u || edge[(size_t)u * nn + v]) continue;
            edge[(size_t)u * nn + v] = 1;
            indeg[v]++;
        }
    }
    int *q = malloc(sizeof(int) * nn), qh = 0, qt = 0;
    for (int u = 0; u < nn; u++) if (present[u] && !indeg[u]) q[qt++] = u;
    while (qh < qt) {
        int u = q[qh++];
        ord[no++] = u;
        for (int v = 0; v < nn; v++)
            if (edge[(size_t)u * nn + v] && !--indeg[v]) q[qt++] = v;
    }
    int np = 0;
    for (int u = 0; u < nn; u++) np += present[u];
    if (no != np || (r >= 0 && (no == 0 || ord[0] != f->loops[r].header))) f->fail = true;
    for (int k = 0; k < no && !f->fail; k++) {
        int u = ord[k];
        if (u >= f->nb) {
            int l = u - f->nb, h = f->loops[l].header;
            // (the guard: graphics loops end far sooner - a loop over data a skipped pass never wrote must not run
            // into the GPU's hang detection over a whole screen of pixels)
            sp(&f->o, "%*sfor (uint g%d = 0u; f%d && g%d < %uu; g%d++) {\n", ind, "", h, h, h,
               f->c->stage == STAGE_COMPUTE ? 16384u : 1024u, h);
            flow_emit_region(f, l, ind + 2);
            sp(&f->o, "%*s}\n", ind, "");
            continue;
        }
        FlowBlock *x = &f->b[u];
        sp(&f->o, "%*sif (f%d) {\n%*sf%d = false;\n", ind, "", u, ind + 2, "", u);
        for (size_t i = x->first; i <= x->last; i++) {
            if (i % 4 == 0 || flow_kind(f->code[i]) != FLOW_NONE) continue;
            sp(&f->o, "%.*s", (int)(f->off1[i] - f->off0[i]), f->c->body.buf + f->off0[i]);
        }
        if (x->brx >= 0) {
            // a jump table (a switch the shader's compiler made): to the block its target starts, as BRX computes it
            char buf[16];
            uint64_t w = f->code[x->last];
            uint32_t pc = (uint32_t)(x->last * 8);
            bool g = !(x->pr == PT && !x->prn);
            int nt = x->nsucc - (g ? 1 : 0);
            if (g) sp(&f->o, "%*sif (%s) {\n", ind + 2, "", pred(f->c, x->pr, x->prn, buf));
            if (x->brx != RZ) f->c->reg_used[x->brx] = 1;
            sp(&f->o, "%*s{ uint t = %uu + %s%d + 0x%08xu; if (((t >> 3) & 3u) == 0u) t += 8u; switch (t) {", ind + 4, "",
               pc + 8, x->brx == RZ ? "0u * " : "r", x->brx == RZ ? 0 : x->brx, (uint32_t)sbits(w, 20, 24));
            for (int k = 0; k < nt; k++)
                if (x->succ[k] >= 0) sp(&f->o, " case %uu: f%d = true; break;", (uint32_t)(f->b[x->succ[k]].first * 8), x->succ[k]);
            sp(&f->o, " default: break; } }\n");
            if (g) {
                sp(&f->o, "%*s} else {\n", ind + 2, "");
                flow_jump(f, x->succ[x->nsucc - 1], ind + 4);
                sp(&f->o, "%*s}\n", ind + 2, "");
            }
        } else if (x->nsucc == 2) {
            char buf[16];
            sp(&f->o, "%*sif (%s) {\n", ind + 2, "", pred(f->c, x->pr, x->prn, buf));
            flow_jump(f, x->succ[0], ind + 4);
            sp(&f->o, "%*s} else {\n", ind + 2, "");
            flow_jump(f, x->succ[1], ind + 4);
            sp(&f->o, "%*s}\n", ind + 2, "");
        } else if (x->nsucc == 1) {
            flow_jump(f, x->succ[0], ind + 2);
        }
        sp(&f->o, "%*s}\n", ind, "");
    }
    free(q), free(indeg), free(ord), free(present), free(edge);
}

// The structured body (MSL spelling, as c->body), or NULL: keep the state machine.
// (LARECOMP_LOG_STRUCTURE=1: why a shader kept the state machine)
#define FLOW_FAIL(why) do { if (getenv("LARECOMP_LOG_STRUCTURE")) fprintf(stderr, "[shader] structure: case %d at block %d\n", why, f.nb); goto out; } while (0)
static char *structure_body(Ctx *c, const uint64_t *code, size_t end, const uint8_t *label, const size_t *off0,
                            const size_t *off1) {
    static int disabled = -1;
    if (disabled < 0) disabled = getenv("LARECOMP_SHADER_PC_MACHINE") != NULL;
    if (disabled) return NULL;
    Flow f = {.c = c, .code = code, .off0 = off0, .off1 = off1};
    int *bof = malloc(sizeof(int) * (end + 1)), *work = NULL;
    f.b = calloc(end + 1, sizeof *f.b);
    uint8_t *lab = malloc(end + 1);
    size_t *brx_end = calloc(end + 1, sizeof(size_t));
    // BRX (a jump table): any instruction from it up to where its cases meet may be a target - the nearest SSY / PBK
    // target past it, or else the nearest target of an unconditional branch forward from its cases
    // (not the translator's labels: with a BRX those are every instruction) the targets of BRA / SSY / PBK / PCNT
    memset(lab, 0, end + 1);
    lab[1] = 1;
    for (size_t j = 0; j < end; j++) {
        int k = j % 4 ? flow_kind(code[j]) : FLOW_NONE;
        if (k != FLOW_BRA && k != FLOW_SSY && k != FLOW_PBK && k != FLOW_PCNT) continue;
        int64_t t = norm_target((int64_t)(j * 8) + 8 + sbits(code[j], 20, 24));
        if (t >= 0 && (size_t)(t / 8) < end) lab[t / 8] = 1;
    }
    (void)label;
    for (size_t i = 0; i < end; i++) {
        if (i % 4 == 0 || flow_kind(code[i]) != FLOW_BRX) continue;
        if (brx_of(c, i) >= 0) {  // (its table: the targets are known, and labels already)
            brx_end[i] = i + 1;
            for (int q = 0; q < c->brx[brx_of(c, i)].n; q++) lab[c->brx[brx_of(c, i)].t[q] / 8] = 1;
            continue;
        }
        size_t lim = 0;
        for (size_t j = 0; j < i; j++) {
            int k = j % 4 ? flow_kind(code[j]) : FLOW_NONE;
            if (k != FLOW_SSY && k != FLOW_PBK) continue;
            size_t t = norm_target((int64_t)(j * 8) + 8 + sbits(code[j], 20, 24)) / 8;
            if (t > i && t < end && (!lim || t < lim)) lim = t;
        }
        for (size_t j = i + 1; !lim && j < end; j++) {
            if (j % 4 == 0 || flow_kind(code[j]) != FLOW_BRA || bits(code[j], 16, 4) != PT) continue;
            size_t t = norm_target((int64_t)(j * 8) + 8 + sbits(code[j], 20, 24)) / 8;
            if (t > j && t < end) lim = t;
        }
        if (lim && lim - i <= FLOW_SUCC * 4 / 3 - 4) {
            brx_end[i] = lim;
            for (size_t j = i + 1; j < lim; j++) if (j % 4) lab[j] = 1;
            continue;
        }
        // else (a table of whole sections, each ending the program: compute shaders' switch on a constant) the
        // targets can only be where code starts that nothing falls into: after an unconditional terminator, or a label
        brx_end[i] = end;
        for (size_t j = i + 1, prev = i; j < end; j++) {
            if (j % 4 == 0) continue;
            int pk = flow_kind(code[prev]);
            if (flow_leaves(pk) && bits(code[prev], 16, 4) == PT) lab[j] = 1;
            prev = j;
        }
    }
    label = lab;
    // blocks: from labels and after flow that leaves, to such flow
    for (size_t i = 0; i < end; i++) {
        bof[i] = -1;
        if (i % 4 == 0) continue;
        int pk = f.nb ? flow_kind(code[f.b[f.nb - 1].last]) : FLOW_NONE;
        if (!f.nb || label[i] || flow_leaves(pk) || pk == FLOW_BRX) f.b[f.nb++].first = i;
        f.b[f.nb - 1].last = i;
        bof[i] = f.nb - 1;
    }
    if (!f.nb) FLOW_FAIL(2);
    // successors, walking the stacks from the entry (a block must always be entered with the same stacks)
    work = malloc(sizeof(int) * f.nb);
    int nw = 0;
    f.b[0].seen = true;
    work[nw++] = 0;
    while (nw) {
        int id = work[--nw];
        FlowBlock *x = &f.b[id];
        FlowStacks st = x->in, taken = st;
        int tgt = -2;  // -2: no flow at the end, -1: the end of the program
        bool guarded = false;
        x->pr = PT, x->prn = false, x->brx = -1;
        for (size_t i = x->first; i <= x->last; i++) {
            if (i % 4 == 0) continue;
            uint64_t w = code[i];
            int k = flow_kind(w);
            uint32_t t = norm_target((int64_t)(i * 8) + 8 + sbits(w, 20, 24));
            if (k == FLOW_SSY || k == FLOW_PBK || k == FLOW_PCNT) {
                int s = k - FLOW_SSY;
                if (st.n[s] >= FLOW_DEPTH) FLOW_FAIL(3);
                st.v[s][st.n[s]++] = t;
                continue;
            }
            if (k == FLOW_BRX && i == x->last) {
                int pr = (int)bits(w, 16, 3);
                bool prn = bits(w, 19, 1);
                if (pr == PT && prn) continue;
                guarded = !(pr == PT && !prn);
                x->pr = pr, x->prn = prn, x->brx = (int)bits(w, 8, 8);
                continue;
            }
            if (!flow_leaves(k)) continue;
            int pr = (int)bits(w, 16, 3);
            bool prn = bits(w, 19, 1);
            if (pr == PT && prn) continue;  // @!pt: never taken
            guarded = !(pr == PT && !prn);
            x->pr = pr, x->prn = prn;
            taken = st;
            uint32_t to = 0;
            if (k == FLOW_BRA) to = t;
            else if (k == FLOW_SYNC) { if (!taken.n[0]) FLOW_FAIL(4); to = taken.v[0][--taken.n[0]]; }
            else if (k == FLOW_BRK) { if (!taken.n[1]) FLOW_FAIL(5); to = taken.v[1][--taken.n[1]]; }
            else if (k == FLOW_CONT) { if (!taken.n[2]) FLOW_FAIL(6); to = taken.v[2][taken.n[2] - 1]; }
            if (k == FLOW_EXIT) tgt = -1;
            else if (to / 8 < end && bof[to / 8] >= 0 && f.b[bof[to / 8]].first == to / 8) tgt = bof[to / 8];
            else FLOW_FAIL(7);
        }
        int n = 0;
        FlowStacks sst[FLOW_SUCC];
        if (x->brx >= 0 && brx_of(c, x->last) >= 0) {  // the table's targets, with the stacks as they are
            int b = brx_of(c, x->last);
            for (int q = 0; q < c->brx[b].n; q++) {
                size_t j = c->brx[b].t[q] / 8;
                if (bof[j] < 0 || f.b[bof[j]].first != j) FLOW_FAIL(13);
                if (n == FLOW_SUCC - 1) FLOW_FAIL(12);
                x->succ[n] = bof[j], sst[n++] = st;
            }
            if (guarded) x->succ[n] = id + 1 < f.nb ? id + 1 : -1, sst[n++] = st;
        } else if (x->brx >= 0) {  // every possible target, with the stacks as they are (then the fallthrough if guarded)
            for (size_t j = x->last + 1; j < brx_end[x->last]; j++)
                if (j % 4 && bof[j] >= 0 && f.b[bof[j]].first == j) {
                    if (n == FLOW_SUCC - 1) FLOW_FAIL(12);
                    x->succ[n] = bof[j], sst[n++] = st;
                }
            if (guarded) x->succ[n] = id + 1 < f.nb ? id + 1 : -1, sst[n++] = st;
        } else {
            if (tgt != -2) x->succ[n] = tgt, sst[n++] = taken;
            if (tgt == -2 || guarded) x->succ[n] = id + 1 < f.nb ? id + 1 : -1, sst[n++] = st;
        }
        x->nsucc = n;
        for (int k = 0; k < n; k++) {
            int s = x->succ[k];
            if (s < 0) continue;
            if (!f.b[s].seen) f.b[s].seen = true, f.b[s].in = sst[k], work[nw++] = s;
            else if (!stacks_eq(&f.b[s].in, &sst[k])) {
                if (getenv("LARECOMP_LOG_STRUCTURE"))
                    fprintf(stderr, "[shader] structure: pc %zx from pc %zx: stacks %d/%d/%d top %x/%x, before %d/%d/%d top %x/%x\n",
                            f.b[s].first * 8, x->last * 8, sst[k].n[0], sst[k].n[1], sst[k].n[2],
                            sst[k].n[0] ? sst[k].v[0][sst[k].n[0] - 1] : 0, sst[k].n[1] ? sst[k].v[1][sst[k].n[1] - 1] : 0,
                            f.b[s].in.n[0], f.b[s].in.n[1], f.b[s].in.n[2],
                            f.b[s].in.n[0] ? f.b[s].in.v[0][f.b[s].in.n[0] - 1] : 0, f.b[s].in.n[1] ? f.b[s].in.v[1][f.b[s].in.n[1] - 1] : 0);
                FLOW_FAIL(8);
            }
        }
    }
    // loops: back edges of a depth-first walk; reducible flow only (the loop's header dominates the edge's source)
    {
        int nb = f.nb, *state = calloc(nb, sizeof(int)), *stk = malloc(sizeof(int) * nb * 2), ns = 0;
        int *rpo = malloc(sizeof(int) * nb), nr = nb, *order = malloc(sizeof(int) * nb), *idom = malloc(sizeof(int) * nb);
        uint8_t *back = calloc((size_t)nb * FLOW_SUCC, 1);
        bool ok = true;
        stk[ns++] = 0, stk[ns++] = 0, state[0] = 1;
        while (ns) {
            int id = stk[ns - 2], k = stk[ns - 1];
            if (k < f.b[id].nsucc) {
                stk[ns - 1]++;
                int s = f.b[id].succ[k];
                if (s < 0) continue;
                if (state[s] == 1) back[id * FLOW_SUCC + k] = 1;
                else if (!state[s]) state[s] = 1, stk[ns++] = s, stk[ns++] = 0;
            } else {
                state[id] = 2, rpo[--nr] = id, ns -= 2;
            }
        }
        for (int i = 0; i < nb; i++) order[i] = -1, idom[i] = -1;
        for (int i = nr; i < nb; i++) order[rpo[i]] = i;
        idom[0] = 0;
        for (bool changed = true; changed;) {
            changed = false;
            for (int i = nr + 1; i < nb; i++) {
                int id = rpo[i], nd = -1;
                for (int p = 0; p < nb; p++)
                    for (int k = 0; k < f.b[p].nsucc; k++) {
                        if (f.b[p].succ[k] != id || idom[p] < 0 || back[p * FLOW_SUCC + k]) continue;
                        if (nd < 0) { nd = p; continue; }
                        int u = p, v = nd;
                        while (u != v) {
                            while (order[u] > order[v]) u = idom[u];
                            while (order[v] > order[u]) v = idom[v];
                        }
                        nd = u;
                    }
                if (nd >= 0 && idom[id] != nd) idom[id] = nd, changed = true;
            }
        }
        int *qq = malloc(sizeof(int) * nb);
        for (int p = 0; p < nb && ok; p++)
            for (int k = 0; k < f.b[p].nsucc && ok; k++) {
                if (!back[p * FLOW_SUCC + k]) continue;
                int h = f.b[p].succ[k], d = p;
                while (d != h && d != 0) d = idom[d];
                if (d != h) { ok = false; break; }
                int li = -1;
                for (int l = 0; l < f.nloops; l++) if (f.loops[l].header == h) li = l;
                if (li < 0) {
                    f.loops = realloc(f.loops, sizeof *f.loops * (f.nloops + 1));
                    f.loops[f.nloops] = (FlowLoop){h, -1, 1, calloc(nb, 1)};
                    f.loops[f.nloops].in[h] = 1;
                    li = f.nloops++;
                }
                // the natural loop: what reaches the edge's source without passing the header
                int nq = 0;
                if (!f.loops[li].in[p]) f.loops[li].in[p] = 1, f.loops[li].size++, qq[nq++] = p;
                while (nq) {
                    int y = qq[--nq];
                    for (int z = 0; z < nb; z++)
                        for (int kk = 0; kk < f.b[z].nsucc; kk++)
                            if (f.b[z].succ[kk] == y && f.b[z].seen && !f.loops[li].in[z])
                                f.loops[li].in[z] = 1, f.loops[li].size++, qq[nq++] = z;
                }
            }
        free(qq), free(state), free(stk), free(rpo), free(order), free(idom), free(back);
        if (!ok) FLOW_FAIL(9);
    }
    // nesting: a loop's parent is the smallest other loop holding its header; loops must nest or be apart
    for (int l = 0; l < f.nloops; l++) {
        int best = -1;
        for (int m = 0; m < f.nloops; m++)
            if (m != l && f.loops[m].in[f.loops[l].header] && (best < 0 || f.loops[m].size < f.loops[best].size)) best = m;
        f.loops[l].parent = best;
        if (best >= 0 && f.loops[best].size <= f.loops[l].size) FLOW_FAIL(10);
        for (int i = 0; best >= 0 && i < f.nb; i++) if (f.loops[l].in[i] && !f.loops[best].in[i]) FLOW_FAIL(11);
    }
    for (int i = 0; i < f.nb; i++) {
        f.b[i].loop = -1;
        for (int l = 0; l < f.nloops; l++)
            if (f.loops[l].in[i] && (f.b[i].loop < 0 || f.loops[l].size < f.loops[f.b[i].loop].size)) f.b[i].loop = l;
    }
    for (int i = 0; i < f.nb; i++) if (f.b[i].seen) sp(&f.o, "  bool f%d = %s;\n", i, i ? "false" : "true");
    flow_emit_region(&f, -1, 2);
    if (f.fail) {
        if (getenv("LARECOMP_LOG_STRUCTURE")) fprintf(stderr, "[shader] structure: region order\n");
        free(f.o.buf);
        f.o.buf = NULL;
    }
out:
    for (int l = 0; l < f.nloops; l++) free(f.loops[l].in);
    free(f.loops), free(f.b), free(bof), free(work);
    free(lab), free(brx_end);
    return f.o.buf;
}

// ------------------------------------------------------------------ nested structured flow (from mamcjr/LASRecompAndroid, MIT)
// The state machine's blocks as if / else / for, once the stacks are resolved (resolve_stacks): program order is the
// structure the shader compiler emitted - a conditional forward branch over code is an if (with an else when the
// skipped part ends by jumping over what follows), a backward branch closes a loop, and jumps to a loop's header or
// exit are continue / break. Anything else (a jump out of nested ifs, out of two loops, into the middle of a region)
// fails, and the shader goes to structure_body (blocks behind flags), which handles more shapes but gives every block
// a flag test and a join of its own: the lit shaders' light loop tested seven flags per light.
typedef struct { uint32_t s, e; int term, tgt, brx; bool code; char cond[8]; } SBlk;  // term: 0 none, 1 jump, 2 exit, 3 BRX
typedef struct {
    const Ctx *c;
    const char *src;
    const size_t *us, *ue;
    const uint64_t *w;
    uint8_t *skip;  // per instruction: no code of its own here (flow, SSY / PBK / PCNT)
    SBlk *b;
    int nb, *latch, nloops, loops[32][2];
    int nsw, sw_loops[16];  // open GLSL switches (st_switch), each with the loop count when it opened
    uint8_t *consumed;
    Str out;
    bool fail, exit_used;
} SCtx;

static void st_fail(SCtx *x, int k, const char *why) {
    if (!x->fail && flow_log())
        fprintf(stderr, "[flow] structure failed at %x (block %d of %d, %d loops, %d switches): %s\n",
                k >= 0 && k < x->nb ? x->b[k].s * 8 : 0, k, x->nb, x->nloops, x->nsw, why);
    x->fail = true;
}

static int st_thread(SCtx *x, int b) {  // through blocks that are only an unconditional jump
    for (int n = 0; b >= 0 && b < x->nb && n < x->nb; n++) {
        SBlk *k = &x->b[b];
        if (k->code || k->term != 1 || strcmp(k->cond, "true")) break;
        b = k->tgt;
    }
    return b;
}

// The statement for a jump to block t (-1: EXIT) inside the current loops, or NULL when t is no loop target.
static const char *st_jump(SCtx *x, int t) {
    if (t < 0) {
        x->exit_used = true;
        return x->nloops || x->nsw ? "exit_now = true; break;" : "break;";
    }
    for (int l = x->nloops - 1; l >= 0; l--) {
        int h = x->loops[l][0], e = x->loops[l][1];
        bool cont = t == h, brk = t == e || st_thread(x, t) == st_thread(x, e);
        if (!cont && !brk) continue;
        if (l != x->nloops - 1) { st_fail(x, -1, cont ? "continue out of two loops" : "break out of two loops"); return NULL; }
        if (brk && x->nsw && x->sw_loops[x->nsw - 1] > l) { st_fail(x, -1, "break out of a switch"); return NULL; }
        return cont ? "continue;" : "break;";
    }
    return NULL;
}

static void st_code(SCtx *x, int k) {
    for (uint32_t i = x->b[k].s; i < x->b[k].e; i++)
        if (i % 4 && !x->skip[i]) sp(&x->out, "%.*s", (int)(x->ue[i] - x->us[i]), x->src + x->us[i]);
}

static int st_switch(SCtx *x, int k, int end, int depth);

static void st_range_alt(SCtx *x, int a, int end, int opened, int depth, int alt);
static void st_range(SCtx *x, int a, int end, int opened, int depth) { st_range_alt(x, a, end, opened, depth, -1); }
static void st_range_alt(SCtx *x, int a, int end, int opened, int depth, int alt) {
    if (depth > 64) { st_fail(x, a, "nesting over 64"); return; }
    for (int k = a; k < end && !x->fail;) {
        if (x->latch[k] >= 0 && k != opened) {  // a loop: [k, its last back edge]
            int le = x->latch[k] + 1;
            if (le > end || x->nloops == 32) { st_fail(x, k, le > end ? "loop latch past the region" : "32 loops"); return; }
            sp(&x->out, "  for (;;) {\n");
            x->loops[x->nloops][0] = k, x->loops[x->nloops][1] = le, x->nloops++;
            st_range(x, k, le, k, depth + 1);
            x->nloops--;
            sp(&x->out, "  break;\n  }\n");
            if (x->exit_used) sp(&x->out, "  if (exit_now) break;\n");
            k = le;
            continue;
        }
        SBlk *b = &x->b[k];
        st_code(x, k);
        if (!b->term || x->consumed[k]) { k++; continue; }
        if (b->term == 3) {
            k = st_switch(x, k, end, depth);
            if (k < 0 || x->fail) return;
            continue;
        }
        bool always = !strcmp(b->cond, "true");
        const char *j = st_jump(x, b->term == 2 ? -1 : b->tgt);
        if (x->fail) return;
        if (j) {
            if (always) sp(&x->out, "  %s\n", j);
            else sp(&x->out, "  if (%s) { %s }\n", b->cond, j);
            k++;
            continue;
        }
        int t = b->tgt;
        if (t == end || (alt >= 0 && t == alt)) {  // the rest of the region, when not taken
            if (always) {
                if (k + 1 < end) st_fail(x, k, "unconditional jump to the region's end with code after it");
                return;
            }
            sp(&x->out, "  if (!(%s)) {\n", b->cond);
            st_range(x, k + 1, end, -1, depth + 1);
            sp(&x->out, "  }\n");
            return;
        }
        if (t <= k || t > end || always) {
            char why[96];
            snprintf(why, sizeof why, "%s jump to %x (region %x-%x%s)", t <= k ? "backward non-loop" : t > end ? "forward out-of-region" : "unconditional",
                     t >= 0 && t < x->nb ? x->b[t].s * 8 : t, x->b[a].s * 8, end < x->nb ? x->b[end].s * 8 : (unsigned)end * 0 + 0xffff, alt >= 0 ? ", alt" : "");
            st_fail(x, k, why);
            return;
        }
        SBlk *last = &x->b[t - 1];
        int J = last->tgt;
        bool els = t - 1 > k && last->term == 1 && !strcmp(last->cond, "true") && J > t && J <= end && !x->consumed[t - 1];
        if (els) {
            int nl = x->nloops;  // (J must be no loop target: those stay jumps)
            bool ex = x->exit_used;
            const char *jj = st_jump(x, J);
            x->exit_used = ex;
            if (x->fail || jj) { x->fail = false; els = false; }
            x->nloops = nl;
        }
        if (!els && alt >= 0 && t - 1 > k && last->term == 1 && !strcmp(last->cond, "true") && J == alt && !x->consumed[t - 1]) {
            x->consumed[t - 1] = 1;
            sp(&x->out, "  if (!(%s)) {\n", b->cond);
            st_range_alt(x, k + 1, t, -1, depth + 1, alt);
            sp(&x->out, "  } else {\n");
            st_range_alt(x, t, end, -1, depth + 1, alt);
            sp(&x->out, "  }\n");
            return;
        }
        sp(&x->out, "  if (!(%s)) {\n", b->cond);
        if (els) {
            x->consumed[t - 1] = 1;
            st_range_alt(x, k + 1, t, -1, depth + 1, J);
            sp(&x->out, "  } else {\n");
            st_range(x, t, J, -1, depth + 1);
            sp(&x->out, "  }\n");
            k = J;
        } else {
            st_range(x, k + 1, t, -1, depth + 1);
            sp(&x->out, "  }\n");
            k = t;
        }
    }
}

// A BRX through a table (block k): `bt` is its computed target, and each target starts a case running to the next
// one (the last: to the region's end); a case must end in a jump or an EXIT, not run into the next. A target that is
// none of them exits, as the state machine's default does.
static int st_switch(SCtx *x, int k, int end, int depth) {
    const Ctx *c = x->c;
    int bx = x->b[k].brx;
    uint64_t w = x->w[x->c->brx[bx].at];
    int ra = (int)bits(w, 8, 8), n = 0;
    int tb[256];
    for (int q = 0; q < c->brx[bx].n; q++) {  // (blocks; the list is a superset: words of cbuf 1 that land on code)
        int ti = (int)(c->brx[bx].t[q] / 8), b = 0;
        while (b < x->nb && x->b[b].s != (uint32_t)ti) b++;
        if (b <= k || b >= x->nb) { st_fail(x, k, "brx target not a block after it"); return -1; }
        tb[n++] = b;
    }
    if (!n || tb[0] != k + 1 || x->nsw == 16) { st_fail(x, k, "brx: code between it and its first case"); return -1; }  // (code between the BRX and its first case)
    // the join: where most cases that end in a jump go, forward past their own start; the table's words past it are
    // no cases (nor past the region's end)
    int J = -1, best = 0;
    for (int q = 0; q + 1 < n; q++) {
        SBlk *last = &x->b[tb[q + 1] - 1];
        if (last->term != 1 || strcmp(last->cond, "true") || last->tgt <= tb[q + 1] - 1) continue;
        int cnt = 0;
        for (int r = 0; r + 1 < n; r++) {
            SBlk *l2 = &x->b[tb[r + 1] - 1];
            cnt += l2->term == 1 && !strcmp(l2->cond, "true") && l2->tgt == last->tgt;
        }
        if (cnt > best) best = cnt, J = last->tgt;
    }
    if (J > end) J = -1;
    int stop = J >= 0 ? J : end;
    while (n && tb[n - 1] >= stop) n--;
    if (!n) { st_fail(x, k, "brx: no cases before the join"); return -1; }
    char r[8];
    if (ra == RZ) strcpy(r, "0u"); else snprintf(r, sizeof r, "r%d", ra);
    sp(&x->out, "  { uint bt = %uu + %s + 0x%08xu; if (((bt >> 3) & 3u) == 0u) bt += 8u;\n  switch (bt) {\n",
       x->c->brx[bx].at * 8 + 8, r, (uint32_t)sbits(w, 20, 24));
    x->sw_loops[x->nsw++] = x->nloops;
    for (int q = 0; q < n && !x->fail; q++) {
        int e = q + 1 < n ? tb[q + 1] : stop;
        SBlk *last = &x->b[e - 1];
        // ends in a jump to the join or an EXIT: a break after it (and a jump to the join inside skips the rest);
        // else it runs into the next case (one of the table's extra words: never a value of bt)
        bool to_join = last->term == 1 && !strcmp(last->cond, "true") && last->tgt == stop;
        bool ends = to_join || (last->term == 2 && !strcmp(last->cond, "true")) || (last->term == 1 && !strcmp(last->cond, "true"));
        if (q == n - 1) ends = true;  // (the last case runs up to the join: running off its end reaches it, as a break does)
        sp(&x->out, "  case %uu: {\n", x->b[tb[q]].s * 8);
        st_range_alt(x, tb[q], e, -1, depth + 1, ends ? stop : -1);
        sp(&x->out, ends ? "  break; }\n" : "  }\n");
    }
    x->nsw--;
    x->exit_used = true;
    sp(&x->out, "  default: exit_now = true; break;\n  }\n  if (exit_now) break;\n  }\n");
    return J;
}

// Rewrites c->body (the state machine's, with per-instruction text in [us[i], ue[i])) as structured code; false: kept
static bool structure(Ctx *c, const uint64_t *code, size_t end, const uint8_t *label, const size_t *us, const size_t *ue) {
    SCtx x = {.c = c, .src = c->body.buf, .us = us, .ue = ue, .w = code};
    x.skip = calloc(end + 1, 1);
    int *blk = malloc((end + 1) * sizeof *blk);
    x.b = calloc(end + 1, sizeof *x.b);
    x.latch = malloc((end + 1) * sizeof *x.latch);
    x.consumed = calloc(end + 1, 1);
    bool ok = x.skip && blk && x.b && x.latch && x.consumed && c->body.buf && c->flow_to;
    int nb = 0;
    bool prev_flow = false;
    for (size_t i = 0; ok && i < end; i++) {
        blk[i] = -1;
        if (i % 4 == 0) continue;
        uint64_t w = code[i];
        uint32_t top = (uint32_t)(w >> 52);
        if (nb == 0 || label[i] || prev_flow) {
            if (nb) x.b[nb - 1].e = (uint32_t)i;
            x.b[nb] = (SBlk){.s = (uint32_t)i, .e = (uint32_t)end};
            nb++;
        }
        blk[i] = nb - 1;
        SBlk *b = &x.b[nb - 1];
        int pr = (int)bits(w, 16, 3);
        bool prn = bits(w, 19, 1);
        bool push = (w & 0xfff0000000000020ull) == 0xe290000000000000ull || (w & 0xfff0000000000020ull) == 0xe2a0000000000000ull ||
                    (w & 0xfff0000000000020ull) == 0xe2b0000000000000ull;
        bool bra = (w & 0xfff0000000000020ull) == 0xe240000000000000ull;
        bool pop = (w & 0xfff8000000000000ull) == 0xf0f8000000000000ull || top == 0xe34 || top == 0xe35;
        bool ex = top == 0xe30;
        int bx = top == 0xe25 ? brx_of(c, i) : -1;
        prev_flow = false;
        if (push) { x.skip[i] = 1; continue; }
        if (bx >= 0 && !(pr == PT && prn)) {
            x.skip[i] = 1;
            prev_flow = true;
            if (pr != PT || prn) { ok = false; if (flow_log()) fprintf(stderr, "[flow] structure failed at %zx: guarded brx\n", i * 8); }
            b->term = 3, b->brx = bx, b->tgt = -1;
            strcpy(b->cond, "true");
            continue;
        }
        if (!(bra || pop || ex) || (pr == PT && prn)) { b->code |= ue[i] > us[i]; continue; }  // (@!PT: never runs)
        x.skip[i] = 1;
        prev_flow = true;
        if (pr == PT) strcpy(b->cond, "true");
        else snprintf(b->cond, sizeof b->cond, "%sp%d", prn ? "!" : "", pr);
        int64_t t = bra ? (int64_t)norm_target((int64_t)i * 8 + 8 + sbits(w, 20, 24)) : ex ? -1 : c->flow_to[i];
        b->term = t < 0 || (size_t)(t / 8) >= end ? 2 : 1;  // (past the end, or a SYNC never reached: the default's exit)
        b->tgt = b->term == 1 ? (int)(t / 8) : -1;  // (instruction index for now)
    }
    x.nb = nb;
    for (int k = 0; ok && k < nb; k++) {
        x.latch[k] = -1;
        if (x.b[k].term != 1) continue;
        int ti = x.b[k].tgt;
        if (blk[ti] < 0 || x.b[blk[ti]].s != (uint32_t)ti) {  // (into the middle of a block)
            ok = false;
            if (flow_log()) fprintf(stderr, "[flow] structure failed at %x: jump into the middle of a block (%x)\n", x.b[k].s * 8, ti * 8);
        }
        else x.b[k].tgt = blk[ti];
    }
    for (int k = 0; ok && k < nb; k++) {  // (a table's targets: block starts too)
        if (x.b[k].term != 3) continue;
        const uint32_t *t = c->brx[x.b[k].brx].t;
        for (int q = 0; q < c->brx[x.b[k].brx].n && ok; q++) {
            size_t ti = t[q] / 8;
            if (blk[ti] < 0 || x.b[blk[ti]].s != (uint32_t)ti) {
                ok = false;
                if (flow_log()) fprintf(stderr, "[flow] structure failed at %x: brx target into the middle of a block (%zx)\n", x.b[k].s * 8, ti * 8);
            }
        }
    }
    for (int k = 0; ok && k < nb; k++)
        if (x.b[k].term == 1 && x.b[k].tgt <= k && k > x.latch[x.b[k].tgt]) x.latch[x.b[k].tgt] = k;
    if (ok) {
        sp(&x.out, "  bool exit_now = false;\n");
        st_range(&x, 0, nb, -1, 0);
    }
    ok = ok && !x.fail;
    if (!ok && flow_log()) {  // the blocks: start-end, terminator (cond > target), loop latch
        fprintf(stderr, "[flow] blocks of the shader with %zu words (word 8: %016llx), %d blocks\n", end, (unsigned long long)code[8], nb);
        for (int k = 0; k < nb; k += 12) {
            char line[1024]; int o = 0;
            for (int q = k; q < nb && q < k + 12; q++) {
                SBlk *b = &x.b[q];
                o += snprintf(line + o, sizeof line - o, " %d:%x-%x%s", q, b->s * 8, b->e * 8, b->code ? "c" : "");
                if (b->term == 1) o += snprintf(line + o, sizeof line - o, "(%s>%d)", b->cond, b->tgt);
                else if (b->term == 2) o += snprintf(line + o, sizeof line - o, "(%s>exit)", b->cond);
                else if (b->term == 3) o += snprintf(line + o, sizeof line - o, "(brx)");
                if (x.latch[q] >= 0) o += snprintf(line + o, sizeof line - o, "L%d", x.latch[q]);
            }
            fprintf(stderr, "[flow]  %s\n", line);
        }
    }
    if (ok) {
        free(c->body.buf);
        c->body = x.out;
    } else free(x.out.buf);
    free(x.skip); free(blk); free(x.b); free(x.latch); free(x.consumed);
    return ok;
}

// ------------------------------------------------------------------ GLSL output
static bool id_start(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_'; }
static bool id_char(char ch) { return id_start(ch) || (ch >= '0' && ch <= '9'); }

// MSL-spelled instruction body -> GLSL (token-aware; comments and numbers are copied as they are).
static void glsl_rewrite(Ctx *c, Str *o, const char *src) {
    static const char *const map[][2] = {
        {"float2", "vec2"}, {"float3", "vec3"}, {"float4", "vec4"}, {"int2", "ivec2"}, {"int3", "ivec3"},
        {"int4", "ivec4"}, {"uint2", "uvec2"}, {"uint3", "uvec3"}, {"uint4", "uvec4"}, {"bool2", "bvec2"},
        {"ulong", "uint64_t"}, {"long", "int64_t"}, {"rsqrt", "inversesqrt"}, {"rint", "roundEven"},
        {"front_facing", "gl_FrontFacing"}, {"point_coord", "gl_PointCoord"}, {"lane", "gl_SubgroupInvocationID"},
        {"vid", "uint(gl_VertexIndex)"}, {"iid", "uint(gl_InstanceIndex)"},
    };
    const char *p = src;
    while (*p) {
        if (p[0] == '/' && p[1] == '*') {
            const char *e = strstr(p + 2, "*/");
            e = e ? e + 2 : p + strlen(p);
            sp(o, "%.*s", (int)(e - p), p);
            p = e;
            continue;
        }
        if (p[0] == '/' && p[1] == '/') {
            const char *e = strchr(p, '\n');
            if (!e) e = p + strlen(p);
            sp(o, "%.*s", (int)(e - p), p);
            p = e;
            continue;
        }
        if (*p >= '0' && *p <= '9') {  // number (0x..u, 1.0f, ...)
            const char *e = p;
            while (id_char(*e) || *e == '.') e++;
            sp(o, "%.*s", (int)(e - p), p);
            p = e;
            continue;
        }
        if (!id_start(*p)) { sp(o, "%c", *p); p++; continue; }
        const char *e = p;
        while (id_char(*e)) e++;
        int n = (int)(e - p);
        char id[64];
        if (n >= (int)sizeof id || (p > src && p[-1] == '.')) {  // member / swizzle
            sp(o, "%.*s", n, p);
            p = e;
            continue;
        }
        memcpy(id, p, n);
        id[n] = 0;
        p = e;
        if (!strcmp(id, "as_type") && *p == '<') {
            const char *gt = strchr(p, '>');
            if (!gt) { fail(c, "glsl: bad as_type"); return; }
            int tn = (int)(gt - p - 1);
            if (tn == 5 && !strncmp(p + 1, "float", 5)) sp(o, "as_f");
            else if (tn == 4 && !strncmp(p + 1, "uint", 4)) sp(o, "as_u");
            else if (tn == 3 && !strncmp(p + 1, "int", 3)) sp(o, "as_i");
            else if (tn == 5 && !strncmp(p + 1, "half2", 5)) sp(o, "unpack_h2");
            else { fail(c, "glsl: as_type<%.*s>", tn, p + 1); return; }
            p = gt + 1;
            continue;
        }
        if ((!strcmp(id, "in") || !strcmp(id, "out")) && *p == '.') {  // stage I/O struct members
            const char *m = p + 1, *me = m;
            while (id_char(*me)) me++;
            char mem[32];
            snprintf(mem, sizeof mem, "%.*s", (int)(me - m), m);
            if (!strcmp(mem, "pos")) sp(o, c->stage == STAGE_FRAGMENT ? "gl_FragCoord" : "gl_Position");
            else if (!strcmp(mem, "psize")) sp(o, "gl_PointSize");
            else sp(o, "%s", mem);  // a<i> (vertex input), v<i> (varying)
            p = me;
            continue;
        }
        if ((!strcmp(id, "char") || !strcmp(id, "short")) && *p == '(') { sp(o, id[0] == 'c' ? "sx8" : "sx16"); continue; }
        if (!strcmp(id, "tid") || !strcmp(id, "ctaid")) {
            sp(o, c->stage != STAGE_COMPUTE ? "uvec3(0u)" : id[0] == 't' ? "gl_LocalInvocationID" : "gl_WorkGroupID");
            continue;
        }
        if (id[0] == 'r' && n > 1 && n < 6) {  // registers beyond r255 (RZ) come from out-of-range encodings
            bool num = true;
            for (int i = 1; i < n; i++) num = num && id[i] >= '0' && id[i] <= '9';
            if (num && atoi(id + 1) > 255) { fail(c, "register %s out of range", id); return; }
        }
        const char *rep = id;
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (!strcmp(id, map[i][0])) { rep = map[i][1]; break; }
        if (!strcmp(id, "lane") && c->stage == STAGE_FRAGMENT && !maxwell_frag_subgroups) rep = "quad_lane()";
        sp(o, "%s", rep);
    }
}

static const char glsl_prelude[] =
    "#version 460\n"
    "#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require\n"
    "#extension GL_KHR_shader_subgroup_basic : require\n"
    "#extension GL_KHR_shader_subgroup_shuffle : require\n"
    "#extension GL_KHR_shader_subgroup_shuffle_relative : require\n"
    "#extension GL_EXT_texture_shadow_lod : require\n"
    "uint as_u(float x) { return floatBitsToUint(x); }\n"
    "uint as_u(uint x) { return x; }\n"
    "uint as_u(int x) { return uint(x); }\n"
    "float as_f(uint x) { return uintBitsToFloat(x); }\n"
    "float as_f(int x) { return intBitsToFloat(x); }\n"
    "float as_f(float x) { return x; }\n"
    "int as_i(uint x) { return int(x); }\n"
    "int as_i(float x) { return floatBitsToInt(x); }\n"
    "int sx8(uint v) { return bitfieldExtract(int(v), 0, 8); }\n"
    "int sx8(int v) { return bitfieldExtract(v, 0, 8); }\n"
    "int sx16(uint v) { return bitfieldExtract(int(v), 0, 16); }\n"
    "int sx16(int v) { return bitfieldExtract(v, 0, 16); }\n"
    "uint clz(uint v) { return v == 0u ? 32u : uint(31 - findMSB(v)); }\n"
    "uint brev32(uint v) { return bitfieldReverse(v); }\n"
    "uint flo_sh(uint msb) { return msb == 0xFFFFFFFFu ? msb : 31u - msb; }\n"
    "uint popcount(uint v) { return uint(bitCount(v)); }\n"
    "uint mulhi(uint a, uint b) { uint hi, lo; umulExtended(a, b, hi, lo); return hi; }\n"
    "int mulhi(int a, int b) { int hi, lo; imulExtended(a, b, hi, lo); return hi; }\n"
    "uint bfe_u(uint v, uint ctl) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u) return 0u; if (pos >= 32u) return 0u; return bitfieldExtract(v, int(pos), int(min(len, 32u - pos))); }\n"
    "uint bfe_s(uint v, uint ctl) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u) return 0u; if (pos >= 32u) return (int(v) < 0) ? 0xFFFFFFFFu : 0u; return uint(bitfieldExtract(int(v), int(pos), int(min(len, 32u - pos)))); }\n"
    "uint bfi(uint ins, uint ctl, uint base) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u || pos >= 32u) return base; return bitfieldInsert(base, ins, int(pos), int(min(len, 32u - pos))); }\n"
    "float fswzadd(float a, float b, uint swz, uint lane) { uint m = (swz >> ((lane & 3u) * 2u)) & 3u; float ma = m == 1u ? -1.0f : (m == 3u ? 0.0f : 1.0f); float mb = m == 2u ? -1.0f : 1.0f; return a * ma + b * mb; }\n"
    "uvec2 rsc(uvec2 c, vec4 s) { return (s.x == 1.0f && s.y == 1.0f) ? c : uvec2(floor((vec2(c) + 0.5f) * s.xy)); }\n"
    // a scaled texture whose contents occupy its top-left part (Dynamic resolution): sample that part
    "vec2 uvs(vec2 uv, vec4 s) { return (s.z == 1.0f && s.w == 1.0f) ? uv : min(uv, 1.0f) * s.zw; }\n";
// MSL semantics: saturate(NaN) = 0, fmin/fmax return the non-NaN operand - as Maxwell's .SAT and FMNMX. On a GPU whose
// min / max / clamp do the same by themselves (maxwell_ieee_minmax, probed by device.c; Adreno) the plain functions:
// the isnan selects cost two to four instructions a site, and keep the Adreno compiler from folding the clamp into
// the (sat) bit of the instruction that made the value (it only does so for that value's single use).
static const char glsl_prelude_minmax[] =
    "float saturate(float x) { return isnan(x) ? 0.0f : clamp(x, 0.0f, 1.0f); }\n"
    "float fmin(float a, float b) { return isnan(a) ? b : (isnan(b) ? a : min(a, b)); }\n"
    "float fmax(float a, float b) { return isnan(a) ? b : (isnan(b) ? a : max(a, b)); }\n";
static const char glsl_prelude_minmax_ieee[] =
    "float saturate(float x) { return clamp(x, 0.0f, 1.0f); }\n"
    "float fmin(float a, float b) { return min(a, b); }\n"
    "float fmax(float a, float b) { return max(a, b); }\n";
static const char glsl_prelude_minmax2[] = "vec2 saturate(vec2 x) { return vec2(saturate(x.x), saturate(x.y)); }\n";
// Run-time indexed constant buffers: cbr a word at a byte offset, cbra a word of a row from a 16-aligned register
// (cbuf_ref_aligned). The index clamped to the block (an out-of-range guest index reads the last row). The variant
// without the clamp, for a device that reads zeros past a buffer (maxwell_robust_cbuf), is an experiment that lost
// (glsl_finish: LARECOMP_VK_ROBUST_CBUF).
static const char glsl_prelude_cbr[] =
    "#define cbr(cb, off) cb[min((off) >> 4, uint(cb.length() - 1))][((off) >> 2) & 3u]\n"
    "#define cbra(cb, row, k, comp) cb[min(((row) + (k)) & 0x0fffffffu, uint(cb.length() - 1))][comp]\n";
static const char glsl_prelude_cbr_robust[] =
    "#define cbr(cb, off) cb[(off) >> 4][((off) >> 2) & 3u]\n"
    "#define cbra(cb, row, k, comp) cb[((row) + (k)) & 0x0fffffffu][comp]\n";
static const char glsl_prelude_h2f_bits[] =
    // half -> float by bits, not unpackHalf2x16: NVIDIA's driver recognises unpacked halves and runs some of the
    // fp32 math after them in native fp16, differently per shader, so a depth pre-pass and its colour pass (EQUAL
    // test) placed wind-animated foliage apart: black slashes / flicker (tools: notes/PLAN.md 2026-10-08)
    "float h2f(uint h) { uint s = (h & 0x8000u) << 16, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;"
    " if (e == 0u) { float d = float(m) * 5.9604644775390625e-8; return s != 0u ? -d : d; }"
    " if (e == 31u) return uintBitsToFloat(s | 0x7F800000u | (m << 13));"
    " return uintBitsToFloat(s | ((e + 112u) << 23) | (m << 13)); }\n"
    "vec2 unpack_h2(uint v) { return vec2(h2f(v & 0xFFFFu), h2f(v >> 16)); }\n";
// ... and in fragment / compute shaders (and on other GPUs) the conversion instruction: positions don't depend on them
// there, and the by-bits version (~12 integer ops and two branches per half) was half of the main lighting pass's GPU
// time on an Intel UHD 770 and a third on an RTX 3080 Ti. LARECOMP_VK_EXACT_H2F=1: by bits in every stage.
static const char glsl_prelude_h2f_native[] =
    "float h2f(uint h) { return unpackHalf2x16(h).x; }\n"
    "vec2 unpack_h2(uint v) { return unpackHalf2x16(v); }\n";
// Half-precision instructions (HADD2 / HMUL2 / HFMA2 / HSET(P)2): operands through hx, results through hpack.
// maxwell_half_native (phones, shaderFloat16): native fp16 - twice the rate on Adreno, and rounded per operation as the
// Switch's GPU does; else in fp32, rounded when packed.
static const char glsl_prelude_half_native[] =
    "f16vec2 hx(uint v, uint swz) { if (swz == 1u) return f16vec2(float16_t(uintBitsToFloat(v))); f16vec2 h = unpackFloat2x16(v); return swz == 2u ? h.xx : (swz == 3u ? h.yy : h); }\n"
    "uint hpack(f16vec2 r, uint merge, uint old) { if (merge == 1u) return floatBitsToUint(float(r.x)); uint p = packFloat2x16(r); if (merge == 2u) return (old & 0xFFFF0000u) | (p & 0xFFFFu); if (merge == 3u) return (old & 0xFFFFu) | (p & 0xFFFF0000u); return p; }\n";
static const char glsl_prelude2[] =
    "vec2 hx(uint v, uint swz) { if (swz == 1u) return vec2(uintBitsToFloat(v)); vec2 h = unpack_h2(v); return swz == 2u ? h.xx : (swz == 3u ? h.yy : h); }\n"
    // (one conversion of both halves: the same bits as one per half, and the merge is a constant at each call)
    "uint hpack(vec2 r, uint merge, uint old) { if (merge == 1u) return floatBitsToUint(r.x); uint p = packHalf2x16(r); if (merge == 2u) return (old & 0xFFFF0000u) | (p & 0xFFFFu); if (merge == 3u) return (old & 0xFFFFu) | (p & 0xFFFF0000u); return p; }\n"
    "uint prmt(uint a, uint b, uint sel) { uint r = 0u; for (uint i = 0u; i < 4u; i++) { uint s = (sel >> (i * 4u)) & 7u; uint bt = s < 4u ? (a >> (s * 8u)) & 0xFFu : (b >> ((s - 4u) * 8u)) & 0xFFu; r |= bt << (i * 8u); } return r; }\n";

static const char *glsl_sampler_type(const ShaderTexture *t) {
    static const char *const names[][2] = {
        {"sampler1D", "sampler1DShadow"}, {"sampler2D", "sampler2DShadow"}, {"sampler3D", "sampler3D"},
        {"samplerCube", "samplerCubeShadow"}, {"sampler2DArray", "sampler2DArrayShadow"},
        {"samplerCubeArray", "samplerCubeArrayShadow"}, {"sampler1DArray", "sampler1DArrayShadow"}};
    int ty = t->type <= TEXTYPE_1D_ARRAY ? t->type : TEXTYPE_2D;
    static const char *const uint_names[] = {"usampler1D", "usampler2D", "usampler3D", "usamplerCube",
                                           "usampler2DArray", "usamplerCubeArray", "usampler1DArray"};
    if (t->is_uint) return uint_names[ty];
    return names[ty][t->shadow ? 1 : 0];
}

// A cbuf indexed at run time, as words (maxwell_scalar_cbuf): cbN[a][b] -> cbN_s[(a) * 4u + (b)], cbr(cbN, o) -> cbrs(cbN_s, o).
// Each load is then one word: as uvec4 rows a light loop's dozen loads per light took four registers each (the
// driver hoists loads), and the lit shaders ran at two waves per core - latency-bound on a phone GPU.
static char *scalar_cbuf_rewrite(const char *src, int idx) {
    char name[8], cbrn[16], cbran[16];
    snprintf(name, sizeof name, "cb%d", idx);
    snprintf(cbrn, sizeof cbrn, "cbr(cb%d,", idx);
    snprintf(cbran, sizeof cbran, "cbra(cb%d,", idx);
    size_t nl = strlen(name), cl = strlen(cbrn), al = strlen(cbran);
    Str o = {0};
    for (const char *p = src; *p;) {
        bool word = p == src || !(isalnum((unsigned char)p[-1]) || p[-1] == '_');
        if (word && !strncmp(p, cbran, al)) { sp(&o, "cbras(cb%d_s,", idx); p += al; continue; }
        if (word && !strncmp(p, cbrn, cl)) { sp(&o, "cbrs(cb%d_s,", idx); p += cl; continue; }
        if (word && !strncmp(p, name, nl) && p[nl] == '[') {
            // two bracketed indices (each balanced)
            const char *q = p + nl, *a0, *a1, *b0, *b1;
            int d = 0;
            a0 = q + 1;
            for (; *q; q++) { if (*q == '[') d++; else if (*q == ']' && --d == 0) break; }
            if (!*q || q[1] != '[') { sp(&o, "%c", *p++); continue; }
            a1 = q, b0 = q + 2, q++;
            d = 0;
            for (; *q; q++) { if (*q == '[') d++; else if (*q == ']' && --d == 0) break; }
            if (!*q) { sp(&o, "%c", *p++); continue; }
            b1 = q;
            sp(&o, "cb%d_s[(%.*s) * 4u + (%.*s)]", idx, (int)(a1 - a0), a0, (int)(b1 - b0), b0);
            p = q + 1;
            continue;
        }
        const char *n = p + 1;
        while (*n && *n != 'c') n++;  // (copy up to the next possible 'cb')
        sp(&o, "%.*s", (int)(n - p), p);
        p = n;
    }
    return o.buf ? o.buf : strdup("");
}

// (GLSL fragment, lazy heap chunks) Early depth / stencil tests for a shader that counts chunk misses (GMiss atomics):
// only where the tests' outcome can't depend on the shader - no discard, no depth output (a sample mask output
// isn't translated) - and the shader has no side effect of its own (stores), so whether a hidden fragment runs it is
// not observable. (Alpha to
// coverage with early tests would write depth for samples it drops: the renderer uses this shader's module without
// the execution mode for those pipelines, vulkan/exec.c fs_late.)
static bool glsl_early_tests(const Ctx *c) {
    return c->glsl && c->stage == STAGE_FRAGMENT && c->info->uses_gmem && maxwell_gmem_chunk_shift && maxwell_gmem_lazy &&
           !maxwell_gmem_fs_miss && !c->info->uses_discard && !c->omap_depth && !c->uses_gmem_store;
}

// Declarations + entry point around the (rewritten) body; mirrors the MSL assembly in maxwell_translate.
static ShaderInfo *glsl_finish(Ctx *c, bool any_branch, bool dbg) {
    ShaderInfo *info = c->info;
    const ShaderKey *key = c->key;
    int stage = c->stage;
    Str body = {0};
    glsl_rewrite(c, &body, c->body.buf ? c->body.buf : "");
    if (info->uses_gmem && (info->cbuf_mask & (1u << GMEM_BUFFER_INDEX))) fail(c, "cbuf %d clashes with gmem", GMEM_BUFFER_INDEX);
    if (info->cbuf_mask >> 18) fail(c, "cbuf index out of range");
    for (int i = 0; i < info->ntex; i++)
        if (info->tex[i].shadow && info->tex[i].type == TEXTYPE_3D) fail(c, "3d shadow texture");
    if (c->fail) { free(body.buf); free(c->body.buf); free(c); return info; }
    int set = stage == STAGE_FRAGMENT ? 1 : 0;
    Str s = {0};
    // LARECOMP_VK_ROBUST_CBUF=1: no clamp where the device is robust. Off by default: the clamp's min() is what keeps
    // the row * 16 + word shape the Adreno compiler's alignment analysis wants - without it `(r >> 4) << 4` folds to a
    // mask, the loads go back to a vec4 and a chain of selects, and the Field's skinned grass needed 21 registers
    // instead of 12 (801 instead of 714 instructions), the lit shaders 34 more each.
    static int robust_env = -1;
    if (robust_env < 0) robust_env = getenv("LARECOMP_VK_ROBUST_CBUF") ? atoi(getenv("LARECOMP_VK_ROBUST_CBUF")) != 0 : 0;
    bool robust = maxwell_robust_cbuf && robust_env;
    {
        const char *rest = strchr(glsl_prelude, '\n') + 1;  // (after #version: the fp16 extension goes first)
        const char *version_end = rest;
        if (!maxwell_shader_int64) rest = strchr(rest, '\n') + 1; // skip only the int64 extension
        sp(&s, "%.*s", (int)(version_end - glsl_prelude), glsl_prelude);
        if (info->uses_gmem && maxwell_gmem_chunk_shift && maxwell_gmem_lazy) sp(&s, "#extension GL_EXT_buffer_reference : require\n");
        if (c->uses_vote) sp(&s, "#extension GL_KHR_shader_subgroup_ballot : require\n");
        if (maxwell_half_native) sp(&s, "#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require\n");
        if (maxwell_scalar_cbuf && info->cbuf_dynamic) sp(&s, "#extension GL_EXT_scalar_block_layout : require\n");
        static int exact = -1;
        if (exact < 0) exact = getenv("LARECOMP_VK_EXACT_H2F") && atoi(getenv("LARECOMP_VK_EXACT_H2F")) != 0;
        bool bits = exact || (maxwell_h2f_bits && stage != STAGE_FRAGMENT && stage != STAGE_COMPUTE);
        sp(&s, "%s%s%s%s%s", robust ? glsl_prelude_cbr_robust : glsl_prelude_cbr, rest,
           maxwell_ieee_minmax ? glsl_prelude_minmax_ieee : glsl_prelude_minmax, glsl_prelude_minmax2,
           bits ? glsl_prelude_h2f_bits : glsl_prelude_h2f_native);
        if (!maxwell_shader_int64) {
        sp(&s, "uint add32(uint a, uint b, uint ci, out uint co) { uint x, y; uint v = uaddCarry(a, b, x); "
               "v = uaddCarry(v, ci, y); co = (x + y) & 1u; return v; }\n"
               "uint add3_32(uint a, uint b, uint c, uint ci, out uint co) { uint x, y, z; uint v = uaddCarry(a, b, x); "
               "v = uaddCarry(v, c, y); v = uaddCarry(v, ci, z); co = (x + y + z) & 1u; return v; }\n"
               "uint shf32(uint lo, uint hi, uint s, bool right, bool signhi) { "
               "if (right) { if (s >= 64u) return signhi ? uint(int(hi) >> 31) : 0u; "
               "if (s >= 32u) return signhi ? uint(int(hi) >> (s - 32u)) : hi >> (s - 32u); "
               "if (s == 0u) return lo; return (lo >> s) | (hi << (32u - s)); } "
               "if (s >= 64u) return 0u; if (s >= 32u) return lo << (s - 32u); "
               "if (s == 0u) return hi; return (hi << s) | (lo >> (32u - s)); }\n");
        }
        if (maxwell_half_native) {
            const char *hp = strstr(glsl_prelude2, "uint prmt(");  // (the fp32 hx / hpack replaced)
            sp(&s, "%s%s%s", glsl_prelude_half_native,
               maxwell_ieee_minmax ? "f16vec2 saturate(f16vec2 x) { return clamp(x, f16vec2(0.0hf), f16vec2(1.0hf)); }\n"
                                   : "f16vec2 saturate(f16vec2 x) { return mix(clamp(x, f16vec2(0.0hf), f16vec2(1.0hf)), f16vec2(0.0hf), isnan(x)); }\n",
               hp);
        } else {
            sp(&s, "%s", glsl_prelude2);
        }
    }
    if (stage == STAGE_FRAGMENT && !maxwell_frag_subgroups)  // the quad's lanes through fine derivatives (SHFL above)
        sp(&s, "uint quad_lane() { ivec2 p = ivec2(gl_FragCoord.xy); return uint(p.x & 1) | (uint(p.y & 1) << 1); }\n"
               "uint quad_xor(uint v, uint m) { vec2 h = vec2(float(v & 0xFFFFu), float(v >> 16)); ivec2 p = ivec2(gl_FragCoord.xy);"
               " if ((m & 1u) != 0u) { vec2 d = dFdxFine(h); h = (p.x & 1) == 0 ? h + d : h - d; }"
               " if ((m & 2u) != 0u) { vec2 d = dFdyFine(h); h = (p.y & 1) == 0 ? h + d : h - d; }"
               " return uint(h.x + 0.5) | (uint(h.y + 0.5) << 16); }\n");
    for (int i = 0; i < 18; i++) {
        if (!(info->cbuf_mask & (1u << i))) continue;
        uint32_t n = info->cbuf_size[i] / 16;
        if (c->cbuf_need[i] > n) n = c->cbuf_need[i];
        if ((info->cbuf_dynamic & (1u << i)) && n < 4096) n = 4096;
        if (!n) n = 1;
        if (maxwell_scalar_cbuf && (info->cbuf_dynamic & (1u << i))) {
            sp(&s, "layout(scalar, set = %d, binding = %d) uniform CB%d { uint cb%d_s[%u]; };\n", set, i, i, i, n * 4);
            char *nb = scalar_cbuf_rewrite(body.buf ? body.buf : "", i);
            free(body.buf);
            body.buf = nb, body.len = strlen(nb), body.cap = body.len + 1;
            continue;
        }
        sp(&s, "layout(std140, set = %d, binding = %d) uniform CB%d { uvec4 cb%d[%u]; };\n", set, i, i, i, n);
    }
    if (maxwell_scalar_cbuf && info->cbuf_dynamic)  // (cbras: row * 4 + word, an offset the compiler sees as 16-aligned)
        sp(&s, robust ? "#define cbrs(cb, off) cb[(off) >> 2]\n#define cbras(cb, row, k, comp) cb[(((row) + (k)) & 0x0fffffffu) * 4u + (comp)]\n"
                                   : "#define cbrs(cb, off) cb[min((off) >> 2, uint(cb.length() - 1))]\n"
                                     "#define cbras(cb, row, k, comp) cb[min(((row) + (k)) & 0x0fffffffu, uint(cb.length() / 4 - 1)) * 4u + (comp)]\n");
    if (info->uses_gmem && !maxwell_gmem_chunk_shift) {
        sp(&s, "layout(std430, set = %d, binding = %d) %sbuffer GMem { uint w[]; } gmem_;\n", set, GMEM_BUFFER_INDEX,
           c->uses_gmem_store ? "" : "readonly ");
        sp(&s, "#define GMR(i) gmem_.w[i]\n");
        if (c->uses_gmem_store)
            sp(&s, "#define GMW(i, v) gmem_.w[i] = (v)\n#define GMAND(i, m) atomicAnd(gmem_.w[i], m)\n"
                   "#define GMOR(i, m) atomicOr(gmem_.w[i], m)\n");
        if (c->uses_red) sp(&s, "#define GMCAS(i, a, b) atomicCompSwap(gmem_.w[i], a, b)\n");
    } else if (info->uses_gmem && !maxwell_gmem_lazy) {  // the heap in windows, one binding each, plain branches (shader.h)
        int n = maxwell_gmem_chunks(), ws = (int)maxwell_gmem_chunk_shift - 2, cs = GMEM_CHUNK_SET(stage);
        for (int k = 0; k < n; k++)
            sp(&s, "layout(std430, set = %d, binding = %d) %sbuffer GMem%d { uint w[]; } gmem%d_;\n", cs, GMEM_CHUNK_BINDING + k,
               c->uses_gmem_store ? "" : "readonly ", k, k);
        sp(&s, "uint GMR_(uint i) { uint c = i >> %du, o = i & 0x%xu;\n", ws, (1u << ws) - 1);
        for (int k = 0; k < n; k++) sp(&s, "  if (c == %du) return gmem%d_.w[o];\n", k, k);
        sp(&s, "  return 0u; }\n#define GMR(i) GMR_(i)\n");
        if (c->uses_gmem_store) {
            static const char *const ops[3][2] = {{"GMW_", "gmem%d_.w[o] = v"}, {"GMAND_", "atomicAnd(gmem%d_.w[o], v)"},
                                                  {"GMOR_", "atomicOr(gmem%d_.w[o], v)"}};
            for (int op = 0; op < 3; op++) {
                sp(&s, "void %s(uint i, uint v) { uint c = i >> %du, o = i & 0x%xu;\n", ops[op][0], ws, (1u << ws) - 1);
                for (int k = 0; k < n; k++) {
                    char body[64];
                    snprintf(body, sizeof body, ops[op][1], k);
                    sp(&s, "  if (c == %du) { %s; return; }\n", k, body);
                }
                sp(&s, "}\n");
            }
            sp(&s, "#define GMW(i, v) GMW_(i, v)\n#define GMAND(i, m) GMAND_(i, m)\n#define GMOR(i, m) GMOR_(i, m)\n");
            if (c->uses_red) {
                sp(&s, "uint GMCAS_(uint i, uint a, uint b) { uint c = i >> %du, o = i & 0x%xu;\n", ws, (1u << ws) - 1);
                for (int k = 0; k < n; k++) sp(&s, "  if (c == %du) return atomicCompSwap(gmem%d_.w[o], a, b);\n", k, k);
                sp(&s, "  return 0u; }\n#define GMCAS(i, a, b) GMCAS_(i, a, b)\n");
            }
        }
    } else if (info->uses_gmem) {  // chunks imported lazily: a table of device addresses, buffer references (shader.h)
        int ws = (int)maxwell_gmem_chunk_shift - 2, cs = GMEM_CHUNK_SET(stage);
        bool direct = stage != STAGE_COMPUTE;  // (compute: its storage buffers are in gfast_ already)
        sp(&s, "layout(std140, set = %d, binding = %d) uniform GChunks { uvec4 gchunk[%d]; };\n", cs, GMEM_CHUNK_BINDING, GMEM_CHUNKS_MAX);
        sp(&s, "layout(buffer_reference, std430, buffer_reference_align = 4) %sbuffer GW { uint w[]; };\n", c->uses_gmem_store ? "" : "readonly ");
        // Misses are counted with atomics on GMiss: a side effect, which in a fragment shader turns early depth / stencil
        // tests off - every overdrawn fragment ran in full (UHD 770 main pass 13.3 ms vs 8.4). So a fragment shader
        // whose own result can't depend on late tests (no discard or depth output, no stores) declares
        // early tests (glsl_early_tests) and keeps its counter; one that can't (discard / depth, no stores)
        // counts nothing - it would cost those tests - and relies on the runtime bringing its chunks in before the draw:
        // the bound storage buffers' chunks (gmem_chunk_bind), pointers in uniform buffers, the chunks other shaders
        // missed (cache/gmem_chunks.txt). LARECOMP_VK_GMEM_FS_MISS=1 (maxwell_gmem_fs_miss): every shader counts, no
        // early tests (as before; to find a chunk only such a shader reaches).
        bool count = stage != STAGE_FRAGMENT || maxwell_gmem_fs_miss || c->uses_gmem_store || glsl_early_tests(c);
        if (count) {
            sp(&s, "layout(std430, set = %d, binding = %d) buffer GMiss { uint n[%d]; uint w[%d]; } gmiss_;\n", cs, GMEM_MISS_BINDING,
               GMEM_CHUNKS_MAX, GMEM_CHUNKS_MAX);
            sp(&s, "void gmiss(uint c, uint i) { atomicAdd(gmiss_.n[c], 1u); atomicMin(gmiss_.w[c], i); }\n");
        } else {
            sp(&s, "void gmiss(uint c, uint i) { }\n");
        }
        // (gchunk[c]: x, y = device address, z = words; the first word's chunk holds the whole aligned access)
        sp(&s, "#define GMC(i) ((i) >> %du)\n#define GMO(i) ((i) & 0x%xu)\n", ws, (1u << ws) - 1);
        if (direct) {  // (gd[k]: x = the chunk's first word, y = its words (0: unused), z, w = its device address; GDir<k>: it)
            sp(&s, "layout(push_constant) uniform GDirect { uvec4 gd[%d]; };\n", GMEM_DIRECT_MAX);
            for (int k = 0; k < GMEM_DIRECT_MAX; k++)
                sp(&s, "layout(std430, set = %d, binding = %d) %sbuffer GDir%d { uint w[]; } gdir%d_;\n", cs, GMEM_DIRECT_BINDING + k,
                   c->uses_gmem_store ? "" : "readonly ", k, k);
        }
        // Each access: the draw's storage buffer chunks first (gd, uniform: no table read, no miss path), then the table.
        static const struct { const char *head, *act, *miss; int last; bool store; } fn[] = {
            {"uint GMR1(uint i)", "return p.w[o];", "return 0u;", 0, false},
            {"uvec2 GMR2(uint i)", "return uvec2(p.w[o], p.w[o + 1u]);", "return uvec2(0u);", 1, false},
            {"uvec4 GMR4(uint i)", "return uvec4(p.w[o], p.w[o + 1u], p.w[o + 2u], p.w[o + 3u]);", "return uvec4(0u);", 3, false},
            {"void GMW1(uint i, uint v)", "p.w[o] = v; return;", "", 0, true},
            {"void GMW2(uint i, uvec2 v)", "p.w[o] = v.x; p.w[o + 1u] = v.y; return;", "", 1, true},
            {"void GMW4(uint i, uvec4 v)", "p.w[o] = v.x; p.w[o + 1u] = v.y; p.w[o + 2u] = v.z; p.w[o + 3u] = v.w; return;", "", 3, true},
            {"void GMAND1(uint i, uint v)", "atomicAnd(p.w[o], v); return;", "", 0, true},
            {"void GMOR1(uint i, uint v)", "atomicOr(p.w[o], v); return;", "", 0, true},
            {"uint GMCAS1(uint i, uint a, uint b)", "return atomicCompSwap(p.w[o], a, b);", "return 0u;", 0, true},
        };
        for (int f = 0; f < (int)(sizeof fn / sizeof fn[0]); f++) {
            if (fn[f].store && !c->uses_gmem_store) continue;
            if (!c->uses_red && !strncmp(fn[f].head, "uint GMCAS1", 11)) continue;
            sp(&s, "%s { uint o;", fn[f].head);
            if (direct)  // (whole chunks: an aligned access in one never leaves it; the storage buffer, not the address)
                for (int k = 0; k < GMEM_DIRECT_MAX; k++) {
                    char act[192], *d = act;
                    for (const char *a = fn[f].act; *a && d < act + sizeof act - 8; a++) {
                        if (a[0] == 'p' && a[1] == '.') d += snprintf(d, 8, "gdir%d_", k);
                        else *d++ = *a;
                    }
                    *d = 0;
                    sp(&s, " o = i - gd[%d].x; if (o < gd[%d].y) { %s }", k, k, act);
                }
            char lim[16];
            if (fn[f].last) snprintf(lim, sizeof lim, "o + %du", fn[f].last); else snprintf(lim, sizeof lim, "o");
            sp(&s, " uint c = GMC(i); o = GMO(i); uvec4 e = gchunk[c]; if (%s < e.z) { GW p = GW((uint64_t(e.y) << 32) | uint64_t(e.x)); %s } "
                       "gmiss(c, i); %s }\n", lim, fn[f].act, fn[f].miss);
        }
        sp(&s, "#define GMR(i) GMR1(i)\n");
        if (c->uses_gmem_store) sp(&s, "#define GMW(i, v) GMW1(i, v)\n#define GMAND(i, m) GMAND1(i, m)\n#define GMOR(i, m) GMOR1(i, m)\n");
        if (c->uses_red) sp(&s, "#define GMCAS(i, a, b) GMCAS1(i, a, b)\n");
    }
    if (info->uses_gmem_readonly) {
        sp(&s, "layout(std140, set = %d, binding = %d) uniform GMap { uvec4 gmap[%d]; };\n",
           set, GRO_MAP_BUFFER_INDEX, GMAP_MAX + 1);
        sp(&s, "uint gload(uint g) { uint a = g & ~3u; uint n = min(gmap[%d].x, %uu); "
               "for (uint i = 0u; i < n; ++i) { uint o = a - gmap[i].x; "
               "if (a >= gmap[i].x && gmap[i].y >= 4u && o <= gmap[i].y - 4u) "
               "return gmem_.w[gmap[i].z + (o >> 2)]; } return 0u; }\n", GMAP_MAX, GMAP_MAX);
    }
    if (info->uses_gmem_map) {  // (compute) the dispatch's storage buffers, device-local: gmap[i] = heap offset, size, word
        sp(&s, "layout(std140, set = %d, binding = %d) uniform GMap { uvec4 gmap[%d]; };\n", set, GMAP_BUFFER_INDEX, GMAP_MAX + 1);
        sp(&s, "layout(std430, set = %d, binding = %d) %sbuffer GFast { uint w[]; } gfast_;\n", set, GFAST_BUFFER_INDEX,
           c->uses_gmem_store ? "" : "readonly ");
        sp(&s, "uint gmap_index(uint g, uint span, uvec4 e) { uint o = g - e.x; "
               "if (span == 0u || g < e.x || e.y < span || o > e.y - span) return 0xFFFFFFFFu; "
               "uint n = uint(gfast_.w.length()), w = o >> 2u, need = (span + 3u) >> 2u; "
               "if (e.z >= n || w > n - e.z || need > n - e.z - w) return 0xFFFFFFFFu; "
               "return e.z + w; }\n");
        if (maxwell_gfind_cache)  // the ranges are disjoint (exec.c mirror_table): the last range found serves the next load in it
            sp(&s, "uint gc_lo = 0u, gc_sz = 0u, gc_w = 0u;\n"
                   "uint gfind(uint g, uint span) { uint w = gmap_index(g, span, uvec4(gc_lo, gc_sz, gc_w, 0u)); "
                   "if (w != 0xFFFFFFFFu) return w; uint n = min(gmap[%d].x, %uu); "
                   "for (uint i = 0u; i < n; i++) { w = gmap_index(g, span, gmap[i]); if (w != 0xFFFFFFFFu) { gc_lo = gmap[i].x; "
                   "gc_sz = gmap[i].y; gc_w = gmap[i].z; return w; } } return 0xFFFFFFFFu; }\n", GMAP_MAX, GMAP_MAX);
        else
            sp(&s, "uint gfind(uint g, uint span) { uint n = min(gmap[%d].x, %uu); for (uint i = 0u; i < n; i++) { "
                   "uint w = gmap_index(g, span, gmap[i]); if (w != 0xFFFFFFFFu) return w; } return 0xFFFFFFFFu; }\n", GMAP_MAX, GMAP_MAX);
    }
    if (c->uses_rscale && stage != STAGE_VERTEX) {
        sp(&s, "layout(std140, set = %d, binding = 18) uniform RScale { vec4 rscale[%d]; };\n", set,
           key && key->flip_x ? 66 : 65);  // ([65]: mirror mode's render target width)
        info->uses_rscale = true;
    }
    bool tint = stage == STAGE_FRAGMENT && !dbg && (c->omap_target & 0x7) == 0x7 && (!key || key->rt_kind[0] == 0);
    if (tint) {
        sp(&s, "layout(std140, set = %d, binding = 19) uniform Tint { vec4 ltint[4]; };\n", set);
        info->uses_tint = true;
    }
    for (int i = 0; i < info->ntex; i++) {
        ShaderTexture *t = &info->tex[i];
        if (t->is_image) {
            const char *dim = t->type == TEXTYPE_1D ? "1D" : t->type == TEXTYPE_1D_ARRAY ? "1DArray"
                            : t->type == TEXTYPE_2D ? "2D" : t->type == TEXTYPE_2D_ARRAY ? "2DArray" : "3D";
            sp(&s, "layout(set = %d, binding = %d) coherent writeonly uniform image%s t%d;\n", set, 32 + i, dim, i);
            continue;
        }
        sp(&s, "layout(set = %d, binding = %d) uniform %s t%d;\n", set, 32 + i, glsl_sampler_type(t), i);
        // size of level l as (width, height, depth or layers)
        switch (t->type) {
        case TEXTYPE_1D: sp(&s, "ivec3 tsize%d(int l) { return ivec3(textureSize(t%d, l), 1, 1); }\n", i, i); break;
        case TEXTYPE_1D_ARRAY: sp(&s, "ivec3 tsize%d(int l) { ivec2 s = textureSize(t%d, l); return ivec3(s.x, 1, s.y); }\n", i, i); break;
        case TEXTYPE_3D: case TEXTYPE_2D_ARRAY: case TEXTYPE_CUBE_ARRAY:
            sp(&s, "ivec3 tsize%d(int l) { return textureSize(t%d, l); }\n", i, i); break;
        default: sp(&s, "ivec3 tsize%d(int l) { return ivec3(textureSize(t%d, l), 1); }\n", i, i);
        }
    }
    if (stage == STAGE_VERTEX) {
        if (info->clip_mask) {
            unsigned count = 0;
            for (unsigned i = 0; i < 8; i++) if (info->clip_mask & (1u << i)) count = i + 1;
            sp(&s, "out float gl_ClipDistance[%u];\n", count);
        }
        for (int i = 0; i < 32; i++) {
            if (!(c->in_attr_mask & (1u << i))) continue;
            int kind = key ? key->attr_kind[i] : 0;
            sp(&s, "layout(location = %d) in %s a%d;\n", i, kind == 1 ? "ivec4" : kind == 2 ? "uvec4" : "vec4", i);
        }
        // depth pre-pass + EQUAL test needs identical positions from different shaders: invariant, and precise so no
        // compiler fuses or reorders the math feeding it (MSL gets this from preserveInvariance; without it foliage
        // drawn twice flickered black). Phones too: without precise, the sea's reflection pass (its own shader
        // compares its depth with an earlier pass's) broke into blocks; with the leaner shaders precise costs ~0.3 ms
        // (it was 3 ms on the tall grass before). LARECOMP_VK_PRECISE_POS=0/1 overrides.
        static int prec = -1;
        if (prec < 0) prec = getenv("LARECOMP_VK_PRECISE_POS") ? atoi(getenv("LARECOMP_VK_PRECISE_POS")) != 0 : maxwell_precise_pos;
        sp(&s, prec ? "invariant gl_Position;\nprecise gl_Position;\n" : "invariant gl_Position;\n");
        for (int i = 0; i < 32; i++)
            if (c->out_attr_mask & (1u << i)) sp(&s, "layout(location = %d) out vec4 v%d;\n", i, i);
    } else if (stage == STAGE_FRAGMENT) {
        for (int i = 0; i < 32; i++) {
            if (!(c->in_attr_mask & (1u << i))) continue;
            const char *interp = (c->flat_mask & (1u << i)) ? "flat " : (c->noperspective_mask & (1u << i)) ? "noperspective " : "";
            sp(&s, "layout(location = %d) %sin vec4 v%d;\n", i, interp, i);
        }
        for (int rt = 0; rt < 8; rt++) {
            if (!((c->omap_target >> (rt * 4)) & 0xF)) continue;
            int kind = key ? key->rt_kind[rt] : 0;
            sp(&s, "layout(location = %d) out %s c%d;\n", rt, kind == 1 ? "ivec4" : kind == 2 ? "uvec4" : "vec4", rt);
        }
        if (c->omap_depth) info->writes_depth = true;
        if (glsl_early_tests(c)) sp(&s, "layout(early_fragment_tests) in;\n");
    } else {
        sp(&s, "layout(local_size_x_id = 0, local_size_y_id = 1, local_size_z_id = 2) in;\n");
    }
    sp(&s, "void main() {\n");
    if (stage == STAGE_VERTEX) {
        sp(&s, "  gl_Position = vec4(0, 0, 0, 1);\n");
        if (info->clip_mask) {
            unsigned count = 0;
            for (unsigned i = 0; i < 8; i++) if (info->clip_mask & (1u << i)) count = i + 1;
            // Holes and conditionally unwritten planes stay on the neutral
            // boundary. Never expose uninitialized clipping output values.
            for (unsigned i = 0; i < count; i++) sp(&s, "  gl_ClipDistance[%u] = 0.0f;\n", i);
        }
        for (int i = 0; i < 32; i++)
            if (c->out_attr_mask & (1u << i)) sp(&s, "  v%d = vec4(0, 0, 0, 1);\n", i);
        if (info->writes_point_size) sp(&s, "  gl_PointSize = 0.0f;\n");
    }
    for (int i = 0; i < 256; i++)
        if (c->reg_used[i]) { info->register_count++; sp(&s, "  uint r%d = 0u;\n", i); }
    for (int i = 0; i < 256; i++)
        if (c->huse[i]) { info->half_register_count++; sp(&s, "  f16vec2 h%d = f16vec2(0.0hf);\n", i); }
    for (int i = 0; i < 256; i++)
        if (c->hcuse[i]) { info->half_conversion_count++; sp(&s, "  f16vec2 hc%d = f16vec2(0.0hf);\n", i); }
    for (int i = 0; i < 7; i++)
        if (c->pred_used[i]) sp(&s, "  bool p%d = false;\n", i);
    if (c->uses_cc) sp(&s, "  uint cc = 0u; bool ccz = false, ccs = false;\n");
    info->local_words = c->uses_local ? LMEM_WORDS : 0;
    if (c->uses_local && c->local_sparse) {
        for (unsigned i = 0; i < LMEM_WORDS; i++) if (c->local_written[i]) sp(&s, "  uint lm%u = 0u;\n", i);
    } else if (c->uses_local) sp(&s, "  uint lmem[%d]; for (int li_ = 0; li_ < %d; li_++) lmem[li_] = 0u;\n", LMEM_WORDS, LMEM_WORDS);
    if (dbg) sp(&s, maxwell_dbg_pc == 0xffffffffu ? "  float dbgv = 1.0f;\n" : "  float dbgv = -1.0f;\n");
    if (any_branch) {
        if (c->static_stacks) sp(&s, "  uint pc = 8u;\n");  // (the stacks resolved: no arrays)
        else sp(&s, "  uint pc = 8u; uint ssy[16]; uint ssy_n = 0u; uint pbk[16]; uint pbk_n = 0u; uint pcnt[16]; uint pcnt_n = 0u;\n");
        sp(&s, "  bool exit_now = false;\n");
        sp(&s, "  for (uint guard = 0u; guard < %uu; guard++) {\n  switch (pc) {\n", stage == STAGE_COMPUTE ? 100000u : 16384u);
        sp(&s, "%s", body.buf ? body.buf : "");
        sp(&s, "    exit_now = true; break;\n  default: exit_now = true; break;\n  }\n  if (exit_now) break;\n  }\n");
    } else {
        sp(&s, "  do {\n%s  } while (false);\n", body.buf ? body.buf : "");
    }
    free(body.buf);
    if (stage == STAGE_VERTEX) {
        if (key && key->flip_y) sp(&s, "  gl_Position.y = -gl_Position.y;\n");
        if (key && key->flip_x) sp(&s, "  gl_Position.x = -gl_Position.x;\n");
        if (key && key->depth_minus_one) sp(&s, "  gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5f;\n");
    } else if (stage == STAGE_FRAGMENT) {
        int r = 0;
        for (int rt = 0; rt < 8; rt++) {
            uint32_t m = (c->omap_target >> (rt * 4)) & 0xF;
            if (!m) continue;
            int kind = key ? key->rt_kind[rt] : 0;
            const char *cv = kind == 1 ? "int" : kind == 2 ? "uint" : "";
            for (int comp = 0; comp < 4; comp++) {
                if (!(m & (1u << comp))) continue;
                if (dbg) {
                    if (comp == 3) sp(&s, "  c%d[%d] = %s(1.0f);\n", rt, comp, cv);
                    else sp(&s, "  c%d[%d] = %s(dbgv);\n", rt, comp, cv);
                } else if (r < 256 && c->reg_used[r]) {
                    if (kind == 0) sp(&s, "  c%d[%d] = as_f(r%d);\n", rt, comp, r);
                    else sp(&s, "  c%d[%d] = %s(r%d);\n", rt, comp, cv, r);
                } else {
                    sp(&s, "  c%d[%d] = 0;\n", rt, comp);
                }
                r++;
            }
        }
        if (tint)
            sp(&s, "  if (ltint[0].w > 0.0f) { vec3 g = c0.rgb; float l = dot(g, vec3(0.299f, 0.587f, 0.114f));\n"
                   "    c0.rgb = mix(g, ltint[0].rgb * pow(l, ltint[1].w) + ltint[1].rgb * l, ltint[0].w); }\n");
        if (tint) emit_recolour(&s, "c0", "vec3", "vec4");
        if (c->omap_depth) {
            if (c->reg_used[r + 1]) sp(&s, "  gl_FragDepth = as_f(r%d);\n", r + 1);
            else sp(&s, "  gl_FragDepth = gl_FragCoord.z;\n");
        }
    }
    sp(&s, "}\n");
    info->msl = s.buf;
    info->attr_mask = c->in_attr_mask;
    info->out_mask = stage == STAGE_FRAGMENT ? c->omap_target : c->out_attr_mask;
    free(c->body.buf);
    free(c);
    return info;
}

ShaderInfo *maxwell_translate_sized(const uint8_t *blob, size_t max_size, size_t constants_size, int stage, const ShaderKey *key) {
    ShaderInfo *info = calloc(1, sizeof *info);
    info->stage = stage;
    Ctx *c = calloc(1, sizeof *c);
    c->info = info;
    c->key = key;
    c->stage = stage;
    c->ipa_mode = -1;
    c->glsl = key && key->glsl;
    // Check before reading the optional SDK16.2 header or subtracting its length.
    size_t code_offset = stage == STAGE_COMPUTE ? 0 : 0x80;
    if (!blob || max_size < (stage == STAGE_COMPUTE ? 4u : 0x80u)) {
        fail(c, "truncated shader header"); free(c); return info;
    }
    if (stage == STAGE_COMPUTE) {
        uint32_t magic; memcpy(&magic, blob, sizeof magic);
        if (magic == 0x12345679u) code_offset = 0x100;
    }
    if (max_size < code_offset) {
        fail(c, "truncated compute header"); free(c); return info;
    }
    const uint8_t *sph = stage == STAGE_COMPUTE ? NULL : blob + 0x30;
    const uint8_t *code = blob + code_offset;
    if (stage == STAGE_VERTEX) c->clip_declared = sph[0x48];
    if (stage == STAGE_FRAGMENT) {
        c->omap_target = *(const uint32_t *)(sph + 0x48);
        c->omap_depth = (*(const uint32_t *)(sph + 0x4C) >> 1) & 1;
    }
    // find the end: the last EXIT followed by self-branch padding, or max_size
    size_t n = (max_size - (code - blob)) / 8;
    if (n > 65536) n = 65536;
    size_t end = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t w = ((const uint64_t *)code)[i];
        if (i % 4 == 0) continue;
        if ((w & 0xfff0000000000000ull) == 0xe240000000000000ull && sbits(w, 20, 24) == -8 && end) break;  // bra self
        if (w == 0 && i > 4) break;
        end = i + 1;
    }
    local_store_footprint(c, (const uint64_t *)code, end);
    // collect branch targets (labels)
    uint8_t *label = calloc(end + 4, 1);
    label[1] = 1;
    bool any_branch = false;
    for (size_t i = 0; i < end; i++) {
        if (i % 4 == 0) continue;
        uint64_t w = ((const uint64_t *)code)[i];
        uint32_t pc = (uint32_t)(i * 8);
        bool is_flow = (w & 0xfff0000000000020ull) == 0xe240000000000000ull ||
                       (w & 0xfff0000000000020ull) == 0xe290000000000000ull ||
                       (w & 0xfff0000000000020ull) == 0xe2a0000000000000ull ||
                       (w & 0xfff0000000000020ull) == 0xe2b0000000000000ull;
        if (is_flow) {
            int64_t t = norm_target((int64_t)pc + 8 + sbits(w, 20, 24));
            if (t >= 0 && (size_t)(t / 8) < end) label[t / 8] = 1;
            any_branch = true;
        }
        if ((w & 0xfff0000000000000ull) == 0xe250000000000000ull) {  // BRX: its table's targets, else any instruction
            any_branch = true;
            info->brx_count++;
            static int table_on = -1, window_on = -1;
            if (table_on < 0) table_on = getenv("LARECOMP_VK_BRX_TABLE") ? atoi(getenv("LARECOMP_VK_BRX_TABLE")) != 0 : maxwell_brx_table;
            // LARECOMP_VK_BRX_WINDOW=0: without a table, every instruction a label (as before). With it, only those
            // between the BRX and the join of the nearest PBK / SSY pushed before it that lies past it - where the
            // compiler puts a switch's cases (from the mamcjr Android port, caa833c: an SSAO pass on Adreno 20 ms ->
            // under 1). Labels everywhere else split the straight code after it into blocks the GPU's compiler can't
            // schedule, and reset what is known of the registers at each one.
            if (window_on < 0) window_on = getenv("LARECOMP_VK_BRX_WINDOW") ? atoi(getenv("LARECOMP_VK_BRX_WINDOW")) != 0 : 1;
            if (!(table_on && c->glsl && max_size < 0x10000 && brx_table(c, (const uint64_t *)code, i, end, (const uint32_t *)(blob + max_size), constants_size, label))) {
                size_t hi = 0;
                for (size_t k = i; window_on && k-- > 0;) {
                    if (k % 4 == 0) continue;
                    uint64_t wk = ((const uint64_t *)code)[k];
                    if ((wk & 0xfff0000000000020ull) != 0xe2a0000000000000ull && (wk & 0xfff0000000000020ull) != 0xe290000000000000ull)
                        continue;  // (PBK, SSY)
                    int64_t t = norm_target((int64_t)k * 8 + 8 + sbits(wk, 20, 24));
                    if (t > (int64_t)pc && (size_t)(t / 8) < end) { hi = (size_t)(t / 8); break; }
                }
                if (hi) for (size_t k = i + 1; k <= hi; k++) label[k] = 1;
                else memset(label, 1, end);
            }
        }
        if ((w & 0xfff8000000000000ull) == 0xf0f8000000000000ull || (w & 0xfff0000000000000ull) == 0xe340000000000000ull ||
            (w & 0xfff0000000000000ull) == 0xe350000000000000ull)
            any_branch = true;
    }
    // translate
    bool dbg = maxwell_dbg_code && blob == maxwell_dbg_code && stage == STAGE_FRAGMENT;
    static int annotate = -1;
    if (annotate < 0) annotate = getenv("LARECOMP_SAVE_MSL") != NULL;
    c->branchy = any_branch;
    // LARECOMP_VK_NESTED_FLOW=0: the flag-based structure_body for every shader with branches (comparisons)
    static int nested = -1;
    if (nested < 0) nested = getenv("LARECOMP_VK_NESTED_FLOW") ? atoi(getenv("LARECOMP_VK_NESTED_FLOW")) != 0 : 1;
    bool resolved = false;
    if (c->glsl && any_branch) {  // the stacks resolved: structure() needs them; maxwell_static_stacks: emit_insn, glsl_finish
        static int dyn = -1;
        if (dyn < 0) dyn = getenv("LARECOMP_DYN_STACKS") ? atoi(getenv("LARECOMP_DYN_STACKS")) != 0 : !maxwell_static_stacks;
        if (!dyn || nested) {
            c->flow_to = malloc(end * sizeof *c->flow_to + 4);
            resolved = c->flow_to && resolve_stacks(c, (const uint64_t *)code, end, c->flow_to);
            c->static_stacks = resolved && !dyn;
        }
    }
    size_t *off0 = calloc(end + 1, sizeof(size_t)), *off1 = calloc(end + 1, sizeof(size_t));  // (each instruction's text)
    // What is known of the registers at a block entry (reg_tz, hstate: emit_insn) is what holds on every path there:
    // the fall-through and each branch to it, recorded as they are emitted (all earlier in the code), unless a later
    // instruction may jump there too (a loop header: a BRA back, or a SYNC / BRK / CONT after a target pushed before
    // it; BRX: anywhere) - those start from nothing. The light loop of the Field's lit shaders computes a light's
    // cbuf base in one block and loads from it in the blocks its type branches to.
    const uint64_t *cw = (const uint64_t *)code;
    int *labno = malloc(sizeof(int) * (end + 4));
    size_t nlab = 0, last_pop = 0;
    bool brx = false;
    for (size_t i = 0; i < end; i++) labno[i] = label[i] ? (int)nlab++ : -1;
    for (size_t j = 1; j < end; j++) {
        int k = j % 4 ? flow_kind(cw[j]) : FLOW_NONE;
        if (k == FLOW_BRX && brx_of(c, j) < 0) brx = true;  // (a BRX with a table: a BRA to each of its targets)
        if (k == FLOW_SYNC || k == FLOW_BRK || k == FLOW_CONT) last_pop = j;
    }
    // A loop header keeps what holds on entry for the registers its loop never writes (the lit shaders' light loop:
    // the pixel's inputs, converted and unpacked once instead of per light): loop_end[t] is the last instruction a
    // back edge to t comes from (end: not known), and every destination field in [t, loop_end] counts as a write.
    uint8_t *back = calloc(end + 4, 1);
    size_t *loop_end = calloc(end + 4, sizeof *loop_end);
#define BACK(t, j) do { size_t t__ = (t), j__ = (j); back[t__] = 1; if (j__ > loop_end[t__]) loop_end[t__] = j__; } while (0)
    for (size_t j = 1; j < end && !brx; j++) {
        int k = j % 4 ? flow_kind(cw[j]) : FLOW_NONE;
        if (k == FLOW_BRX) {
            int b = brx_of(c, j);
            for (int q = 0; q < c->brx[b].n; q++) if (c->brx[b].t[q] / 8 <= j) BACK(c->brx[b].t[q] / 8, j);
            continue;
        }
        if (k != FLOW_BRA && k != FLOW_SSY && k != FLOW_PBK && k != FLOW_PCNT) continue;
        int64_t t = norm_target((int64_t)(j * 8) + 8 + sbits(cw[j], 20, 24)) / 8;
        if (t < 0 || (size_t)t >= end) continue;
        if (k == FLOW_BRA) { if ((size_t)t <= j) BACK((size_t)t, j); }
        else if (last_pop > j && last_pop >= (size_t)t) BACK((size_t)t, end);
    }
#undef BACK
    if (brx) nlab = 0;
    uint8_t *ltz = malloc(nlab * 256 + 1), *lh = malloc(nlab * 256 + 1), *lhc = malloc(nlab * 256 + 1);
    memset(ltz, 32, nlab * 256), memset(lh, 2, nlab * 256), memset(lhc, 1, nlab * 256);  // (nothing seen yet: everything holds)
    uint32_t *pushed = malloc(sizeof(uint32_t) * (end + 1));
    size_t npushed = 0;
#define MEET(t) do { size_t t_ = (t); if (!brx && t_ < end && labno[t_] >= 0) for (int r_ = 0; r_ < 256; r_++) { \
        uint8_t *z_ = ltz + (size_t)labno[t_] * 256 + r_, *h_ = lh + (size_t)labno[t_] * 256 + r_; \
        if (c->reg_tz[r_] < *z_) *z_ = c->reg_tz[r_]; \
        if (c->hstate[r_] != 2) *h_ = 0; \
        if (c->hcstate[r_] != 1) lhc[(size_t)labno[t_] * 256 + r_] = 0; } } while (0)
    for (size_t i = 0; i < end && !c->fail; i++) {
        if (i % 4 == 0) continue;
        uint32_t pc = (uint32_t)(i * 8);
        if (label[i]) {  // a block entry
            memset(c->image_origin, 0, sizeof c->image_origin);
            if (brx) memset(c->reg_tz, 0, sizeof c->reg_tz), hreset_all(c);
            else {
                memcpy(c->reg_tz, ltz + (size_t)labno[i] * 256, 256), memcpy(c->hstate, lh + (size_t)labno[i] * 256, 256);
                memcpy(c->hcstate, lhc + (size_t)labno[i] * 256, 256);
                if (back[i])  // (a loop header: what the loop writes, by destination fields, is unknown)
                    for (size_t j = i; j <= loop_end[i] && j < end; j++) {
                        if (j % 4 == 0) continue;
                        int d0 = (int)bits(cw[j], 0, 8), d1 = (int)bits(cw[j], 28, 8);
                        for (int k = 0; k < 4; k++) {
                            if (d0 + k < 256) c->reg_tz[d0 + k] = 0, c->hstate[d0 + k] = 0, c->hcstate[d0 + k] = 0;
                            if (d1 + k < 256) c->reg_tz[d1 + k] = 0, c->hstate[d1 + k] = 0, c->hcstate[d1 + k] = 0;
                        }
                    }
            }
            c->xmad_lo_pc = 0;
        }
        if (any_branch && label[i]) sp(&c->body, "  case %uu:\n", pc);
        if (annotate) sp(&c->body, "    // pc %x: %016llx\n", pc, (unsigned long long)((const uint64_t *)code)[i]);
        off0[i] = c->body.len;
        emit_insn(c, ((const uint64_t *)code)[i], pc);
        if (shared_title_las()) light_tiles_fix(c, (const uint64_t *)code, end, pc);
        size_t nx = i + 1;
        if (nx % 4 == 0) nx++;
        int fk = flow_kind(cw[i]);
        if (fk != FLOW_NONE) memset(c->image_origin, 0, sizeof c->image_origin);
        bool leaves = fk != FLOW_NONE && fk != FLOW_SSY && fk != FLOW_PBK && fk != FLOW_PCNT && bits(cw[i], 16, 4) == PT;
        if (nx >= end || label[nx] || flow_kind(cw[nx]) != FLOW_NONE) hflush_all(c);  // the uints current before a block
                                                                                        // entry, a flow instruction (its text goes: structure_body) or the end
        {   // what this instruction's paths bring to the labels they reach
            int64_t t = norm_target((int64_t)pc + 8 + sbits(cw[i], 20, 24)) / 8;
            if (fk == FLOW_BRA && t >= 0) MEET((size_t)t);
            if (fk == FLOW_BRX && brx_of(c, i) >= 0)
                for (int q = 0; q < c->brx[brx_of(c, i)].n; q++) MEET(c->brx[brx_of(c, i)].t[q] / 8);
            if ((fk == FLOW_SSY || fk == FLOW_PBK || fk == FLOW_PCNT) && t >= 0) pushed[npushed++] = (uint32_t)t;
            if (fk == FLOW_SYNC || fk == FLOW_BRK || fk == FLOW_CONT)
                for (size_t k = 0; k < npushed; k++) MEET(pushed[k]);
            if (nx < end && label[nx] && !leaves) MEET(nx);
        }
        off1[i] = c->body.len;
        if (dbg && pc == maxwell_dbg_pc) {
            int rr = maxwell_dbg_reg % 1000, half = maxwell_dbg_reg / 1000;  // +1000: low f16, +2000: high f16
            c->reg_used[rr] = 1;
            hmat(c, rr);
            if (half == 3) sp(&c->body, "    dbgv = float(r%d) * 0.125f;\n", rr);  // +3000: an integer, / 8
            else if (half) sp(&c->body, c->glsl ? "    dbgv = unpackHalf2x16(r%d)[%d];\n" : "    dbgv = float(as_type<half2>(r%d)[%d]);\n", rr, half - 1);
            else sp(&c->body, c->glsl ? "    dbgv = as_f(r%d);\n" : "    dbgv = as_type<float>(r%d);\n", rr);
        }
        info->num_insts++;
    }
    info->flow_kind = any_branch ? SHADER_FLOW_PC : SHADER_FLOW_STRAIGHT;
    info->stacks_resolved = resolved;
    info->static_stacks = c->static_stacks;
    info->branch_labels = (uint32_t)nlab;
    info->brx_tables = (uint32_t)c->nbrx;
    if (!c->fail && c->glsl && any_branch && !dbg && nested && !resolved && flow_log())
        fprintf(stderr, "[flow] structure not attempted: stacks unresolved (%zu words, word 8: %016llx)\n", end, (unsigned long long)((const uint64_t *)code)[8]);
    if (!c->fail && c->glsl && any_branch && !dbg && nested && resolved &&
        structure(c, (const uint64_t *)code, end, label, off0, off1)) {  // if / else / for from program order
        any_branch = false;
        info->structured = true;
        info->flow_kind = SHADER_FLOW_NESTED;
    } else if (!c->fail && c->glsl && any_branch && !dbg) {  // phone GPUs: structured flow instead of the pc state machine
        char *sb = structure_body(c, (const uint64_t *)code, end, label, off0, off1);
        if (sb) {
            free(c->body.buf);
            c->body.buf = sb, c->body.len = strlen(sb), c->body.cap = c->body.len + 1;
            any_branch = false;
            info->structured = true;
            info->flow_kind = SHADER_FLOW_FLAGS;
        }
    }
#undef MEET
    free(labno), free(back), free(loop_end), free(ltz), free(lh), free(lhc), free(pushed);
    free(c->flow_to), c->flow_to = NULL;
    free(off0), free(off1);
    free(label);
    // (MSL: more textures than sampler slots take a slot table, at a buffer index the vertex stage's streams use)
    if (!c->glsl && stage == STAGE_VERTEX && info->ntex > MSL_MAX_SAMPLERS)
        fail(c, "%d textures in a vertex shader (Metal: %d samplers)", info->ntex, MSL_MAX_SAMPLERS);
    if (c->fail) { free(c->body.buf); free(c); return info; }
    if (c->glsl) return glsl_finish(c, any_branch, dbg);

    // ---------------------------------------------------------- MSL source
    Str s = {0};
    sp(&s, "#include <metal_stdlib>\nusing namespace metal;\n");
    sp(&s, "static inline uint cbr(constant uint4 *cb, uint off) { return cb[off >> 4][(off >> 2) & 3]; }\n");
    sp(&s, "static inline uint brev32(uint v) { return reverse_bits(v); }\n");
    sp(&s, "static inline uint flo_sh(uint msb) { return msb == 0xFFFFFFFFu ? msb : 31u - msb; }\n");
    sp(&s, "static inline uint bfe_u(uint v, uint ctl) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u) return 0u; if (pos >= 32u) return 0u; return extract_bits(v, pos, min(len, 32u - pos)); }\n");
    sp(&s, "static inline uint bfe_s(uint v, uint ctl) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u) return 0u; if (pos >= 32u) return (int(v) < 0) ? 0xFFFFFFFFu : 0u; return uint(extract_bits(int(v), pos, min(len, 32u - pos))); }\n");
    sp(&s, "static inline uint bfi(uint ins, uint ctl, uint base) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u || pos >= 32u) return base; return insert_bits(base, ins, pos, min(len, 32u - pos)); }\n");
    sp(&s, "static inline float fswzadd(float a, float b, uint swz, uint lane) { uint m = (swz >> ((lane & 3u) * 2u)) & 3u; float ma = m == 1u ? -1.0f : (m == 3u ? 0.0f : 1.0f); float mb = m == 2u ? -1.0f : 1.0f; return a * ma + b * mb; }\n");
    sp(&s, "static inline uint2 rsc(uint2 c, float4 s) { return (s.x == 1.0f && s.y == 1.0f) ? c : uint2(floor((float2(c) + 0.5f) * s.xy)); }\n");
    sp(&s, "static inline float2 uvs(float2 uv, float4 s) { return (s.z == 1.0f && s.w == 1.0f) ? uv : min(uv, 1.0f) * s.zw; }\n");
    sp(&s, "static inline float2 hx(uint v, uint swz) { if (swz == 1u) return float2(as_type<float>(v)); float2 h = float2(as_type<half2>(v)); return swz == 2u ? h.xx : swz == 3u ? h.yy : h; }\n");
    // merges on half2 lanes rather than with integer masks: on an M1 (macOS 15.0.1) the Metal compiler miscompiled
    // (old & 0xFFFF0000u) | lo in Link's body shader - the merged low half read back as a constant 512.0 and Link
    // rendered white. Same bits either way.
    sp(&s, "static inline uint hpack(float2 r, uint merge, uint old) { if (merge == 1u) return as_type<uint>(r.x); half2 o = as_type<half2>(old), n = half2(r); if (merge == 2u) o.x = n.x; else if (merge == 3u) o.y = n.y; else o = n; return as_type<uint>(o); }\n");
    sp(&s, "static inline uint prmt(uint a, uint b, uint sel) { uint r = 0; for (uint i = 0; i < 4; i++) { uint s = (sel >> (i * 4)) & 7u; uint byte = s < 4 ? (a >> (s * 8)) & 0xFFu : (b >> ((s - 4) * 8)) & 0xFFu; r |= byte << (i * 8); } return r; }\n");

    // inputs/outputs
    if (stage == STAGE_VERTEX) {
        sp(&s, "struct VIn {\n");
        for (int i = 0; i < 32; i++) {
            if (!(c->in_attr_mask & (1u << i))) continue;
            int kind = key ? key->attr_kind[i] : 0;
            sp(&s, "  %s4 a%d [[attribute(%d)]];\n", kind == 1 ? "int" : kind == 2 ? "uint" : "float", i, i);
        }
        sp(&s, "};\n");
        sp(&s, "struct VOut {\n  float4 pos [[position, invariant]];\n");  // depth pre-pass + EQUAL test needs identical positions
        for (int i = 0; i < 32; i++)
            if (c->out_attr_mask & (1u << i)) sp(&s, "  float4 v%d [[user(locn%d)]];\n", i, i);
        if (info->writes_point_size) sp(&s, "  float psize [[point_size]];\n");
        sp(&s, "};\n");
    } else if (stage == STAGE_FRAGMENT) {
        sp(&s, "struct FIn {\n  float4 pos [[position]];\n");
        for (int i = 0; i < 32; i++) {
            if (!(c->in_attr_mask & (1u << i))) continue;
            const char *interp = (c->flat_mask & (1u << i)) ? " [[flat]]" : (c->noperspective_mask & (1u << i)) ? " [[center_no_perspective]]" : "";
            sp(&s, "  float4 v%d [[user(locn%d)]]%s;\n", i, i, interp);
        }
        sp(&s, "};\n");
        sp(&s, "struct FOut {\n");
        for (int rt = 0; rt < 8; rt++) {
            if (!((c->omap_target >> (rt * 4)) & 0xF)) continue;
            int kind = key ? key->rt_kind[rt] : 0;
            sp(&s, "  %s4 c%d [[color(%d)]];\n", kind == 1 ? "int" : kind == 2 ? "uint" : "float", rt, rt);
        }
        if (c->omap_depth) { sp(&s, "  float depth [[depth(any)]];\n"); info->writes_depth = true; }
        sp(&s, "};\n");
    }

    // entry point
    Str args = {0};
    for (int i = 0; i < 18; i++)
        if (info->cbuf_mask & (1u << i)) sp(&args, ", constant uint4 *cb%d [[buffer(%d)]]", i, i);
    if (info->uses_gmem) {
        if (info->cbuf_mask & (1u << GMEM_BUFFER_INDEX)) {
            free(args.buf);
            args = (Str){0};  // (was a use after free + double free of args.buf)
            fail(c, "cbuf %d clashes with gmem", GMEM_BUFFER_INDEX);
        }
        sp(&args, ", device uchar *gmem [[buffer(%d)]]", GMEM_BUFFER_INDEX);
    }
    if (c->uses_rscale && stage != STAGE_VERTEX) {
        sp(&args, ", constant float4 *rscale [[buffer(18)]]");
        info->uses_rscale = true;
    }
    // Runtime highlight (e.g. golden unchecked pots): fragment shaders writing a float color target 0 get a
    // per-draw tint, float4 [0] = colour.rgb + strength, [1] = added glow.rgb (strength 0 = untouched; < 0: the
    // Cosmetics recolour, emit_recolour).
    bool tint = stage == STAGE_FRAGMENT && !dbg && (c->omap_target & 0x7) == 0x7 && (!key || key->rt_kind[0] == 0);
    if (tint) {
        sp(&args, ", constant float4 *ltint [[buffer(19)]]");
        info->uses_tint = true;
    }
    // Metal has 16 sampler slots a stage (textures: 31). Up to 16 textures, texture i samples with sampler i. With more
    // (issue #215: a cave floor's fragment shader with 17), the samplers come as one array of the 16 slots and a table
    // (buffer SMAP_BUFFER_INDEX) names each texture's slot: the renderer puts each distinct sampler state the draw uses
    // in a slot of its own, so every texture keeps its own filtering and wrapping (renderer.m sampler_slots).
    bool smap = info->ntex > MSL_MAX_SAMPLERS;  // (not in vertex shaders: refused above)
    if (smap) sp(&args, ", constant uint *smap [[buffer(%d)]], array<sampler, %d> ss [[sampler(0)]]", SMAP_BUFFER_INDEX,
                 MSL_MAX_SAMPLERS);
    for (int i = 0; i < info->ntex; i++) {
        ShaderTexture *t = &info->tex[i];
        const char *tn = tex_type_name(t->type);
        sp(&args, ", %s%s<float> t%d [[texture(%d)]]", t->shadow ? "depth" : "texture", tn, i, i);
        if (!smap) sp(&args, ", sampler s%d [[sampler(%d)]]", i, i);
    }
    if (stage == STAGE_VERTEX) {
        sp(&s, "vertex VOut main0(VIn in [[stage_in]], uint vid [[vertex_id]], uint iid [[instance_id]]%s%s) {\n",
           (maxwell_dbg_vs && blob == maxwell_dbg_vs) ? ", device float4 *dbgout [[buffer(29)]]" : "",
           args.buf ? args.buf : "");
        sp(&s, "  VOut out = {};\n  out.pos = float4(0, 0, 0, 1);\n");
        for (int i = 0; i < 32; i++)
            if (c->out_attr_mask & (1u << i)) sp(&s, "  out.v%d = float4(0, 0, 0, 1);\n", i);
    } else if (stage == STAGE_FRAGMENT) {
        sp(&s, "fragment FOut main0(FIn in [[stage_in]], bool front_facing [[front_facing]]%s%s) {\n",
           c->uses_lane ? ", uint lane [[thread_index_in_simdgroup]]" : "", args.buf ? args.buf : "");
    } else {
        sp(&s, "kernel void main0(uint3 tid [[thread_position_in_threadgroup]], uint3 ctaid [[threadgroup_position_in_grid]]%s) {\n",
           args.buf ? args.buf : "");
    }
    free(args.buf);
    if (smap)
        for (int i = 0; i < info->ntex; i++) sp(&s, "  sampler s%d = ss[smap[%d]];\n", i, i);
    for (int i = 0; i < 256; i++)
        if (c->reg_used[i]) sp(&s, "  uint r%d = 0;\n", i);
    for (int i = 0; i < 7; i++)
        if (c->pred_used[i]) sp(&s, "  bool p%d = false;\n", i);
    if (c->uses_cc) sp(&s, "  uint cc = 0; bool ccz = false, ccs = false;\n");
    if (c->uses_local) sp(&s, "  uint lmem[%d] = {};\n", LMEM_WORDS);
    if (dbg) sp(&s, maxwell_dbg_pc == 0xffffffffu ? "  float dbgv = 1.0f;\n" : "  float dbgv = -1.0f;\n");
    if (any_branch) {
        sp(&s, "  uint pc = 8u; uint ssy[16]; uint ssy_n = 0; uint pbk[16]; uint pbk_n = 0; uint pcnt[16]; uint pcnt_n = 0;\n");
        sp(&s, "  bool exit_now = false;\n");
        sp(&s, "  for (uint guard = 0; guard < 100000u; guard++) {\n  switch (pc) {\n");
        sp(&s, "%s", c->body.buf ? c->body.buf : "");
        sp(&s, "    exit_now = true; break;\n  default: exit_now = true; break;\n  }\n  if (exit_now) break;\n  }\n");
    } else {
        sp(&s, "  do {\n%s  } while (false);\n", c->body.buf ? c->body.buf : "");
    }
    if (stage == STAGE_VERTEX) {
        if (key && key->flip_y) sp(&s, "  out.pos.y = -out.pos.y;\n");
        if (key && key->flip_x) sp(&s, "  out.pos.x = -out.pos.x;\n");
        if (key && key->depth_minus_one) sp(&s, "  out.pos.z = (out.pos.z + out.pos.w) * 0.5f;\n");
        if (maxwell_dbg_vs && blob == maxwell_dbg_vs) sp(&s, "  if (iid < 64u && vid < 8u) { dbgout[iid * 8u + vid] = out.pos; dbgout[512u + iid * 8u + vid] = %s; }\n", (c->out_attr_mask & 2u) ? "out.v1" : "float4(0)");
        sp(&s, "  return out;\n}\n");
    } else if (stage == STAGE_FRAGMENT) {
        sp(&s, "  FOut o;\n");
        int r = 0;
        for (int rt = 0; rt < 8; rt++) {
            uint32_t m = (c->omap_target >> (rt * 4)) & 0xF;
            if (!m) continue;
            int kind = key ? key->rt_kind[rt] : 0;
            for (int comp = 0; comp < 4; comp++) {
                if (!(m & (1u << comp))) continue;
                if (dbg) {
                    sp(&s, comp == 3 ? "  o.c%d[%d] = 1.0f;\n" : "  o.c%d[%d] = dbgv;\n", rt, comp);
                } else if (r < 256 && c->reg_used[r]) {
                    if (kind == 0) sp(&s, "  o.c%d[%d] = as_type<float>(r%d);\n", rt, comp, r);
                    else sp(&s, "  o.c%d[%d] = r%d;\n", rt, comp, r);
                } else {
                    sp(&s, "  o.c%d[%d] = 0;\n", rt, comp);
                }
                r++;
            }
        }
        if (tint)
            sp(&s, "  if (ltint[0].w > 0.0f) { float3 g = o.c0.rgb; float l = dot(g, float3(0.299f, 0.587f, 0.114f));\n"
                   "    o.c0.rgb = mix(g, ltint[0].rgb * pow(l, ltint[1].w) + ltint[1].rgb * l, ltint[0].w); }\n");
        if (tint) emit_recolour(&s, "o.c0", "float3", "float4");
        if (c->omap_depth) {
            if (c->reg_used[r + 1]) sp(&s, "  o.depth = as_type<float>(r%d);\n", r + 1);
            else sp(&s, "  o.depth = in.pos.z;\n");
        }
        sp(&s, "  return o;\n}\n");
    } else {
        sp(&s, "  return;\n}\n");
    }
    info->msl = s.buf;
    info->attr_mask = c->in_attr_mask;
    info->out_mask = stage == STAGE_FRAGMENT ? c->omap_target : c->out_attr_mask;
    free(c->body.buf);
    free(c);
    return info;
}

// Callers without compiler-constant metadata retain the original translation path.
ShaderInfo *maxwell_translate(const uint8_t *blob, size_t max_size, int stage, const ShaderKey *key) {
    return maxwell_translate_sized(blob, max_size, SIZE_MAX, stage, key);
}

void maxwell_free(ShaderInfo *s) {
    if (!s) return;
    free(s->msl);
    free(s->spirv);
    free(s);
}

void maxwell_disassemble(const uint8_t *code, size_t max_size, int stage, void *file) {
    (void)code; (void)max_size; (void)stage; (void)file;
}
