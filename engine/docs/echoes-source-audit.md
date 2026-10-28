# Echoes of Wisdom source constraints

Source-only audit, 2026-10-25. Remote `HEAD` resolved to `refs/heads/main` at **`dd9c79d6c01d9eaddafc1fc5a07b6658bfc56505`**, authored 2026-10-21. The detached, clean checkout is `sources/echoes`; its independent pin is [echoes-source.lock.json](../echoes-source.lock.json). The existing two-title source lock is unchanged. No repository scripts, skills, builds, generated code, game imports or device actions were run. Sizes below are source declarations/calculations or upstream-reported input facts, not measurements of locally supplied Echoes assets.

The [pinned README](https://github.com/Nycolazs/EoWRecomp/blob/dd9c79d6c01d9eaddafc1fc5a07b6658bfc56505/README.md) reports Linux/Vulkan opening and first controllable scene, with input. Android's reported verification is the launcher/setup on an Android15 ARM64 emulator; game execution on Android remains pending. These are maintainer reports, not verified Thor results. The alpha release does not establish Android gameplay, save support or performance. Echoes remains future design evidence; LAS/Eevee are the current shared-runtime acceptance pair.

## Actual module layout differs from both current titles

[`runtime/modules.h`](https://github.com/Nycolazs/EoWRecomp/blob/dd9c79d6c01d9eaddafc1fc5a07b6658bfc56505/runtime/modules.h) is the single list used by generation, loader, entry tables and dispatcher. Its order is also guest symbol lookup order:

| Load/lookup order | Module | Canonical base | Reserved room | Reported image size |
| --- | --- | --- | --- | --- |
|0 | main | `0x7000000000` |128 MiB | `0x4004000` |
|1 | subsdk0 | `0x7008000000` |32 MiB | `0xa8b000` |
|2 | subsdk1 | `0x700a000000` |32 MiB | `0x6b2000` |
|3 | sdk | `0x700c000000` |32 MiB | `0xe04000` |

Image sizes except main come from the upstream [GAP inventory](https://github.com/Nycolazs/EoWRecomp/blob/dd9c79d6c01d9eaddafc1fc5a07b6658bfc56505/docs/GAP.md); main's size is also documented in the active module header. **Main exceeds64 MiB by16 KiB**, so the LAS/Let's Go SDK base `0x7004000000` would overlap it. Echoes's SDK is at a different base and last in load order. Four modules alone therefore do not imply the current `main,sdk,subsdk0,subsdk1` profile.

The [loader](https://github.com/Nycolazs/EoWRecomp/blob/dd9c79d6c01d9eaddafc1fc5a07b6658bfc56505/runtime/loader.c) calculates mapped size from data offset + data size + BSS, rounded to16 KiB, then checks it fits before the next declared base. Applying that formula to the reported image sizes gives `0x4004000`, `0xa8c000`, `0x6b4000`, `0xe04000` respectively; these inferred values must be replaced by actual NSO-derived metadata before any load. The source's room check occurs after writing the NSO image; the common loader must validate bounds/overlap before committing or copying segments.

Android subtracts `0x6000000000` through `GUEST()`, and `tools/gen.py --shift` translates the same profile. Actual Android bases would be `0x1000000000`, `0x1008000000`, `0x100a000000`, `0x100c000000`. The declared window is the familiar shifted `[0x1000000000,0x1320000000)`. No Echoes reservation was tested here.

## CPU, generated library and hooks

The [current CPU header](https://github.com/Nycolazs/EoWRecomp/blob/dd9c79d6c01d9eaddafc1fc5a07b6658bfc56505/runtime/cpu.h) retains `GuestFn = void (*)(Cpu *)`, the register prefix (`sp256`, `pc264`, `fpcr320`, `fpsr328`) and vectors at512, aligned16. It adds **`excl_hi` at336**, using eight bytes that LAS/Eevee treat as padding, then pads to512. The declaration implies a1,024-byte LP64 CPU; no compiler/target size probe was run. `dispatch.c` uses a separate TLS cache of4,096 `{addr,fn}` entries, rather than LAS Android's appended CPU cache.

For the proposed common66,560-byte CPU, naming/initializing `excl_hi` inside the prefix can preserve vector and appended-cache offsets. It still changes the meaning of previously reserved bytes: version the helper/CPU contract and declare the capability. A size-only compatibility test would miss this difference. Generated code must be rebuilt against the chosen common header; no binary compatibility with the existing Echoes library is asserted.

Current helpers/lifter implement exclusive pairs (`LDXP/STXP/LDAXP/STLXP`), CAS/CASP and ARMv8.1 LSE operations. The64-bit-register pair uses one `unsigned __int128` atomic load/CAS and stores the high monitor half in `Cpu.excl_hi`; it must not be lowered into two independently atomic64-bit accesses. `tools/test_lift.py` and `tools/test_lse.c` contain relevant tests, and STATUS reports synthesized ARM64/Unicorn checks. None were executed for this audit. Android toolchain lowering/linkage, possible compiler atomic helpers,16-byte alignment and contention semantics remain a future validation gate; source syntax does not prove them.

[`gamelib.h`](https://github.com/Nycolazs/EoWRecomp/blob/dd9c79d6c01d9eaddafc1fc5a07b6658bfc56505/runtime/gamelib.h) declares upstream ABI2, entry **`eowgame_tables`**, Android name `libeowgame.so`, a counted module-array pointer and32-byte module records. Its table is source-implied32 bytes on LP64, distinct from LAS's64-byte ABI1 and Eevee's152-byte ABI3. The Makefile fingerprint includes `runtime/modules.h`, generator/helper headers and flags; it is not a substitute for full original-module IDs/hashes and the shared execution contract.

`FN_ENTER` is profiling-only, compiled away unless `EOWRECOMP_FNPROF` is defined; there is no Eevee-style whole-function hook in this header. `tools/patches.py` currently has an empty `PATCHES` map with original-word checking infrastructure, and `gen.py` emits per-instruction replacements. Do not infer LAS's runtime instruction-site `rt_hook` protocol or Eevee's direct-function-entry hook capability from the presence of `FN_ENTER`/a patch file. Hook/profiling capabilities belong in the generated-artifact identity independently of CPU size.

## SDK startup and writable GPU memory

[`main.c`](https://github.com/Nycolazs/EoWRecomp/blob/dd9c79d6c01d9eaddafc1fc5a07b6658bfc56505/runtime/main.c) resolves SDK16.2's **five-argument** `nn::init::Start(thread,args,init_sdk,init,fini)`. SDK constructors run early. After SDK heap startup, its late callback walks loaded modules in reverse, excluding SDK: **subsdk1, subsdk0, main**. This differs from both the module load order and the current LAS/Eevee startup policy. Keep these three concepts separate: load/symbol resolution, constructor order/stage, and SDK callback signature. A generic loop that initializes every non-main module early would change behavior.

The current [IPC implementation](https://github.com/Nycolazs/EoWRecomp/blob/dd9c79d6c01d9eaddafc1fc5a07b6658bfc56505/runtime/ipc.c) handles TIPC in addition to HIPC. `services/audio.c` handles the larger effect-output status used from REV9 onward and the reported REV11 renderer setup. Kernel condvar comments describe guest timeout/reacquisition semantics, while `svcGetInfo` still hardcodes Echoes's program ID. Put accepted build IDs, program/save/input policy and SDK startup data in the adapter; retain protocol parsing, synchronization and versioned service structures in the core. Do not import title constants with a new generic service implementation.

The Maxwell/Vulkan source contains global loads **and stores**, including shader atomics. [`exec.c`](https://github.com/Nycolazs/EoWRecomp/blob/dd9c79d6c01d9eaddafc1fc5a07b6658bfc56505/runtime/nvn/vulkan/exec.c) imports the heap through `VK_EXT_external_memory_host`; compute `GMap/GFast` accelerates bound ranges and copies stores back **into that imported heap**. `gfast_prepare()` returns without mapped ranges when no imported heap exists. `gmem_info()` then supplies a4 KiB zero buffer. This desktop acceleration is not an Android CPU↔GPU mirror implementation. The translator still uses64-bit address arithmetic as well, so Thor's observed missing `shaderInt64` is another unverified port requirement.

The shared design must allow ordered GPU writes, readback/merge and later CPU/vertex/texture visibility. Eevee's current read-only bound-range snapshots are a deliberately limited implementation; rejecting unsupported stores is safer than presenting them as full capability. This audit does not prove which storing shaders the owner would encounter in Echoes. It establishes that the source models such operations and that the current Android fallback cannot supply their semantics.

## Documentation status and five shared-design requirements

The early `docs/ARCHITECTURE.md` still says exclusive pairs/LSE are unsupported and module layout needs revision. Current `cpu.h`, `lifter.py` and `modules.h` already implement those changes. Prefer active source for those facts. Conversely, STATUS's warning that Android lacks a copied global-memory path remains supported by the current `gmem_info/gfast_prepare` code. README/STATUS scene claims remain maintainer evidence; no Linux result has been independently reproduced here.

1. **Profile-defined modules and ranges:** stable module IDs/names plus explicit canonical bases, original hashes, sizes and load/lookup order. Preserve the useful two/four-module bounds, but do not freeze LAS/Eevee's base/order into a universal validator. Reject a profile unless its declared independent expectation is supported; add an Echoes-specific synthetic overlap/order fixture before future integration.
2. **A versioned CPU/helper contract with room for128-bit atomics:** retain LAS's cache offset/per-thread allocation while defining reserved-prefix semantics such as `excl_hi`. Identify atomic/helper requirements and exact generated-code options; test failed/successful pair stores and contention on the actual target before claiming support.
3. **Separate initialization policy and SDK capabilities:** support early/late constructor stages and five guest startup arguments without changing ordinary `GuestFn(Cpu *)`. Protocol/audio revision capabilities and title-specific registrations must be explicit. Module array order must not silently decide every initialization phase.
4. **Explicit hook capabilities:** permit compiled instruction replacements, LAS runtime site hooks, Eevee whole-function hooks and optional profiling as distinct producer requirements. Keep direct calls covered where required; avoid imposing an unused function-entry check on every title merely because one needs it.
5. **Extensible GPU memory semantics:** keep read-only snapshots as one capability, with an eventual coherent writable mirror/merge path and GPU feature negotiation. Fail unsupported operations clearly; do not assume imported host memory,64-bit shader arithmetic or a zero fallback universally works on Android.

These constraints justify small contract adjustments and future synthetic cases. They do not authorize a third-title port, a renderer replacement, asset import or device qualification, and should not delay the current LAS/Eevee execution seam.
