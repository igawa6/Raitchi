/* Policy derived from pinned Let's Go Recomp source at
 * 96894fd8e9ea20c1b38cdea53105ec2a335df620: runtime/rt.h, kernel.h,
 * main.c and gamesetup.c. Copyright (c) 2026 raulzito12345rrs.
 * MIT: ../../runtime/LICENSE-LetsGo. */
#include "../title_adapter.h"

const TitleAdapter eevee_title_adapter = {
    .id = TITLE_EEVEE, .key = "eevee", .label = "Let's Go, Eevee!", .version = "1.0.0",
    .save_namespace = "eevee-1.0.0", .program_id = UINT64_C(0x0100187003A36000),
    .guest_shift = 0,
    .canonical_window_lo = UINT64_C(0x7000000000), .canonical_window_hi = UINT64_C(0x7320000000),
    .capabilities = 2, .audout_min_queue_ms = 60, .module_count = 4, .main_build_prefix_size = 8,
    .main_build_prefix = {0x0a, 0x3f, 0x0d, 0x21, 0xe6, 0x7d, 0xad, 0x80},
    .modules = {{MODULE_ABI_MAIN, "main", UINT64_C(0x7000000000)},
                {MODULE_ABI_SDK, "sdk", UINT64_C(0x7004000000)},
                {MODULE_ABI_SUBSDK0, "subsdk0", UINT64_C(0x7006000000)},
                {MODULE_ABI_SUBSDK1, "subsdk1", UINT64_C(0x7008000000)}},
    .regions = {
        [SESSION_HLE] = {UINT64_C(0x7080000000), UINT64_C(0x100000)},
        [SESSION_TLS] = {UINT64_C(0x7090000000), UINT64_C(0x100000)},
        [SESSION_HOST_HEAP] = {UINT64_C(0x7092000000), UINT64_C(0xe000000)},
        [SESSION_ALIAS] = {UINT64_C(0x7100000000), UINT64_C(0x100000000)},
        [SESSION_STACK] = {UINT64_C(0x7200000000), UINT64_C(0x40000000)},
        [SESSION_APP_HEAP] = {UINT64_C(0x7240000000), UINT64_C(0xd0000000)}},
    .startup = {.argument_count = 4, .early_count = 3,
                .early_modules = {MODULE_ABI_SDK, MODULE_ABI_SUBSDK0, MODULE_ABI_SUBSDK1},
                .late_count = 1, .late_modules = {MODULE_ABI_MAIN}}
};
