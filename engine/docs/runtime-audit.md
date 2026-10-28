# Runtime and generated-code compatibility audit

Audit date: 2026-10-24. This is source evidence and an extraction proposal. No Android build, game execution, asset import, performance measurement, or device compatibility is established by this audit.

## Pinned evidence

All links below refer to the read-only local checkouts at these exact commits:

| Local tree | Origin | Commit |
| --- | --- | --- |
| `sources/las-android` | `Nycolazs/LASRecomp-Android-Plus` | `cca791a3e7218e1dac95e7d4a11b8f97dba3ea51` |
| `sources/las-upstream` | `peiikai/LASRecomp` | `59b61fd3c65dcabd0feb62d5d5706240cb3f5bb9` |
| `sources/letsgo` | `raulzrrs/letsgo-recomp` | `96894fd8e9ea20c1b38cdea53105ec2a335df620` |

Tracked source changes were empty in all three trees. Reproduce the source inventory with `python3 tools/audit_runtime.py`. Its declaration counts are textual, exclude headers, include conditional code, and do not measure implemented features or runtime import coverage.

## Conclusion

The projects share a recognizable AOT execution model, guest CPU register prefix, NSO loader, HLE symbol convention, and NVN API names. Their game libraries are **not binary interchangeable**. A shared runtime needs a new explicit generated-code contract and title adapters; copying one executable into another title's packaging cannot satisfy the identical-runtime-artifact requirement.

Use LAS Android as the initial Android platform and graphics baseline, retain its working AOT path, and generalize one boundary at a time. Import Let’s Go’s module model and required semantics through reviewed changes. Do not replace the Android renderer wholesale or infer title support from the common ancestry.

## CPU and game-library ABI

| Boundary | LAS upstream | LAS Android | Let’s Go |
| --- | --- | --- | --- |
| `GuestFn` | `void (*)(Cpu *)` | Same | Same |
| Register prefix | `sp=256`, `pc=264`, `nzcv=272`, `fpcr=320`, `fpsr=328`, vectors at 512 | Same | Same |
| `Cpu` size/alignment on inspected host LP64 | 1024 / 16 bytes | 66560 / 16 bytes | 1024 / 16 bytes |
| Dispatch cache | Runtime thread-local | 4096 entries appended to `Cpu` | Runtime thread-local |
| Game-library ABI constant | 1 | 1 | 3 |
| `GameLibTables` host LP64 size | 64 bytes | 64 bytes | 152 bytes |
| Function tables | Fixed main/SDK fields | Fixed main/SDK fields | Count plus four named module descriptors |
| Hook path used by generated functions | Instruction-site `rt_hook` | Instruction-site `rt_hook`; Android normally disables `FN_ENTER` profiling | `FN_ENTER` can call `rt_fnhook` and return early |

Sources: [LAS CPU](../sources/las-android/runtime/cpu.h), [upstream CPU](../sources/las-upstream/runtime/cpu.h), [Let’s Go CPU](../sources/letsgo/runtime/cpu.h), [LAS game-library header](../sources/las-android/runtime/gamelib.h), [Let’s Go game-library header](../sources/letsgo/runtime/gamelib.h). Sizes were verified by compiling a temporary C11 program against each unmodified header with local Clang and printing `sizeof`, `_Alignof`, and `offsetof`; Android/NDK ABI probing remains required.

The prefix match permits a migration strategy, not arbitrary mixed headers. LAS Android dispatch accesses memory beyond Let’s Go’s `Cpu` allocation. A library compiled against another `FN_ENTER`, SIMD mode, shift, or hook convention has different behavior even when register offsets match. Both exported entries are called `lasgame_tables`; checking the name alone is insufficient. The upstream and Android forks both report ABI 1 despite the cache and address-policy differences, relying on their separate content IDs to reject incompatible generated code.

[LAS entry](../sources/las-android/tools/gamelib_entry.c) exports main/SDK tables and optional `lasgame_pgo_dump`. [Let’s Go entry](../sources/letsgo/tools/gamelib_entry.c) exports four descriptors. Both loaders use `dlopen(..., RTLD_NOW | RTLD_LOCAL)` and `dlsym`, then validate ABI and ID ([LAS loader](../sources/las-android/runtime/gamelib.c), [Let’s Go loader](../sources/letsgo/runtime/gamelib.c)). The generated library still calls runtime functions and may reference runtime globals, so its complete undefined-symbol set must resolve in the actual Android linker namespace before the entry check can run. An explicit versioned runtime API is preferable to preserving unrestricted executable/global exports indefinitely.

