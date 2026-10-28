# Shared AOT execution foundation

This records the first component milestone. The subsequent [full Android
integration](shared-android-integration.md) adds the real NSO loader, OS/services,
adapters, renderer and two private producers. The current joint checker has94
scenarios per host variant; Android gameplay remains unverified. Counts below
describe the earlier foundation checkpoint.

Implemented and verified on the host, 2026-10-25. One `libshared_aot.so` now
executes synthetic two-module, four-module and optional-profiling producers
through the same dispatch, hooks, CPU layout and session gate. There are no
title/profile compile definitions in that common library. Each producer fixes
its own shift and generated hook capabilities.

This is the first executable shared component. It is not the complete Android
`libmain.so`: neither real game has been rebuilt or run through it. The working
LAS/Eevee apps and private game libraries remain reference artifacts. No device
action was performed for this milestone.

## Implemented boundary

| Component | Behavior |
| --- | --- |
| `native/runtime/aot_cpu.h` | Common LP64 CPU: 66,560 bytes, alignment16, vectors512, 4,096-entry per-CPU cache at1,024. Compile-time assertions cover the full layout. Arithmetic, memory, exclusive and ARM64 FP helpers retain the pinned LAS semantics. |
| `aot_dispatch.h/.c` | Validated two-/four-module registry, mandatory tail dispatch, per-CPU cache, HLE and override precedence. Overrides invalidate all attached CPU caches; HLE calls are never cached. |
| `aot_hooks.h/.c` | Actual-address instruction and function hooks, direct/cached/indirect function-entry coverage, nested original-call bypass scoped to host thread and CPU, optional independent profiling. Build-specific addresses stay outside the core. |
| `aot_libc.h/.c` | Nine generated SDK CPU-call wrappers, with pinned copy/string/comparison semantics. No title maps or HLE symbol-resolution registry. |
| `session_layout.h/.c` | Validated module/system ranges and checked canonical/actual translations. One immutable copied layout per process; no reset or second selection. This validates addresses without mapping memory. |
| `shared_aot.h/.c` | Preflights independent layout/profile/capabilities, selects the layout, loads the descriptor data object through the existing native loader, validates it and binds dispatch. Never calls guest entry itself. |

The new CPU identity is **`MODULE_ABI_CPU_A64_SHARED_66560 = 3`**. It is distinct
from legacy LAS identity2 despite equal size. `excl_hi` is explicitly reserved
and zero-initialized at336 within the old padding. **128-bit exclusive/CASP
helpers are not implemented here.** The field reserves a useful location for
later work; it does not establish Echoes CPU support. Let's Go's upstream
`GAMELIB_ABI3` is unrelated to this CPU identity.

Producer capability bits are instruction hooks1, function hooks2 and profiling4.
The fixtures exercise profiles1,2 and6; the common runtime accepts all supported
combinations only when the independently supplied artifact and required masks
match exactly. LAS's function-entry macro can remain compiled away. CPU size
alone cannot describe that generated-code difference.

## Lifetime and configuration rules

The public setup APIs require caller-serialized configuration while all guest
execution is quiescent. Bind once, initialize the entire CPU, attach every CPU
before execution, and detach it before freeing. Concurrent guest threads use
separate CPUs. This retains LAS's hot cache path without adding a TLS resolver
or epoch check to every dispatch. Hook/profiler registration follows the same
quiescent rule; the volatile generated hook flags do not synchronize live
configuration changes.

`shared_aot_open()` rejects malformed arguments, layout and profile before
consuming a session. Once preflight succeeds, its first attempt consumes that
process even if layout activation, native loading or binding fails. Constructor
reentry reports setup in progress. A retry or title switch requires a fresh
process. Direct low-level layout/dispatch APIs are available for tests and
integration; the production entry path must use the complete pre-entry gate.

The session layout validates six system ranges plus explicit module IDs/bases,
including overlaps, alignment and overflow/underflow. Its hypothetical future
fixture covers a main image larger than64MiB and a different module order. The
**metadata validator still deliberately accepts only the current LAS/Eevee
module orders**; that future layout fixture does not authorize an Echoes
descriptor or implement its startup policy.

## Reproduction and evidence

```sh
python3 tools/check_shared_aot.py
python3 tools/check_module_abi.py
python3 tools/check_module_loader.py
```

The shared checker executes **79 scenarios in each of optimized and ASan/UBSan
host builds**, each scenario in a fresh process where necessary:

- 39 dispatch/execution scenarios:13 cases for each producer profile, including
  cache collisions/isolation, HLE precedence, override invalidation, function
  and instruction hooks, direct calls, nested original calls, TLS bypass
  isolation, profiling, helpers and rejection gates.
- 36 integrated session scenarios:12 per profile, including execution and
  layout/profile/capability/legacy/mapping/metadata/load rejection, malformed
  expected text/digests/order, corrected retries and concurrent setup.
- Three layout scenarios with97/94/93 checks, including boundaries and
  concurrent immutable selection; one libc scenario with111 checks, including
  overlapping copies, exact signed byte differences and bounded strings.

The core hash is checked unchanged across all three profiles within each
variant. Android arm64/API30 cross-linking succeeds for the common core, three
fixture DSOs, all execution/session runners and the layout/libc runners, with
16KiB load alignment requested. **Those Android binaries were not executed.**
The standalone metadata/profile validators pass123 checks per host variant; the
existing native loader's15 scenarios also pass optimized and sanitizer checks.

Ignored `build/shared-aot/verification.json` records core and source hashes and
case output. The checker does not contain or execute game code. Separate
[real generated-code probes](shared-aot-codegen-probes.md) compile12 selected
LAS/Eevee objects against the common header without diagnostics. Their cache
provenance limits, especially the historical LAS manifest gap, remain explicit.

The [complete legacy import audit](shared-aot-imports.md) identifies56 runtime
names across both existing title libraries. The common execution/libc core
supplies15; the remaining41 include OS/trap/timer entrypoints and actual title
helpers. This is a measured integration boundary, not a reason to fill imports
with no-op stubs. Legacy library CPU contracts remain incompatible.

## Next integration gate

Extract the common NSO loader, kernel/services and required title adapters into
one Android runtime, replacing compile-time address constants with the selected
layout. Preserve each title's load, symbol-resolution and initialization policy.
Bind verified producer manifests, original input hashes, generated-code identity,
SIMD/hook requirements and final ELF hash before native loading. The current API
accepts independent trusted expectations; it does not implement that serialized
sidecar verifier or prove metadata symbol ownership/array extents. `dlopen` can
already execute host constructors.

Then rebuild actual title objects/entries in new private cache namespaces and
link them to the same runtime. Incorporate the Android presentation, audio and
UI-idle fixes and converge the required NVN semantics. Only after that host
integration, and owner authorization to end manual play, qualify both titles in
fresh Thor app processes, verify the same loaded runtime bytes and compare
controls, rendering, audio, transitions and save/reload.

The [Echoes source audit](echoes-source-audit.md) supplies future constraints on
module order/size, five-argument SDK startup, 128-bit atomics and writable GPU
memory. It remains source research. No third-title asset import or game build
was performed.
