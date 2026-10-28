/* Actual sampled_texture_handle and shader_texture_object with minimal runtime state. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct { uint64_t addr, size; } Range;
typedef struct {
    uint32_t handle_offset;
    uint8_t type;
    bool shadow, is_array, is_image, bindless, is_uint;
    bool legacy_stencil_compare;
    uint32_t bindless_cbuf, bindless_offset;
} ShaderTexture;
typedef struct NvnTexture { uint32_t tag; } NvnTexture;
typedef struct {
    uint8_t setmask, format, target, ds_mode;
    uint16_t base_level, num_levels, min_layer, num_layers;
    uint8_t swizzle[4];
} NvnTextureView;
typedef struct { NvnTexture *tex; NvnTextureView view; bool has_view; uint32_t dead_format; } NvnTexEntry;
typedef struct { uint64_t addr; int count; void *entries; } NvnDescPool;
static struct { uint64_t tex[6][32]; Range ubuf[6][18]; NvnDescPool *tpool; } st;
static unsigned checks, compatibility_calls, fatal_count;
static bool compatible = true;
static jmp_buf fatal_env;

#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define FATAL(...) do { fatal_count++; longjmp(fatal_env, 1); } while (0)
static bool vk_sampled_float_compatible(NvnTexture *t, const NvnTextureView *v, bool has_view) {
    compatibility_calls++;
    CHECK(t && v && has_view);
    return compatible;
}

#include "production_sampled_texture_helpers.inc"

static void reset(void) {
    memset(&st, 0, sizeof st);
    compatibility_calls = fatal_count = 0;
    compatible = true;
}

static void expect_failure(ShaderTexture texture, int stage) {
    uint64_t handle = UINT64_MAX;
    CHECK(!sampled_texture_handle(stage, &texture, &handle));
    CHECK(handle == 0);
}

static void stage_and_bound_handles(void) {
    reset();
    ShaderTexture t = {.handle_offset = 0x20};
    uint64_t null_handle = UINT64_MAX;
    CHECK(!sampled_texture_handle(0, NULL, &null_handle) && null_handle == 0);
    CHECK(!sampled_texture_handle(0, &t, NULL));
    for (int stage = 0; stage < 6; stage++) {
        uint64_t expected = UINT64_C(0x123456789abcdef0) + (uint64_t)stage;
        st.tex[stage][0] = expected;
        uint64_t handle = 0;
        CHECK(sampled_texture_handle(stage, &t, &handle));
        CHECK(handle == expected); /* Legacy slots preserve the full 64-bit NVN handle. */
    }
    expect_failure(t, -1);
    expect_failure(t, 6);
    t.is_image = true;
    expect_failure(t, 0);
    t.is_image = false;
    t.handle_offset = 0x20 + 8 * 31;
    st.tex[4][31] = UINT64_C(0xfedcba9876543210);
    uint64_t handle = 0;
    CHECK(sampled_texture_handle(4, &t, &handle) && handle == st.tex[4][31]);
    t.handle_offset = 0x24; /* Legacy upper 32-bit word alias of descriptor slot zero. */
    st.tex[4][0] = UINT64_C(0x1020304050607080);
    CHECK(sampled_texture_handle(4, &t, &handle) && handle == st.tex[4][0]);
    t.handle_offset = 0x20 + 8 * 32;
    expect_failure(t, 4);
    t.handle_offset = 0x21;
    expect_failure(t, 4);
    t.handle_offset = 0x1c;
    expect_failure(t, 4);
}

static void driver_table(void) {
    reset();
    ShaderTexture t = {.bindless = true, .bindless_cbuf = 2};
    for (unsigned slot = 0; slot < 32; slot++) {
        uint64_t expected = UINT64_C(0xfeed000000000000) | (UINT64_C(0x1000) + slot);
        st.tex[3][slot] = expected;
        t.bindless_offset = 0x20 + 8 * slot;
        uint64_t handle = 0;
        CHECK(sampled_texture_handle(3, &t, &handle));
        CHECK(handle == (uint32_t)expected); /* cbuf 2's table contains packed 32-bit words. */
    }
    uint64_t handle = UINT64_MAX;
    for (uint32_t bad = 0; bad < 0x20; bad++) {
        t.bindless_offset = bad;
        handle = UINT64_MAX;
        CHECK(!sampled_texture_handle(3, &t, &handle) && handle == 0);
    }
    t.bindless_offset = 0x21;
    expect_failure(t, 3);
    t.bindless_offset = 0x20 + 8 * 32;
    expect_failure(t, 3);
}