LAS Android’s `libmain.so` intentionally exports runtime symbols, hides static dependency exports, retains registration sections, uses `-Bsymbolic`, and requests 16 KB maximum ELF page size ([CMake](../sources/las-android/android/app/CMakeLists.txt)). Desktop Makefiles use `-rdynamic` or executable import libraries. These are different linkage environments; neither successful desktop linking nor matching symbol spelling proves Android loadability.

LAS derives a code-generation ID from generators, headers, flags, patch/version data and, on Android, shift/hot/split options; its full library ID appends the first eight bytes of **main**’s build ID ([Android ID script](../sources/las-android/tools/android/gamelib_id.sh), [header](../sources/las-android/runtime/gamelib.h)). Let’s Go’s Makefile-derived ID hashes generator/header/flag inputs without appending per-title module build IDs ([Makefile](../sources/letsgo/Makefile)). Neither scheme supplies the desired full module manifest. Preserve build caching separately from runtime ABI: the new manifest should identify every NSO by full build ID/hash, title/build profile, generator revision/options, CPU ABI, SIMD mode, shift, target triple, and required runtime API version.

## Dispatch, module layout, and loader

Both dispatchers binary-search sorted guest-address tables, check HLE and address overrides on misses, and use `musttail` for indirect calls ([LAS dispatch](../sources/las-android/runtime/dispatch.c), [Let’s Go dispatch](../sources/letsgo/runtime/dispatch.c)). LAS assumes two nonempty tables; Let’s Go skips empty tables and iterates up to four. Keep LAS Android’s Cpu-local cache initially, generalize the table registry, and validate count/pointer consistency, sorted addresses, duplicates, module bounds, and overlap before dispatch. Tail-call behavior needs an actual NDK/arm64 build check. Function-entry hooks are needed for direct generated calls as well as dispatch misses; moving them solely into dispatch would change Let’s Go behavior.

The guest model uses host pointers as guest addresses. Address policy is therefore part of generated code and every host callback, not a loader-only option.

| Region/module | Desktop canonical base | LAS Android actual base |
| --- | --- | --- |
| Guest window | `0x7000000000`–`0x7320000000` | `0x1000000000`–`0x1320000000` |
| main | `0x7000000000` | `0x1000000000` |
| SDK | `0x7004000000` | `0x1004000000` |
| Let’s Go subsdk0 | `0x7006000000` | Proposed `0x1006000000`; not implemented upstream |
| Let’s Go subsdk1 | `0x7008000000` | Proposed `0x1008000000`; not implemented upstream |
| HLE trampoline space | `0x7080000000` | `0x1080000000` |
| TLS / HLE-owned heap | `0x7090000000` / `0x7092000000` | `0x1090000000` / `0x1092000000` |
| Alias region / application heap | `0x7100000000` / `0x7240000000` | `0x1100000000` / `0x1240000000` |

Sources: [LAS layout](../sources/las-android/runtime/rt.h), [Let’s Go layout](../sources/letsgo/runtime/rt.h). LAS Android subtracts `0x6000000000` with `GUEST()` and generates with `--shift`; function/label names and patch descriptions retain canonical addresses ([generator](../sources/las-android/tools/gen.py)). Let’s Go has no corresponding shift option in its generator. Its hook addresses, guest-pointer tests, GOT locations and memory scanners include raw `0x70...` values. Port canonical-address translation, pointer-range checks, generated immediates and tables together. Subtracting a constant only in `rt.h` is insufficient.

[LAS loader](../sources/las-android/runtime/loader.c) is identical to the pinned upstream loader and loads main then SDK. [Let’s Go loader](../sources/letsgo/runtime/loader.c) loads **main, SDK, subsdk0, subsdk1** at the same bases used by its Makefile generation and entry table. Its source identifies subsdk0 as the runtime GLSL compiler and subsdk1 as multimedia. They are recompiled guest modules, not optional host replacements. Do not drop them because host shaderc or FFmpeg is present. Loading, generation, dispatch, relocation, and constructors must agree on the same module inventory.

All loaders decompress NSO segments into the reserved window, read MOD0/dynamic metadata, and handle AArch64 ABS64, GLOB_DAT, JUMP_SLOT and RELATIVE relocations. All load modules before relocating. A defined symbol stays guest; undefined imports prefer a host HLE implementation, then another guest module, then zero for weak symbols, otherwise a generic HLE stub. Preserve that precedence. Both loaders have limited input-bound checks; malformed segment offsets, decompression lengths, symbol indexes and dynamic tables need validation before this becomes a general import boundary.

