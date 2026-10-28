/* Base-v0 policy derived from public EoWRecomp source at
 * dd9c79d6c01d9eaddafc1fc5a07b6658bfc56505: runtime/modules.h, rt.h,
 * kernel.h, main.c and docs/GAP.md. Upstream MIT license is retained in LICENSE.
 * Build prefixes recognize public baselines; they do not replace full NSO hashes. */
#include "../title_adapter.h"
const TitleAdapter echoes_title_adapter = {
    .id = TITLE_ECHOES, .key = "echoes", .label = "The Legend of Zelda: Echoes of Wisdom",
    .version = "1.0.0", .save_namespace = "echoes-1.0.0",
    .program_id = UINT64_C(0x01008cf01baac000), .guest_shift = UINT64_C(0x6000000000),
    .canonical_window_lo = UINT64_C(0x7000000000), .canonical_window_hi = UINT64_C(0x7320000000),
    .capabilities = 0, .audout_min_queue_ms = 0, .module_count = 4,
    .main_build_prefix_size = 8, .main_build_prefix = {0xa5,0x81,0x40,0x6b,0xc9,0x85,0x86,0x51},
    .module_build_prefix_size = {8,8,8,8},
    .module_build_prefix = {{0xa5,0x81,0x40,0x6b,0xc9,0x85,0x86,0x51},
        {0xe0,0xd8,0xad,0xcc,0x44,0x6c,0xf1,0x4d},
        {0xff,0x01,0x49,0xd0,0xb5,0xe8,0x82,0xb8},
        {0x30,0x53,0xb2,0x29,0x35,0xe3,0x2d,0xdf}},
    .module_spans = {UINT64_C(0x8000000),UINT64_C(0x2000000),UINT64_C(0x2000000),UINT64_C(0x2000000)},
    .modules = {{MODULE_ABI_MAIN,"main",UINT64_C(0x7000000000)},
        {MODULE_ABI_SUBSDK0,"subsdk0",UINT64_C(0x7008000000)},
        {MODULE_ABI_SUBSDK1,"subsdk1",UINT64_C(0x700a000000)},
        {MODULE_ABI_SDK,"sdk",UINT64_C(0x700c000000)}},
    .regions = {
        [SESSION_HLE] = {UINT64_C(0x7080000000),UINT64_C(0x100000)},
        [SESSION_TLS] = {UINT64_C(0x7090000000),UINT64_C(0x100000)},
        [SESSION_HOST_HEAP] = {UINT64_C(0x7092000000),UINT64_C(0xe000000)},
        [SESSION_ALIAS] = {UINT64_C(0x7100000000),UINT64_C(0x100000000)},
        [SESSION_STACK] = {UINT64_C(0x7200000000),UINT64_C(0x40000000)},
        [SESSION_APP_HEAP] = {UINT64_C(0x7240000000),UINT64_C(0xd0000000)}},
    .startup = {.argument_count=5,.early_count=1,.early_modules={MODULE_ABI_SDK},
        .late_count=3,.late_modules={MODULE_ABI_SUBSDK1,MODULE_ABI_SUBSDK0,MODULE_ABI_MAIN}}
};
