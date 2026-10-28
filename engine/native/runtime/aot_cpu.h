/*
 * Derived from LAS Recomp runtime/cpu.h and dispatch.c (MIT).
 * Copyright (c) 2026 Peiikai
 * Function-entry hook protocol derived from Let's Go Recomp (MIT).
 * Copyright (c) 2026 raulzito12345rrs
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
// Guest CPU state and helpers used by recompiled code.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct __attribute__((aligned(16))) Cpu {
    uint64_t x[32];          // x0-x30 (x[31] unused; sp is separate)
    uint64_t sp;             // 256
    uint64_t pc;             // 264: target for indirect dispatch
    uint64_t nzcv;           // 272: flags in bits 31..28
    uint64_t tpidr;          // 280
    uint64_t tpidrro;        // 288
    uint64_t excl_addr;      // 296
    uint64_t excl_val;       // 304
    void *thread;            // 312: runtime thread object
    uint64_t fpcr;           // 320: guest FPCR/FPSR (portable FP/SIMD lowering only; the asm path uses the host's)
    uint64_t fpsr;           // 328
    uint64_t excl_hi;        // 336: reserved high exclusive value, zero at initialization; 128-bit helpers pending
    uint8_t _pad[512 - 344];
    __uint128_t v[32];       // 512
    // rt_dispatch's direct-mapped cache in front of the function lookup. Per guest thread like a __thread array, but
    // reached through c: a dlopen'ed runtime (Android's libmain.so) pays a TLS resolver call on every __thread access.
    struct { uint64_t addr; void (*fn)(struct Cpu *); } dcache[4096];
} Cpu;

_Static_assert(offsetof(Cpu, sp) == 256, "sp");
_Static_assert(offsetof(Cpu, pc) == 264, "pc");
_Static_assert(offsetof(Cpu, nzcv) == 272, "nzcv");
_Static_assert(offsetof(Cpu, fpcr) == 320, "fpcr");
_Static_assert(offsetof(Cpu, fpsr) == 328, "fpsr");
_Static_assert(offsetof(Cpu, v) == 512, "v");

_Static_assert(sizeof(void *) == 8, "shared-aot-v1 requires LP64");
_Static_assert(sizeof(Cpu) == 66560, "Cpu size");
_Static_assert(_Alignof(Cpu) == 16, "Cpu alignment");
_Static_assert(offsetof(Cpu, x) == 0, "x");
_Static_assert(offsetof(Cpu, tpidr) == 280, "tpidr");
_Static_assert(offsetof(Cpu, tpidrro) == 288, "tpidrro");
_Static_assert(offsetof(Cpu, excl_addr) == 296, "excl_addr");
_Static_assert(offsetof(Cpu, excl_val) == 304, "excl_val");
_Static_assert(offsetof(Cpu, thread) == 312, "thread");
_Static_assert(offsetof(Cpu, excl_hi) == 336, "excl_hi");
_Static_assert(offsetof(Cpu, _pad) == 344, "padding");
_Static_assert(sizeof(((Cpu *)0)->v[0]) == 16, "vector stride");
_Static_assert(offsetof(Cpu, dcache) == 1024, "dcache");
_Static_assert(sizeof(((Cpu *)0)->dcache[0]) == 16, "cache stride");
_Static_assert(offsetof(Cpu, dcache[0].fn) == 1032, "cache function offset");
_Static_assert(sizeof(((Cpu *)0)->dcache) / sizeof(((Cpu *)0)->dcache[0]) == 4096, "cache count");

typedef void (*GuestFn)(Cpu *);

// ---- runtime services (runtime/*.c)
__attribute__((visibility("default"))) void rt_dispatch(Cpu *c);                 // call function at c->pc
// hook site (tools/patches.py HOOKS, runtime/hle/hooks.c): 0 = run the instruction, 1 = the hook did it all,
// 2.. = jump to the site's first.. target
__attribute__((visibility("default"))) int rt_hook(Cpu *c, uint64_t site);
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc);
void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) __attribute__((noreturn));
void rt_udf(Cpu *c, uint64_t pc) __attribute__((noreturn));
uint64_t rt_ticks(void);

#if defined(__aarch64__)
static inline uint64_t rt_get_fpcr(void) { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return v; }
static inline uint64_t rt_get_fpsr(void) { uint64_t v; __asm__ volatile("mrs %0, fpsr" : "=r"(v)); return v; }
static inline void rt_set_fpcr(uint64_t v) { __asm__ volatile("msr fpcr, %0" :: "r"(v)); }
static inline void rt_set_fpsr(uint64_t v) { __asm__ volatile("msr fpsr, %0" :: "r"(v)); }
#else
// Host FP state is not the guest's here: code lifted with --simd=c keeps FPCR/FPSR in Cpu (c->fpcr, zeroed
// with the Cpu), so the host-side reset in thread start-up has nothing to do.
static inline uint64_t rt_get_fpcr(void) { return 0; }
static inline uint64_t rt_get_fpsr(void) { return 0; }
static inline void rt_set_fpcr(uint64_t v) { (void)v; }
static inline void rt_set_fpsr(uint64_t v) { (void)v; }
#endif

// Guest pointer values are already actual addresses. A generated profile may supply
// AOT_GUEST_SHIFT for its own canonical literals; the runtime owns no title shift.
#ifdef AOT_GUEST_SHIFT
#define GUEST_SHIFT AOT_GUEST_SHIFT
#define GUEST(a) ((uint64_t)(a) - (uint64_t)AOT_GUEST_SHIFT)
#endif

typedef uint16_t __attribute__((aligned(1), may_alias)) u16u;
typedef uint32_t __attribute__((aligned(1), may_alias)) u32u;
typedef uint64_t __attribute__((aligned(1), may_alias)) u64u;
typedef uint8_t __attribute__((may_alias)) u8a;

// Each generated translation unit fixes these bits in its immutable producer profile.
// The common runtime is compiled once; it never chooses a title via this macro.
#define AOT_CAP_INSTRUCTION_HOOKS 1u
#define AOT_CAP_FUNCTION_HOOKS 2u
#define AOT_CAP_PROFILING 4u
#define AOT_CAP_ALL 7u
#ifndef AOT_PROFILE_CAPABILITIES
#define AOT_PROFILE_CAPABILITIES 0u
#endif
#if (AOT_PROFILE_CAPABILITIES & ~AOT_CAP_ALL)
#error "unsupported shared-aot-v1 profile capabilities"
#endif
#ifndef AOT_API
#define AOT_API __attribute__((visibility("default")))
#endif
AOT_API extern volatile int g_fnprof;
AOT_API void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0);
AOT_API extern volatile int g_fnhook;
AOT_API int rt_fnhook(Cpu *c, uint64_t addr);
#if (AOT_PROFILE_CAPABILITIES & AOT_CAP_PROFILING)
#define AOT_PROFILE_ENTER(a) do { if (__builtin_expect(g_fnprof, 0)) fnprof_hit(a, c->x[30], c->x[0]); } while (0)
#else
#define AOT_PROFILE_ENTER(a) ((void)0)
#endif
#if (AOT_PROFILE_CAPABILITIES & AOT_CAP_FUNCTION_HOOKS)
#define FN_ENTER(a) do { AOT_PROFILE_ENTER(a); if (__builtin_expect(g_fnhook, 0) && rt_fnhook(c, a)) return; } while (0)
#else
#define FN_ENTER(a) AOT_PROFILE_ENTER(a)
#endif
// Functions the play profile never saw run (tools/gen.py --hot): not optimized, so building the game code on a phone
// takes far less time; musttail still holds at optnone
#define COLD_FN __attribute__((optnone, noinline))
// Generated functions take their Cpu as `Cpu *CPU_RESTRICT c`: guest memory never overlaps the host's Cpu, so stores
// to guest memory don't make the compiler reload guest registers.
#define CPU_RESTRICT __restrict

#define LD8(a)  (*(const u8a *)(uintptr_t)(a))
#define LD16(a) (*(const u16u *)(uintptr_t)(a))
#define LD32(a) (*(const u32u *)(uintptr_t)(a))
#define LD64(a) (*(const u64u *)(uintptr_t)(a))
#define ST8(a, v)  (*(u8a *)(uintptr_t)(a) = (uint8_t)(v))
#define ST16(a, v) (*(u16u *)(uintptr_t)(a) = (uint16_t)(v))
#define ST32(a, v) (*(u32u *)(uintptr_t)(a) = (uint32_t)(v))
#define ST64(a, v) (*(u64u *)(uintptr_t)(a) = (uint64_t)(v))

// ---- exclusive monitor (emulated with compare-and-swap)
#define EXCL(N, T)                                                                         \
    static inline uint64_t ldx##N(Cpu *c, uint64_t a) {                                    \
        T v = __atomic_load_n((T *)(uintptr_t)a, __ATOMIC_ACQUIRE);                        \
        c->excl_addr = a;                                                                  \
        c->excl_val = v;                                                                   \
        return v;                                                                          \
    }                                                                                      \
    static inline uint32_t stx##N(Cpu *c, uint64_t a, uint64_t v) {                        \
        if (c->excl_addr != a) return 1;                                                   \
        c->excl_addr = ~0ull;                                                              \
        T expected = (T)c->excl_val;                                                       \
        return !__atomic_compare_exchange_n((T *)(uintptr_t)a, &expected, (T)v, 0,         \
                                            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);           \
    }                                                                                      \
    static inline uint64_t lda##N(uint64_t a) {                                            \
        return __atomic_load_n((T *)(uintptr_t)a, __ATOMIC_ACQUIRE);                       \
    }                                                                                      \
    static inline void stl##N(uint64_t a, uint64_t v) {                                    \
        __atomic_store_n((T *)(uintptr_t)a, (T)v, __ATOMIC_RELEASE);                       \
    }
EXCL(8, uint8_t)
EXCL(16, uint16_t)
EXCL(32, uint32_t)
EXCL(64, uint64_t)

// ---- flags (packed NZCV in bits 31..28)
#define FLAG_N(c) (((c)->nzcv >> 31) & 1)
#define FLAG_Z(c) (((c)->nzcv >> 30) & 1)
#define FLAG_C(c) (((c)->nzcv >> 29) & 1)
#define FLAG_V(c) (((c)->nzcv >> 28) & 1)
#define COND_EQ(c) (FLAG_Z(c))
#define COND_NE(c) (!FLAG_Z(c))
#define COND_CS(c) (FLAG_C(c))
#define COND_CC(c) (!FLAG_C(c))
#define COND_MI(c) (FLAG_N(c))
#define COND_PL(c) (!FLAG_N(c))
#define COND_VS(c) (FLAG_V(c))
#define COND_VC(c) (!FLAG_V(c))
#define COND_HI(c) (FLAG_C(c) && !FLAG_Z(c))
#define COND_LS(c) (!(FLAG_C(c) && !FLAG_Z(c)))
#define COND_GE(c) (FLAG_N(c) == FLAG_V(c))
#define COND_LT(c) (FLAG_N(c) != FLAG_V(c))
#define COND_GT(c) (!FLAG_Z(c) && FLAG_N(c) == FLAG_V(c))
#define COND_LE(c) (!(!FLAG_Z(c) && FLAG_N(c) == FLAG_V(c)))

#define MKFLAGS(n, z, cy, v) \
    (((uint64_t)(n) << 31) | ((uint64_t)(z) << 30) | ((uint64_t)(cy) << 29) | ((uint64_t)(v) << 28))

static inline uint64_t adds64(Cpu *c, uint64_t a, uint64_t b) {
    uint64_t r = a + b;
    c->nzcv = MKFLAGS(r >> 63, r == 0, r < a, ((a ^ r) & (b ^ r)) >> 63);
    return r;
}
static inline uint64_t subs64(Cpu *c, uint64_t a, uint64_t b) {
    uint64_t r = a - b;
    c->nzcv = MKFLAGS(r >> 63, r == 0, a >= b, ((a ^ b) & (a ^ r)) >> 63);
    return r;
}
static inline uint32_t adds32(Cpu *c, uint32_t a, uint32_t b) {
    uint32_t r = a + b;
    c->nzcv = MKFLAGS(r >> 31, r == 0, r < a, ((a ^ r) & (b ^ r)) >> 31);
    return r;
}
static inline uint32_t subs32(Cpu *c, uint32_t a, uint32_t b) {
    uint32_t r = a - b;
    c->nzcv = MKFLAGS(r >> 31, r == 0, a >= b, ((a ^ b) & (a ^ r)) >> 31);
    return r;
}
static inline uint64_t logic64(Cpu *c, uint64_t r) { c->nzcv = MKFLAGS(r >> 63, r == 0, 0, 0); return r; }
static inline uint32_t logic32(Cpu *c, uint32_t r) { c->nzcv = MKFLAGS(r >> 31, r == 0, 0, 0); return r; }

#define ADC_IMPL(N, S)                                                                     \
    static inline uint##N##_t adc##N(Cpu *c, uint##N##_t a, uint##N##_t b) {               \
        return a + b + (uint##N##_t)FLAG_C(c);                                             \
    }                                                                                      \
    static inline uint##N##_t sbc##N(Cpu *c, uint##N##_t a, uint##N##_t b) {               \
        return a - b - 1 + (uint##N##_t)FLAG_C(c);                                         \
    }                                                                                      \
    static inline uint##N##_t adcs##N(Cpu *c, uint##N##_t a, uint##N##_t b) {              \
        uint##N##_t cin = FLAG_C(c), r = a + b + cin;                                      \
        int carry = cin ? r <= a : r < a;                                                  \
        c->nzcv = MKFLAGS(r >> (N - 1), r == 0, carry, ((a ^ r) & (b ^ r)) >> (N - 1));    \
        return r;                                                                          \
    }                                                                                      \
    static inline uint##N##_t sbcs##N(Cpu *c, uint##N##_t a, uint##N##_t b) {              \
        return adcs##N(c, a, (uint##N##_t)~b);                                             \
    }
ADC_IMPL(64, )
ADC_IMPL(32, )

// ---- bit ops
#define ROR64(v, r) ((uint64_t)(v) >> ((r) & 63) | (uint64_t)(v) << ((64 - ((r) & 63)) & 63))
#define ROR32(v, r) ((uint32_t)(v) >> ((r) & 31) | (uint32_t)(v) << ((32 - ((r) & 31)) & 31))
static inline uint64_t clz64(uint64_t v) { return v ? __builtin_clzll(v) : 64; }
static inline uint32_t clz32(uint32_t v) { return v ? __builtin_clz(v) : 32; }
static inline uint64_t cls64(uint64_t v) { return clz64(v ^ (uint64_t)((int64_t)v >> 1)) - 1; }
static inline uint32_t cls32(uint32_t v) { return clz32(v ^ (uint32_t)((int32_t)v >> 1)) - 1; }
static inline uint64_t rbit64(uint64_t v) { return __builtin_bitreverse64(v); }
static inline uint32_t rbit32(uint32_t v) { return __builtin_bitreverse32(v); }
static inline uint32_t rev16_32(uint32_t v) { return ((v & 0x00ff00ffu) << 8) | ((v >> 8) & 0x00ff00ffu); }
static inline uint64_t rev16_64(uint64_t v) {
    return ((v & 0x00ff00ff00ff00ffull) << 8) | ((v >> 8) & 0x00ff00ff00ff00ffull);
}
static inline uint64_t rev32_64(uint64_t v) {
    return ((uint64_t)__builtin_bswap32((uint32_t)v)) | ((uint64_t)__builtin_bswap32((uint32_t)(v >> 32)) << 32);
}
static inline uint64_t udiv64(uint64_t a, uint64_t b) { return b ? a / b : 0; }
static inline uint32_t udiv32(uint32_t a, uint32_t b) { return b ? a / b : 0; }
static inline uint64_t sdiv64(uint64_t a, uint64_t b) {
    if (!b) return 0;
    if ((int64_t)a == INT64_MIN && (int64_t)b == -1) return a;
    return (uint64_t)((int64_t)a / (int64_t)b);
}
static inline uint32_t sdiv32(uint32_t a, uint32_t b) {
    if (!b) return 0;
    if ((int32_t)a == INT32_MIN && (int32_t)b == -1) return a;
    return (uint32_t)((int32_t)a / (int32_t)b);
}

// shared-aot-v1 initially binds ARM64 assembly SIMD. Portable SIMD lowering needs
// a separately versioned helper contract; synthetic host tests use no SIMD lowering.

#ifdef __cplusplus
}
#endif