[LAS startup](../sources/las-android/runtime/main.c) initializes the SDK and then main through callbacks into `nn::init::Start`. [Let’s Go startup](../sources/letsgo/runtime/main.c) initializes all non-main modules in order, then main, preferring each module’s `DT_INIT` over its init array. Preserve this ordering and guest SDK TLS setup. LAS Android also handles reservation failure without desktop re-exec; Android zygote memory placement makes the desktop retry strategy inappropriate.

## SDK/HLE registration and title assumptions

`HLE_FUNC` and `SERVICE` build linker-section registries; NVN uses `NVN(name)` to expand into the same HLE mechanism ([HLE macros](../sources/las-android/runtime/hle.h), [service macros](../sources/las-android/runtime/ipc.h), [NVN macros](../sources/letsgo/runtime/nvn/nvn.h)). NVN functions are also requested dynamically through `nvnBootstrapLoader` / `nvnDeviceGetProcAddress` ([bootstrap](../sources/las-android/runtime/hle/nvn_boot.c)). Retaining the sections and constructor/registration objects in the shared artifact is essential.

Reproducible literal-declaration inventory:

| Inventory | LAS upstream | LAS Android | Let’s Go |
| --- | --- | --- | --- |
| Literal named HLE declarations in implementation files | 69 | 69 | 84 |
| Literal NVN declarations | 495 | 495 | 495 |
| Literal service declarations | 21 | 21 | 28 |

LAS Android and Let’s Go share 67 literal HLE names. Let’s Go adds 17: mostly six-axis/touch/gesture functions, layer scaling, and two movie functions; LAS has two exception-related declarations absent from Let’s Go. The 495 NVN names match exactly, which says nothing about completeness of their bodies. The service sets share 20 names: LAS uniquely declares `nfp:user`; Let’s Go adds `bsd:u`, `bt`, `btm:u`, `csrng`, `hwopus`, `ldn:u`, `nifm:u`, and `ssl` ([services](../sources/letsgo/runtime/services)). Several registrations are stubs. Audit each title’s actual static imports and dynamic proc requests, and log generic unimplemented calls; their current fallback returns zero and can conceal missing behavior.

Title assumptions that must leave the common runtime:

- LAS reports program ID `0x01006BB00C6F0000` directly from `svcGetInfo` ([kernel](../sources/las-android/runtime/kernel.c)); Let’s Go reads ACI0 from `main.npdm` and returns `g_program_id` ([startup](../sources/letsgo/runtime/main.c), [kernel](../sources/letsgo/runtime/kernel.c)). Generic NPDM parsing and identity belong in the runtime; accepted IDs/builds belong in adapters.
- LAS’s instruction hooks, version address maps, randomizer, widescreen, warp, inventory and cosmetic features refer to game objects and offsets ([hooks](../sources/las-android/runtime/hle/hooks.c), [addresses](../sources/las-android/runtime/game_addresses.h)). Their existing mechanisms are not generic title behavior.
- Let’s Go’s `fnhook.c` embeds Eevee 1.0.0 → Pikachu 1.0.0 mappings; unknown builds receive no mapped hooks ([function hooks](../sources/letsgo/runtime/fnhook.c), [build detection](../sources/letsgo/runtime/gamesetup.c)). Controller skip, capture/battle changes, cutscenes and frame-rate patches use those functions and object offsets ([controller](../sources/letsgo/runtime/hle/controller_skip.c), [battle](../sources/letsgo/runtime/hle/wildbattle.c), [FPS](../sources/letsgo/runtime/hle/fps60.c)). Keep the generic hook engine separate from these registrations and address maps.
- Save selection includes title-specific policy ([Let’s Go FS](../sources/letsgo/runtime/services/fs.c)); input defaults and motion behavior also differ ([LAS input](../sources/las-android/runtime/hle/input.c), [Let’s Go input](../sources/letsgo/runtime/hle/input.c)). Adapters need explicit save namespaces and input profiles. Do not inherit a shared `save` directory or change the owner’s existing controller settings.

Both runtimes retain process-wide caches, registries, environment values and loaded library tables. Reboot reset hooks do not establish safe title unloading. Use a **fresh Android process per game session**, as required by the workspace guidance; defer same-process title switching.

## Graphics divergence

The NVN front end, Maxwell translation and Vulkan backend are related but diverged. Among the 22 same-path files under `runtime/nvn`, only seven are byte-identical between LAS Android and Let’s Go. Same API names are not a graphics compatibility proof.