static void uniform_buffers(void) {
    reset();
    uint32_t words[18];
    for (uint32_t cbuf = 3; cbuf <= 20; cbuf++) {
        uint32_t index = cbuf - 3;
        words[index] = UINT32_C(0xabc00000) | cbuf;
        st.ubuf[5][index] = (Range){(uintptr_t)&words[index], sizeof words[index]};
        ShaderTexture t = {.bindless = true, .bindless_cbuf = cbuf};
        uint64_t handle = UINT64_MAX;
        CHECK(sampled_texture_handle(5, &t, &handle));
        CHECK(handle == words[index]);
        words[index] ^= UINT32_C(0x0000ffff); /* Read live bound memory on every lookup. */
        CHECK(sampled_texture_handle(5, &t, &handle) && handle == words[index]);
    }
    ShaderTexture t = {.bindless = true, .bindless_cbuf = 20, .bindless_offset = 4};
    uint32_t last = UINT32_C(0x76543210);
    uint8_t payload[8] = {0};
    memcpy(payload + 4, &last, 4);
    st.ubuf[0][17] = (Range){(uintptr_t)payload, sizeof payload};
    uint64_t handle = 0;
    CHECK(sampled_texture_handle(0, &t, &handle) && handle == last);
    t.bindless_offset = 5;
    expect_failure(t, 0); /* Fewer than four bytes remain. */
    t.bindless_cbuf = 21;
    expect_failure(t, 0);

    reset();
    t = (ShaderTexture){.bindless = true, .bindless_cbuf = 7, .bindless_offset = 99};
    st.ubuf[2][4] = (Range){0, 500};
    handle = UINT64_MAX;
    CHECK(sampled_texture_handle(2, &t, &handle) && handle == 0); /* Unbound UBO is zero regardless of offset. */

    uint8_t *map = mmap(NULL, (size_t)sysconf(_SC_PAGESIZE) * 2, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(map != MAP_FAILED);
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    CHECK(!mprotect(map + page, page, PROT_NONE));
    uint32_t guard_word = UINT32_C(0x1234fedc);
    memcpy(map + page - sizeof guard_word, &guard_word, sizeof guard_word);
    t = (ShaderTexture){.bindless = true, .bindless_cbuf = 3};
    st.ubuf[1][0] = (Range){(uintptr_t)(map + page - sizeof guard_word), sizeof guard_word};
    CHECK(sampled_texture_handle(1, &t, &handle) && handle == guard_word); /* Exactly four readable bytes. */
    CHECK(!munmap(map, page * 2));

    reset();
    t = (ShaderTexture){.bindless = true, .bindless_cbuf = 3, .bindless_offset = 8};
    st.ubuf[0][0] = (Range){UINT64_MAX - 4, 64};
    expect_failure(t, 0); /* Addition overflow is rejected before dereference. */
    t.bindless_offset = 4;
    st.ubuf[0][0] = (Range){(uint64_t)UINTPTR_MAX - 6, 64};
    expect_failure(t, 0); /* A span not representable by a host pointer is rejected. */
}

static void pool_resolution_and_feedback(void) {
    reset();
    NvnTexture textures[2] = {{10}, {20}};
    NvnTexEntry entries[2] = {{.tex = &textures[0], .view = {.setmask = 1}, .has_view = true},
                              {.tex = &textures[1], .view = {.setmask = 2}, .has_view = true}};
    NvnDescPool pool = {.count = 2, .entries = entries};
    st.tpool = &pool;
    ShaderTexture normal = {.handle_offset = 0x20};
    st.tex[0][0] = 0;
    CHECK(shader_texture_object(0, &normal) == NULL); /* Normal handle zero is unbound. */
    ShaderTexture bindless = {.bindless = true, .bindless_cbuf = 2, .bindless_offset = 0x20};
    st.tex[0][0] = 0;
    CHECK(shader_texture_object(0, &bindless) == &textures[0]); /* Bindless handle zero can name pool entry zero. */
    st.tex[0][0] = 1;
    CHECK(shader_texture_object(0, &bindless) == &textures[1]);
    st.tex[0][0] = 2;
    CHECK(shader_texture_object(0, &bindless) == NULL); /* Out-of-range pool index. */

    st.tex[1][0] = 1;
    ShaderTexture feedback_ref = {.bindless = true, .bindless_cbuf = 2, .bindless_offset = 0x20};
    NvnTexture *discovered = shader_texture_object(1, &feedback_ref); /* resolve_feedback call site */
    NvnTexture *bound = shader_texture_object(1, &feedback_ref);     /* descriptor/binding resolver call site */
    CHECK(discovered == bound && discovered == &textures[1]);

    compatible = false;
    st.tex[0][0] = 0;
    compatibility_calls = fatal_count = 0;
    if (!setjmp(fatal_env)) {
        (void)shader_texture_object(0, &bindless);
        CHECK(false);
    }
    CHECK(fatal_count == 1 && compatibility_calls == 1); /* Bindless float/depth view rejection. */
}

int main(void) {
    stage_and_bound_handles();
    driver_table();
    uniform_buffers();
    pool_resolution_and_feedback();
    printf("{\"state\":\"passed\",\"checks\":%u,\"guard_page\":true,\"feedback_resolver_match\":true}\n", checks);
}
