// Asset-free fixture for the live GPU-pass capture region; no Vulkan driver required.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
typedef int VkResult;
typedef uintptr_t VkQueryPool;
typedef uintptr_t VkCommandBuffer;
typedef struct { int sType, queryType; unsigned queryCount; } VkQueryPoolCreateInfo;
enum { VK_SUCCESS = 0, VK_NOT_READY = 1, VK_ERROR_DEVICE_LOST = -4,
       VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO = 11, VK_QUERY_TYPE_TIMESTAMP = 2,
       VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT = 4, VK_QUERY_RESULT_64_BIT = 1,
       VK_QUERY_RESULT_WAIT_BIT = 2 };
static struct { int dev; unsigned ts_bits; struct { struct {
    float timestampPeriod; bool timestampComputeAndGraphics;
} limits; } props; } g_vk;
static unsigned checks, creates, resets, writes, reads, frame_reads, clock_reads;
static uint64_t render_frame, game_frame, clock_ns = 1000000000, pending_seq = 7, submitted_seq;
static VkResult create_result, read_result;
static uint64_t timestamps[1024];
static char logs[131072];
static size_t log_size;
#define CHECK(x) do { ++checks; assert(x); } while (0)
static void test_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(logs + log_size, sizeof logs - log_size, fmt, ap);
    va_end(ap); assert(n >= 0 && (size_t)n < sizeof logs - log_size); log_size += (size_t)n;
}
#define LOG(...) test_log(__VA_ARGS__)
uint64_t renderer_present_count(void) { abort(); return game_frame; }
static uint64_t vk_frame_counter(void) { frame_reads++; return render_frame; }
static uint64_t SDL_GetTicksNS(void) { clock_reads++; return clock_ns; }
static uint64_t vk_pending_seq(void) { return pending_seq; }
static uint64_t rec_submitted(void) { return submitted_seq; }
static VkResult vkCreateQueryPool(int dev, const VkQueryPoolCreateInfo *info, void *alloc, VkQueryPool *out) {
    (void)dev; (void)alloc; CHECK(info->queryCount == 1024); creates++; *out = 1; return create_result;
}
static void rec_CmdResetQueryPool(VkCommandBuffer cb, VkQueryPool pool, unsigned first, unsigned count) {
    (void)cb; CHECK(pool == 1 && !first && count == 1024); resets++;
}
static void rec_CmdWriteTimestamp2(VkCommandBuffer cb, unsigned stage, VkQueryPool pool, unsigned query) {
    (void)cb; CHECK(stage == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT && pool == 1 && query == writes && query < 1024);
    timestamps[query] = 1000 + 1000000ull * query; writes++;
}
static VkResult vkGetQueryPoolResults(int dev, VkQueryPool pool, unsigned first, unsigned count,
                                    size_t size, void *data, uint64_t stride, unsigned flags) {
    (void)dev; CHECK(pool == 1 && !first && count <= writes);
    CHECK(flags == VK_QUERY_RESULT_64_BIT && !(flags & VK_QUERY_RESULT_WAIT_BIT));
    CHECK(size >= count * sizeof(uint64_t) && stride == sizeof(uint64_t)); reads++;
    if (read_result == VK_SUCCESS) memcpy(data, timestamps, count * sizeof(uint64_t));
    return read_result;
}
#include "pass_gpu_fragment.h"
// Mirrors the two production draw guards; the checker verifies those guards remain present.
static void draw_mark(unsigned vertices) {
    if (pg_state == 1 && pg_draws_on()) pg_mark(1, "draw fs %llx vs %llx", 0x123ull, 0x456ull);
    if (pg_state == 1 && pg_draws_on() && pg_n > 0) {
        pg_verts += vertices; pg_vc[pg_n - 1] = vertices;
    }
}
static bool has(const char *needle) { return strstr(logs, needle) != NULL; }
static void start(void) { render_frame = 100; pg_mark(1, "pass 1280x720, 1 color (fmt 1), depth fmt 2, load clear"); }
static void finish(void) { pg_mark(1, "(after pass, 2 draws in it; ended at line 42)"); render_frame++; pg_report(); }
int main(int argc, char **argv) {
    CHECK(argc == 2);
    unsetenv("LARECOMP_VK_PASS_GPU"); unsetenv("LARECOMP_VK_PASS_GPU_DRAWS"); unsetenv("LARECOMP_VK_PASS_GPU_ALL");
    g_vk.ts_bits = 64; g_vk.props.limits.timestampPeriod = 1;
    g_vk.props.limits.timestampComputeAndGraphics = true;
    pg_draws0 = 0;
    const char *c = argv[1];
    if (!strcmp(c, "disabled") || !strcmp(c, "zero")) {
        if (!strcmp(c, "zero")) setenv("LARECOMP_VK_PASS_GPU", "0", 1);
        pg_mark(1, "pass ignored"); pg_report();
        CHECK(!creates && !writes && !reads && !frame_reads && !clock_reads && !log_size);
    } else if (!strcmp(c, "invalid") || !strcmp(c, "overflow")) {
        setenv("LARECOMP_VK_PASS_GPU", !strcmp(c, "invalid") ? "-1" : "18446744073709551616", 1);
        pg_mark(1, "pass ignored"); pg_report();
        CHECK(pg_state == 3 && has("invalid target") && !creates && !frame_reads);
    } else {
        setenv("LARECOMP_VK_PASS_GPU", "100", 1); setenv("LARECOMP_VK_PASS_GPU_DRAWS", "1", 1);
        if (!strcmp(c, "late")) {
            render_frame = 99; game_frame = 1400; pg_mark(1, "too soon"); CHECK(!writes);
            render_frame = 102; pg_mark(1, "pass late"); CHECK(pg_frame == 102 && has("target missed"));
            game_frame += 90; draw_mark(7); pg_report(); CHECK(pg_state == 1 && !reads && resets == 1);
            finish(); CHECK(pg_state == 3 && has("render frame 102 requested 100"));
        } else if (!strcmp(c, "no_work")) {
            render_frame = 100; pg_report(); pg_report(); CHECK(has("reached without marks") && !creates);
            render_frame = 103; pg_mark(1, "dispatch 1x1x1 cs 123"); finish();
            CHECK(pg_frame == 103 && has("target missed") && reads == 1);
        } else if (!strcmp(c, "unsupported")) {
            g_vk.ts_bits = 0; start(); CHECK(pg_state == 3 && !creates && has("unsupported timestamp"));
        } else if (!strcmp(c, "create_error")) {
            create_result = VK_ERROR_DEVICE_LOST; start(); CHECK(pg_state == 3 && !writes && has("create query pool result -4"));
        } else if (!strcmp(c, "insufficient")) {
            start(); render_frame++; pg_report(); CHECK(pg_state == 3 && !reads && has("only 1 mark"));
        } else if (!strcmp(c, "cap")) {
            start(); for (int i = 1; i < PG_MAX; i++) draw_mark(3);
            CHECK(pg_n == PG_MAX && writes == PG_MAX && pg_state == 1);
            uint64_t vertices = pg_verts, final_v = pg_vc[PG_MAX - 1];
            draw_mark(99); draw_mark(99); CHECK(pg_state == 2 && pg_verts == vertices && pg_vc[PG_MAX - 1] == final_v);
            pg_report(); CHECK(!reads); render_frame++; pg_report();
            CHECK(reads == 1 && has("capture truncated") && has("partial: mark capacity reached") && has("partial pass"));
        } else if (!strcmp(c, "exact_cap")) {
            start(); for (int i = 1; i < PG_MAX; i++) draw_mark(3);
            render_frame++; pg_report(); CHECK(has("partial: mark capacity reached"));
        } else if (!strcmp(c, "pending") || !strcmp(c, "timeout") || !strcmp(c, "frame_timeout")) {
            start(); draw_mark(12); read_result = VK_NOT_READY; finish();
            CHECK(pg_state == 2 && reads == 1 && has("readback pending") && submitted_seq < pg_seq);
            if (!strcmp(c, "pending")) {
                clock_ns += 1000000; submitted_seq = pending_seq; read_result = VK_SUCCESS; pg_report();
                CHECK(pg_state == 3 && has("complete marked range"));
            } else {
                if (!strcmp(c, "timeout")) clock_ns += PG_READBACK_NS;
                else render_frame = pg_frame + PG_READBACK_FRAMES;
                pg_report(); CHECK(pg_state == 3 && has("readback failed: timeout") && has("submitted 0; no query WAIT"));
            }
        } else if (!strcmp(c, "read_error")) {
            start(); read_result = VK_ERROR_DEVICE_LOST; finish(); CHECK(pg_state == 3 && has("readback failed: result -4"));
        } else if (!strcmp(c, "wrap")) {
            g_vk.ts_bits = 32; start(); draw_mark(2); timestamps[0] = UINT32_MAX - 499999; timestamps[1] = 500000;
            render_frame++; pg_report(); CHECK(has("1.00 ms first to last") && has("1.000 ms"));
        } else if (!strcmp(c, "groups")) {
            start(); draw_mark(4); draw_mark(8); pg_report(); CHECK(pg_state == 1 && !reads);
            finish(); CHECK(pg_state == 3 && has("3.00 ms first to last") && has("[passgpu-pass]  3.000 ms") && has("2 draws       12 vertices draw fs 123 vs 456") && !has("partial pass"));
            unsigned old_reads = reads; pg_report(); CHECK(reads == old_reads);
        } else if (!strcmp(c, "group_cap")) {
            start(); for (int i = 0; i < 257; i++) pg_mark(1, "draw fs %x vs 456", i);
            finish(); CHECK(has("partial grouping: 1 draw marks exceed 256"));
        } else if (!strcmp(c, "draws_absent")) {
            unsetenv("LARECOMP_VK_PASS_GPU_DRAWS"); start(); draw_mark(99); CHECK(writes == 1); finish(); CHECK(!has("[drawgpu]"));
        } else return 2;
    }
    printf("{\"assertions\":%u}\n", checks);
    return 0;
}
