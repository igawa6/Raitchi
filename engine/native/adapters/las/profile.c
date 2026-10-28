/* Policy derived from pinned LAS Recomp Android source at
 * cca791a3e7218e1dac95e7d4a11b8f97dba3ea51: runtime/rt.h, kernel.h,
 * kernel.c, main.c and game_addresses/versions.h.
 * Copyright (c) 2026 Peiikai. MIT: ../../runtime/LICENSE-LASRecomp. */
#include "../title_adapter.h"

const TitleAdapter las_title_adapter = {
    .id = TITLE_LAS, .key = "las", .label = "Link's Awakening", .version = "1.0.0",
    .save_namespace = "las-1.0.0", .program_id = UINT64_C(0x01006BB00C6F0000),
    .guest_shift = UINT64_C(0x6000000000),
    .canonical_window_lo = UINT64_C(0x7000000000), .canonical_window_hi = UINT64_C(0x7320000000),
    .capabilities = 1, .audout_min_queue_ms = 0, .module_count = 2, .main_build_prefix_size = 20,
    .renderer_policy_flags = TITLE_RENDERER_LEGACY_TARGET_HEURISTICS,
    .main_build_prefix = {0xae, 0x16, 0xf7, 0x1e, 0x00, 0x2a, 0xf8, 0xcb, 0x05, 0x9a,
                          0x9a, 0x74, 0xc4, 0xd9, 0x0f, 0x34, 0xba, 0x98, 0x48, 0x92},
    .modules = {{MODULE_ABI_MAIN, "main", UINT64_C(0x7000000000)},
                {MODULE_ABI_SDK, "sdk", UINT64_C(0x7004000000)}},
    .regions = {
        [SESSION_HLE] = {UINT64_C(0x7080000000), UINT64_C(0x100000)},
        [SESSION_TLS] = {UINT64_C(0x7090000000), UINT64_C(0x100000)},
        [SESSION_HOST_HEAP] = {UINT64_C(0x7092000000), UINT64_C(0xe000000)},
        [SESSION_ALIAS] = {UINT64_C(0x7100000000), UINT64_C(0x100000000)},
        [SESSION_STACK] = {UINT64_C(0x7200000000), UINT64_C(0x40000000)},
        [SESSION_APP_HEAP] = {UINT64_C(0x7240000000), UINT64_C(0xd0000000)}},
    .startup = {.argument_count = 4, .early_count = 1, .early_modules = {MODULE_ABI_SDK},
                .late_count = 1, .late_modules = {MODULE_ABI_MAIN}}
};
