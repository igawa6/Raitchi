# Shared Android Switch recomp platform

Approved direction, 2026-10-24. Primary hardware is the owner's AYN Thor. Initial two-title candidates are Link's Awakening and Let's Go Eevee, one verified executable revision each. The first release must execute both against the identical runtime binary. Support for arbitrary Switch games is a longer-term ambition, not an initial acceptance claim.

On 2026-10-25 the owner resumed this recomp-launcher direction after reviewing
an NCE/NVN alternative. Keep the existing AOT references and continue the shared
launcher/runtime work. NCE remains deferred; shader preparation is not an
exclusive advantage over Eden and no performance advantage has been established.
Host-side contract/loading work may proceed while the owner tests a reference;
shared-runtime device acceptance still requires both reference gameplay routes.

## Active follow-up, 2026-10-25

The owner reports shared Eevee running fine and explicitly authorizes closing it
when Thor is needed. Continue the agreed order: shared LAS execution against the
same runtime, native Android library/setup shell with on-device generation and
compilation, then Echoes as the third-title architecture test. Do not infer full
route qualification from the owner's general report; record device observations
and remaining gaps separately. The new session may be interrupted for this work.

Use bounded parallel tasks: title staging/input audit, compiler toolkit, portable
producer, with the coordinator owning Android integration and device sessions.
Existing reference installations and saves remain read-only. New builds publish
immutable library/sidecar pairs and preserve the previous working build.

## Hardware evidence

Read-only ADB inspection: AYN_Thor; Android 13; QCS8550; 16 GB-class physical RAM; 4096-byte pages. Current landscape displays are 1920x1080 and 1240x1080. Display IDs and modes must be discovered dynamically. Actual renderer runs verified Adreno740 with the stock Vulkan1.3.128 driver and pinned Turnip, with different correctness limits in the two references. Initial qualification applies only to this actual Thor, not all Thor variants or Android devices.

## Execution policy

Retain the existing ARM64 AOT path. Develop with host cross-compilation, then generalize the existing on-device builder. Separate Android shell, build service, fresh game process, common runtime, title adapter and generated local game library. Do not attempt session reset with dlclose. New NCE, frame-generation improvements, high refresh patches, game-specific second-screen content and unrelated runtime families follow the two-title proof.

Title adapters own executable/build identities, entrypoints, module layouts, SDK differences and game-specific patches. Generic services, graphics translation and audio stay in the common core. Record runtime ABI, CPU-state layout, generator/toolchain/flags and game inputs in each generated artifact; reject mismatches before loading. No title-specific copied renderer/service implementation counts as runtime sharing.

## Gates

1. **Pinned audit:** snapshot all three sources; compare execution ABI, SDK/graphics strategy, hardcoded addresses, modules, dependencies and license obligations. Inventory accessible owned inputs. Exit with explicit compatible differences and build prerequisites.
2. **Thor references:** reproducible standalone LAS Android baseline, then minimal standalone Let's Go Android reference. Qualify movement/combat or capture, area changes, menus, audio and save/reload. A title screen or successful compilation is insufficient.
3. **Contract:** specify package identity, memory and service capabilities, patches and cache identity against both actual implementations. No unversioned CPU structure contract.
4. **Shared core:** extract LAS's reusable code, then integrate Let's Go. Both load the same hashed runtime artifact and pass their reference routes. If incompatible, document failure and revisit the second-title choice rather than hiding two runtime copies.
5. **Thor shell:** game on upper display, lightweight session controls below. Correct controller focus while touching lower display. Ten alternating launches per title; sleep/resume, process death, audio/surface recovery and saved-data checks.
6. **Setup and upgrades:** SAF import, managed native-readable assets, internally stored generated libraries, resumable memory-aware compilation, cancellation, low-space handling and atomic artifact publication. Preserve previous working builds and keep saves independent.
7. **Qualification:** compare unchanged baselines, shared host and compatible emulator/NCE. Same title revision/save/route/resolution/settings/cache conditions and hardware power/fan/brightness. Frame generation disabled. Three matched runs plus a 30-minute soak per title. Proposed limits: <=5% median/p95 frame-time regression, <=10% p99 regression and <=15% peak-memory increase, with variability measured before acceptance. Correctness is mandatory. Trial an upstream update and rerun both games before expanding.

## Current work allocation

- Coordinator: workspace, upstream lock, verification utilities, integrating findings and first build checks.
- Runtime audit agent: source and ABI audit; sources read-only.
- Thor inventory agent: read-only hardware, existing installations and available inputs; local private evidence only.
- Android build agent: dependency/toolchain audit and minimal baseline build recipe; no expensive build or download.

Each task is bounded and owns separate files. Reuse local dependency caches, avoid overlapping scans, and do not benchmark or launch a game until its exact baseline setup is established.

## Evidence rules

Source documentation is not a device test. Record precise commands, hashes and outcomes. Do not publish game files, keys, saves, local generated code or private device details. Keep private evidence outside Git. Status must distinguish source-compatible, compiled, booted, route-tested and qualified. Missing input or dependency should be recorded explicitly; never substitute a launcher mockup for an execution milestone.

## Decisions deferred until evidence

- Exact minimal module ABI and size of the source extraction.
- Let's Go Android port complexity and sustained Thor performance.
- Stock driver sufficiency versus a separately qualified driver.
- Full setup build time/storage and memory limits.
- Whether a later NCE backend gives a useful gain after profiling.

## Sources

