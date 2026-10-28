/* Experimental base-v0 native policy. Original NSO module identities and spans
 * were measured from the owner's package; SDK five-argument Start is exported.
 * SDK-early/main-late follows the shared SDK16 startup policy. Gameplay and
 * original rtld callback ordering remain subject to runtime acceptance. */
#include "../title_adapter.h"
const TitleAdapter wonder_title_adapter = {
    .id = TITLE_WONDER, .key = "wonder", .label = "Super Mario Bros. Wonder (experimental)",
    .version = "1.0.0", .save_namespace = "wonder-1.0.0",
    .program_id = UINT64_C(0x010015100b514000), .guest_shift = UINT64_C(0x6000000000),
    .canonical_window_lo = UINT64_C(0x7000000000), .canonical_window_hi = UINT64_C(0x7320000000),
    .capabilities = 0, .audout_min_queue_ms = 0, .module_count = 2,
    .nvn_api_major = 55, .nvn_api_minor = 15,
    .main_build_prefix_size = 20, .main_build_prefix = {0xcd,0x6e,0x42,0xae,0xe7,0x93,0x4f,0x4d,0x83,0x93,0xcd,0x43,0x89,0x3a,0xe2,0x5e,0xdc,0x83,0xd3,0x38},
    .module_build_prefix_size = {20,20},
    .module_build_prefix = {{0xcd,0x6e,0x42,0xae,0xe7,0x93,0x4f,0x4d,0x83,0x93,0xcd,0x43,0x89,0x3a,0xe2,0x5e,0xdc,0x83,0xd3,0x38},{0x30,0x53,0xb2,0x29,0x35,0xe3,0x2d,0xdf,0x15,0xef,0x81,0x99,0x76,0x03,0xe5,0xc2,0x67,0x53,0x4a,0x90}},
    .module_spans = {UINT64_C(0x8000000),UINT64_C(0x2000000)},
    .modules = {{MODULE_ABI_MAIN,"main",UINT64_C(0x7000000000)},
        {MODULE_ABI_SDK,"sdk",UINT64_C(0x7008000000)}},
    .regions = {
        [SESSION_HLE] = {UINT64_C(0x7080000000),UINT64_C(0x100000)},
        [SESSION_TLS] = {UINT64_C(0x7090000000),UINT64_C(0x100000)},
        [SESSION_HOST_HEAP] = {UINT64_C(0x7092000000),UINT64_C(0xe000000)},
        [SESSION_ALIAS] = {UINT64_C(0x7100000000),UINT64_C(0x100000000)},
        [SESSION_STACK] = {UINT64_C(0x7200000000),UINT64_C(0x40000000)},
        [SESSION_APP_HEAP] = {UINT64_C(0x7240000000),UINT64_C(0xd0000000)}},
    .startup = {.argument_count=5,.early_count=1,.early_modules={MODULE_ABI_SDK},
        .late_count=1,.late_modules={MODULE_ABI_MAIN}}
};
