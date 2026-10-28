# Native game startup memory audit

Historical source audit; the subsequent implementation and device status are in
[the original-code backend report](native-game-backend.md).

The source audit found an exact mechanism for the observed repeated fourfold host allocations: native SDK definition overrides can recursively reenter the existing heap HLE wrappers. This is a routing error; no argument ABI mismatch is required to produce the observed growth.

`loader_native_host_definitions` in [loader.c](../ports/shared-android/runtime/loader.c) registers matching SDK definitions as HLE overrides. The existing `heap_create_unit` wrapper in the pinned runtime's `hle/heaps.c` defaults its unit-heap scale to four, allocates and clears that larger host buffer, and updates the size argument. Its `CALL` macro then calls `guest_call` at the original SDK export returned by `loader_find_symbol`. With that export now overridden, native host lookup selects `heap_create_unit` again. Each recursive invocation scales the already scaled size by another factor of four before it attempts to call the original body.

That path produces the owner's measured 8 MiB, 32 MiB, 128 MiB, 512 MiB, 2 GiB and 8 GiB sequence directly. The new host allocations are cleared immediately, accounting for resident-memory growth before Android kills the process. All nine heap HLE wrappers use the same passthrough `CALL` pattern, so fixing only the allocating wrapper would leave other recursive paths.

The native host lookup currently suppresses overrides only when `aot_original_call_take` consumes an explicit `rt_call_original` token. The heap wrappers call `guest_call`, so they do not arm that token. Simply substituting `rt_call_original` also needs an intentional native-mode capability rule: the existing default API requires the function-hook capability, while LAS declares instruction hooks only. The C-AOT default contract should retain its existing checks.

Two bounded corrections are available:

- Give native passthrough wrappers an explicit original-entry call that bypasses HLE/overrides once, while preserving the existing nested-call return boundary and saved LR. Then execute the original SDK function through the selected native backend.
- Exclude passthrough heap wrappers from native *defined-symbol* overrides. Undefined main-module imports can still use their existing HLE stubs, whose calls into unoverridden SDK definitions then reach the original bodies.

The first option preserves host heap scaling for direct SDK calls and makes the bypass explicit. A regression should use small synthetic allocation sizes, require exactly one scaling allocation per wrapper invocation, observe entry into the original backend once, preserve nested bypass tokens/LR, and prove that generated C is never selected. A native LAS capability-one case and the unchanged inactive-backend capability failures both need coverage.

The SDK export inventory independently found exactly 55 literal defined-symbol HLE matches, agreeing with the runtime's binding count: 24 input, 14 display/graphics setup, nine heap, three font, two exception diagnostics, and one each for NVN bootstrap, keyboard and time. Neither host `malloc`/`realloc` nor the SDK DSO initializer is one of those 55 overridden definitions. The exception diagnostic wrappers log and terminate; they do not explain repeated fourfold host allocation growth. The private inventory is `evidence/native-sdk-host-definitions-audit.json`.

A bounded private parse of the verified SDK also checked its initializer against the generated C reference. DT_INIT sets up registration arguments, calls `__nnmusl_init_dso`, then iterates its constructor list. The inspected entry is not a broad BSS-zeroing loop. Its original BSS is approximately 764 KiB and its complete runtime module mapping is approximately 13 MiB, far below the measured host allocation sequence. Detailed original offsets and instructions remain only in ignored `evidence/native-sdk-init-audit.json`; no original instructions or addresses are copied here. This inspection used the pinned NSO parser and Capstone, not REA evidence.

This report is source evidence correlated with the measured growth pattern. It does not replace the next device trace or establish that this is the only remaining startup issue. No source code, device state or builds were changed by this audit.