- https://github.com/Nycolazs/LASRecomp-Android-Plus
- https://github.com/peiikai/LASRecomp
- https://github.com/raulzrrs/letsgo-recomp

Exact revisions are in `../sources.lock.json`.

Owner-supplied follow-up projects, including BOTWRecompAndroid, are recorded in
[future engineering references](future-references.md). They inform later audits
without changing the initial two-title acceptance gate.

## Authorized verification and native CPU proof, 2026-10-25

The owner explicitly approved the proposed next steps and parallel agents. End
the prior protected LAS session for these tests; preserve both references and
snapshot the shared saves. The current baseline is the two-title common runtime
recorded in the private ready-to-test receipt, with Turnip26.2R5/static vertex
input/normal render thread for LAS and stock/static vertex input for Eevee.

The coordinator owns Thor: complete a real LAS build through the installed
foreground service, measure wall time/process memory/storage/battery temperature,
verify and launch its published library, then repeat with Eevee if LAS succeeds.
Run controlled gameplay/save and lifecycle routes, recording observed behavior
separately from owner reports. Do not update the APK during compilation.

Bounded parallel work: one agent implements an asset-free Android native ARM64
bridge proof (TLS/x18, callbacks, threads/signals, live state and atomics); another
fixes the two proven shader-cache identity gaps with focused regression checks.
Keep the C-AOT path working. The synthetic native proof is not a full game
backend, performance comparison or general instruction-rewriting guarantee.
A later optional LAS native backend and matched comparison depend on this proof;
Echoes remains deferred until the baseline and execution-backend decision.

The owner subsequently requested faster multicore compilation and native work in
parallel. Add a bounded four-worker policy without invalidating verified objects;
validate cancellation/resume across the APK update. Keep the producer immutable
and retain original object provenance when only Android's installation path changes.
The next bounded native proof compares an actual SDK memory/branch function against
CPU3 with guarded read-only buffers; it does not enable native game execution.

Native ARM64 execution is the same CPU principle as emulator NCE: execute original
ARM64 and intercept OS-sensitive operations. It removes this project's ARM-to-C
build, rather than establishing a CPU advantage over Eden's NCE. The distinct
experiment is the surrounding SDK/NVN HLE boundary; GPU shader translation and
pipeline compilation remain necessary. Speed, power and accuracy advantages
require matched measurements. The owner authorized connecting the prototype to
the actual CPU3 production HLE in parallel, starting with GetSystemTick through
rt_svc before stateful guest-thread or SDK startup integration.

Current verification outcome: both real on-device title builds booted identical
final runtime bytes; four-worker compilation and 303-object APK-update reuse were
observed. LAS passed a short Mabe Village movement/menu/manual-save/reload and
home/sleep-resume route. Eevee loaded the existing save into Oak's lab at 30 fps.
The native production SystemTick bridge passed all 65 state/clock transitions.
Normal game execution remains C-AOT. The subsequent isolated diagnostic passed
on two genuine production KThreads: distinct guest TLS slots, current-thread
identity, 48 returning SVC calls, 16 TLS reads, normal return, actual ExitThread
unwind, and completed host-thread joins. Native state and opaque CPU checks
reported zero failures. The installed launcher/runtime stayed unchanged; this
test used a separately staged diagnostic core in a fresh helper process. See
[native thread integration](native-thread-bridge.md).

The subsequent [original SDK wrapper gate](native-sdk-thunk.md) also passed:
two native nested wrapper calls returned correctly, matched the actual C-AOT
descriptor entries on their real guest threads, and retained all state/identity
checks. Its gateway targets copied SVC PC+4 while the original RET uses the
nested LR. The actual installed game library is bound to the audited generated
units through its published producer receipt and verified object records. The
default synthetic mode passed again in a separate fresh process.

The next bounded native gate is a guarded guest stack: native nested calls use
guest SP, HLE runs on the host stack, and both normal return and ExitThread must
preserve the host lifecycle. Full SDK/IPC startup, general original-PC mapping,
and a complete native register ABI remain separate requirements. These
diagnostic fixtures establish neither native game boot nor a speed/power
advantage.


The owner subsequently requested continuation until actual game execution. The
original-code backend now starts the real SDK/main threads and renders LAS's
opening, title and file menu with generated C-AOT code NX guarded. It retains
native blocks, software controls/host atomics and Unicorn fallback, and still
uses the compiled descriptor for setup metadata. A cloned save keeps the owner
save isolated. The next short gate passed: the cloned Mabe autosave loaded a visibly rendered
world and a stick gesture changed the live player position, with generated
code mappings NX. Initial world performance was approximately 0.2 fps. Bounded
validated decode/overflow caches are active; the subsequent indexed host-lookup
build is being tested separately. Normal playable performance and wider
accuracy remain open. The working C-AOT default remains available, with no speed/power
advantage established. See [current backend](native-game-backend.md).


The owner explicitly requested "do until finish using continuous nce". Implement
an opt-in continuous-native mode based on the audited Eden execution model:
original executable module addresses, ahead-of-execution SVC/TLS/clock/HLE/hook
patches, long native branches/loops, real guest SP, and full state transitions
only at exits/interrupts. Keep the current HLE/NVN renderer and default C-AOT.
The next gate is real SDK startup through cloned Mabe rendering/movement using
this mode, followed by matching the working C-AOT comparison conditions on Thor.
Performance parity is an acceptance target, not an established result. Preserve
MrPurple T30 as the owner-selected app-wide driver. See the bounded component
and integration plan in [continuous NCE](native-nce-next-step.md).