- LAS Android contains Android surface creation, volk-loaded Vulkan entry points, API/feature negotiation, driver selection integration, mobile memory handling and Android-specific shader work ([device](../sources/las-android/runtime/nvn/vulkan/device.c), [shader compilation](../sources/las-android/runtime/nvn/vulkan/shaders.c), [Maxwell translation](../sources/las-android/runtime/nvn/maxwell/translate.c)). Keep these as the initial host backend.
- Let’s Go’s device/shader path assumes Vulkan 1.3 in places and adds a render thread with queue/fence/dirty-memory semantics ([render thread](../sources/letsgo/runtime/nvn/vulkan/renderthread.c)); LAS has different queue and recording machinery ([queue](../sources/las-android/runtime/nvn/rqueue.c), [recording](../sources/las-android/runtime/nvn/vulkan/record.c)). These require semantic comparison, not a file union.
- Let’s Go adds title-facing SSAO/contact shadows/motion-vector work and FSR integration ([shaders](../sources/letsgo/runtime/nvn/vulkan/shaders.c), [execution](../sources/letsgo/runtime/nvn/vulkan/exec.c)). Its FSR 3 wrapper dynamically uses a Windows DLL and `fsr3_available()` returns false on other platforms ([wrapper](../sources/letsgo/runtime/nvn/vulkan/fsr3.cpp)); this does not establish Android FSR 3 support.
- LAS has recolor/texture patching and shader preparation paths absent from Let’s Go; Let’s Go’s Maxwell translator includes fixes for its own shaders ([translator](../sources/letsgo/runtime/nvn/maxwell/translate.c)). Extract reusable opcode/format corrections individually with representative shader evidence. Put game-specific hashes, replacement textures, projections and visual policy in adapters.

Start with baseline rendering settings and deterministic captures. Disable optional visual enhancements for the first comparison. Let’s Go’s skinned models, effects, movie playback, texture updates, fences and transitions are separate acceptance cases from LAS’s field rendering.

## Extraction order and acceptance gate

1. **Freeze unmodified baselines.** Record complete module build IDs and private file hashes, build inputs, packaged runtime hash, and observed scene behavior for each title. Confirm a LAS Android session independently before moving its runtime. Let’s Go Android execution is pending and may expose additional platform dependencies.
2. **Define one versioned ABI.** Freeze `Cpu` layout with compile-time offset/size assertions; add sized/versioned table descriptors and explicit module metadata. Preserve LAS’s cache placement and AOT SIMD mode initially. Distinguish canonical addresses from actual guest pointers, cover all four Let’s Go modules, and reject unsupported profiles before dispatch. Rebuild both title libraries against this ABI; do not relabel old libraries.
3. **Extract platform, loader, dispatch and basic services.** Keep one Android runtime artifact and a module manifest supplied by the selected adapter. Preserve import precedence and init order. Move title ID/save/input defaults and known-build checks out of core. Add Let’s Go’s required service/HLE deltas after examining their actual behavior.
4. **Separate hook engine from title hooks.** Retain instruction-site and function-entry semantics, including original-call bypass, as explicit capabilities. Move LAS patches and Let’s Go maps/object offsets into separate adapters; apply shift translation consistently. Verify direct-call hooks as well as indirect dispatch.
5. **Reconcile graphics incrementally.** Keep the Android host setup and mobile feature negotiation. Port only required Let’s Go translator/NVN behavior, then queue/fence/dirty tracking changes with captures and validation evidence. Treat optional enhancements as later work.

The shared-runtime milestone passes only when all of the following are demonstrated:

- Two title adapters and locally generated title libraries load the **same byte-for-byte runtime `.so`**; record its hash and actual loaded mapping in each fresh process. A common source tree producing two differently compiled runtimes does not pass.
- NDK arm64 builds verify CPU/table layouts, required exports/undefined imports, registration-section retention and `musttail` behavior. Runtime loading rejects wrong ABI, wrong shift/SIMD profile, wrong title/module set and changed module hashes with clear errors. No guest entry runs after rejection.
- All two/four modules load, relocate and initialize within the declared non-overlapping window. Static import closure and dynamic NVN requests are recorded. Every generic unimplemented call seen in accepted scenarios is either implemented or explicitly reviewed with a justified behavior; silent return-zero stubs are not evidence of support.
- Each title reproduces its recorded baseline on the Thor with input, audio, save/load in its own managed directory, representative gameplay transitions and movie playback. Include Let’s Go capture/battle and skinned geometry, and LAS field/room transitions. Record duration, build IDs, screenshots and frame-time/memory/thermal observations privately; choose numerical tolerances from measured baselines before judging regressions.
- Fresh-process restart and interruption recover without corrupting saves or sharing title state. Existing installations and settings remain untouched. Renderer validation errors, new visual defects, unexplained crashes or unbounded memory growth keep the gate open.

Passing ABI/load checks is an engineering milestone. Compilation, library loading and a title screen alone do not establish gameplay support, and this audit promises none.
