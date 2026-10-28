# Native game execution: bounded production integration

Historical source audit; the subsequent implementation and device status are in
[the original-code backend report](native-game-backend.md).

This is a source audit of the current shared Android runtime. It proposes the
first native execution of original, meaningful `main`-module code during a real
launcher game session. The isolated SDK/thread helpers establish prerequisites;
their SDK wrappers do not establish native game execution. No build or device
test was performed for this audit.

The shortest practical next implementation is an opt-in native adapter for one
complete, naturally invoked game initializer, installed in the existing dispatch
registry before the real game main thread starts. Keep the working C-AOT SDK
startup, services, NVN renderer and other game functions. Select and audit the
actual initializer from private originals before implementation; no particular
game routine has been admitted by this source-only document.

## Actual startup and interception points

| Location | Existing behavior | Implication for native execution |
| --- | --- | --- |
| `ports/shared-android/runtime/main.c:main` | Loads the selected game descriptor/sidecar, selects LAS version, isolates state, loads settings, creates the UI and prepares the Vulkan guest heap. | Use the real app path. The headless helper omits these systems and cannot establish a native game session. |
| `main.c`, `loader_load` → `kernel_init` → `dispatch_init` | Loads and relocates original modules, records kernel regions, binds HLE guards and LAS instruction hooks. | Configure native admission after these calls, before `kernel_run_main`. Configuration stays quiescent. |
| `main.c:kernel_run_main` | Starts the real KThread at SDK `nn::init::Start`, with thread handle and early/late HLE callback addresses. | Keep this path initially. Starting original NSO offset zero is not equivalent to the supported SDK startup convention. |
| `main.c:run_init_stage` | Early stage initializes SDK. Late stage initializes `main`. Uses `DT_INIT` if present; otherwise calls nonzero `DT_INIT_ARRAY` pointers. Both use `guest_call`. | A deterministic dispatch boundary exists before game initialization. Do not run both `DT_INIT` and its array independently: the former may register TLS and invoke the latter itself. |
| `kernel.c:thread_main` | Sets actual thread-local KThread, installs crash handler, attaches its Cpu, establishes `setjmp`, and enters `guest_call`; detaches and signals on completion. | Production adapters must run on this Cpu. No fabricated `Cpu.thread`, second guest Cpu, or helper-specific kernel path. |
| `native/runtime/aot_dispatch.c:guest_call` | Saves virtual guest LR, sets LR to zero, dispatches, then restores LR. | Retain this API for top-level constructor callbacks. An adapter reached from an ordinary guest call must preserve that caller's existing guest LR convention. |
| `aot_dispatch.c:dispatch_slow` / `rt_dispatch` | Select HLE, dispatch override, then generated code; per-Cpu cache accelerates resolved generated calls. | `aot_dispatch_override` can install a native adapter for a known original entry and invalidates existing caches. Install before threads run. |
| `sources/las-android/tools/gen.py:emit_function` | Known intra-module calls/tails invoke generated C functions directly. Indirect/out-of-module routes use dispatch. | A table/dispatch override does **not** intercept every call to that function. Verify the intended natural startup call uses dispatch. |
| `native/runtime/aot_cpu.h:FN_ENTER` and `native/adapters/las/profile.c` | Function hooks compile only with capability 2; current LAS producer declares capability 1, instruction hooks. | `rt_register_fnhook` is unavailable for the installed producer. Do not promise universal entry hooks without rebuilding that producer. |

The pinned LAS startup commentary explains why SDK constructors run early and
game constructors run after SDK heap setup: `sources/las-android/runtime/main.c`.
The shared adapter preserves this order. `DT_INIT` can register the module and its
TLS template through SDK musl machinery; its execution is not replaceable by a
bare loop over constructor pointers without auditing that behavior.

## Minimal implementable phase

1. Audit a small game-owned initializer reached naturally through a dynamic
   constructor/function pointer during normal startup. Establish an observable
   game-data initialization or other meaningful engine behavior. A generic CRT
   `_init`, SDK wrapper, copied `RET`, or an artificial injected call is
   insufficient. Record original `main` identity, complete routine bounds,
   relocation overlap, exact instructions and generated table/definition
   provenance. Bind the actual on-device game artifact to its producer receipt,
   as with the SDK proof; host/device producer fingerprints may differ while the
   relevant audited units remain identical.
2. Admit a complete finite routine whose first implementation can execute safely:
   no calls, SVC, TLS/system-register access, exclusives, exception operations,
   title hook sites, physical X18/X19 use, stack/frame access, or LR inspection
   beyond `RET`. Require a useful behavior rather than weakening this to an empty
   stub. Audit internal control flow and data accesses. Either reject PC-relative
   instructions initially or relocate each one explicitly with original-PC
   semantics. A constructor that calls other functions requires the nested bridge
   described below; do not silently treat it as an admitted leaf.
3. Add a production native-entry adapter and owned immutable RX code island. At
   startup, opt-in configuration validates the selected session, original bytes,
   CPU3 address and fallback function, then calls
   `aot_dispatch_override(actual_entry, native_adapter)`. A small explicit helper
   called from `main.c` after `dispatch_init` is enough for registration. Keep
   candidate identities in a private profile/manifest rather than embedding game
   bytes or offsets in generic core code.
4. The adapter verifies the actual current Cpu/KThread, checks the unwind safe
   point, enters the audited native routine, transfers its architectural outputs
   back to that same Cpu, restores host state and returns into the original
   C-AOT call chain. A bounded leaf with no observable SP/LR access can use a
   host-backed bridge stack: preserve the virtual guest SP and guest LR instead
   of writing physical host SP or bridge return addresses into Cpu. This is an
   ABI adapter for the admitted routine, not permission to execute arbitrary
   game functions on a host stack.
5. Launch the actual game through the launcher, with native mode explicitly
   enabled. Collect counters for the audited original-main entry, completed
   native returns, actual KThread identity and output validation. Require a
   natural startup invocation and normal app progress into rendered game UI.
   Continue to a meaningful normal game scene with controls when available.
   Record this as **hybrid native game execution**: the admitted original game
   routine executes natively while the surrounding game remains C-AOT.

Use the smallest admitted routine that meets these requirements, then expand
coverage from measured game calls. If the audited startup call reaches the
candidate through a generated direct call, choose a verified dynamic entry or
implement the next bridge. Repeated isolated synthetic probes are not a
substitute for installing this route in the real game process.

Fallback is decided before any native instruction executes. An unadmitted or
disabled entry calls its original generated C function exactly once, using the
descriptor's raw function pointer or `dispatch_lookup`, which bypasses dispatch
overrides. Re-entering `rt_dispatch` at the same overridden address recurses.
Never restart the C-AOT entry after a partially executed native routine: that can
repeat game-state mutations. Likewise, executing a native prefix and then the
original AOT entry is not a valid continuation strategy.

For an initializer with side effects, compare separate fresh AOT/native sessions
or explicitly cloned, audited state. Do not call AOT and native versions twice
on the live game's object graph merely to obtain a differential result. Counters
must distinguish admitted entry, completed native return, pre-entry fallback,
and failure. A successful title screen with zero native game calls fails this
phase's acceptance criterion.

## Nested transitions when the selected game routine needs calls

Original imports resolve in `loader.c` to selected guest exports or fake HLE
addresses registered by `hle_register`. Those fake addresses contain no native
executable function. A native branch into one cannot simply execute host HLE.
Every admitted outgoing BL/BLR/BR path must therefore be known and rewritten or
intercepted before enabling a calling routine.

For native → AOT/HLE, save the complete live guest state and guest SP, move to the
saved host stack, set `Cpu.pc` to the actual guest target and represent BL's guest
LR as its original call-site PC+4. Enter the existing dispatcher/HLE guard. On
ordinary C return, restore resulting guest state and resume the mapped native
continuation. Preserve tail-call behavior separately: an original `BR` is not a
BL followed by an extra return. HLE may itself invoke `guest_call`, so contexts
must support nested AOT → native → HLE/AOT → native calls on the same KThread.
A single shared `before` buffer or one global saved host SP is insufficient.

The helper's `nt_enter`, copied trap island and SDK LR checks are useful reference
code, but its physical X19 context, synthetic site list and one mutable context
are not a general game backend. The original SVC diagnostic PC must be mapped
back to its original game instruction, while native continuation uses the copied
PC+4. A guest function pointer remains a shifted guest address; C-AOT function
pointers and RX island pointers are distinct namespaces.

## Execution permissions, state and failure limits

`platform/android.c:plat_reserve_guest` maps the selected guest window RW,
without execute permission. `loader.c` copies validated relocated images there;
`kernel_init` records guest permissions as metadata but does not make original
text executable. The renderer's `backend_guest_heap_prepare` can own guest heap
mapping decisions. Do not replace the full window or heap with executable
mappings, or assume kernel region permissions equal host page permissions.

Prefer separately owned code islands written RW, instruction-cache flushed, then
made RX. Copied ADR/ADRP, literal loads, B/BL and jump tables retain original
address semantics only after audited relocation. Nearby placement cannot be
assumed: the whole guest window is already reserved and PC-relative ranges are
limited. If code must occupy a reserved-window gap or spare HLE-region tail,
declare and validate its ownership, page boundaries, non-overlap and lifetime
explicitly; do not overwrite module bytes, fake HLE slots, TLS, stacks or GPU
heaps. The existing platform protection enum lacks an executable mode, so RX
islands need an explicit narrow mapping/protection implementation.

Physical Android X18 and host TPIDR_EL0 remain untouched. The current bridge
reserves physical X19 for context. Unrewritten game accesses to either register
are consequently unsafe. Guest read-only TLS uses its real KThread TLR slot;
guest writable TPIDR is a separate Cpu value and is not interchangeable with
that slot. A future arbitrary backend needs correct rewrites and both virtual
values; installing guest TLS into Android's hardware TLS register is invalid.

The native boundary must preserve host callee-saved registers and SIMD state,
host FPCR/FPSR, and the virtual guest NZCV/FP/SIMD/GPR state. Host C calls need
their host floating-point environment while the guest environment is preserved
separately. The prior canary proof's zero FPCR does not establish arbitrary
rounding/FZ/DN behavior. Maintain Cpu's opaque kernel binding, TLR, exclusive
bookkeeping and dispatch cache. Do not claim exclusive-monitor continuity across
host callbacks; reject that instruction class until implemented.

`kernel.c:rt_unwind_point` and SVC ExitThread longjmp to the production
`thread_main` setjmp. No host mutex, cleanup-owning C++ frame, temporary registry
mutation, or stale per-thread native-context pointer may survive such an unwind.
A bounded no-call leaf can check safe points before entry and after return;
unbounded native loops need an actual interruption strategy. Native C++ exception
unwinding through generated C/host HLE is unsupported and must not be admitted by
assumption. Keep code/context allocations alive until actual host thread joins.

Fault reporting should identify the copied native PC and its mapped original
main-module instruction. A stale `Cpu.pc` naming the previous AOT entry does not
identify a native fault accurately. First-phase unsupported inputs or mapping
failures select C-AOT before entry; execution faults abort the isolated game
session with diagnostics rather than pretending fallback repaired its state.

The implementation touchpoints are `main.c` for quiescent configuration,
`aot_dispatch_override`/descriptor functions for route and fallback, a new native
adapter/assembly boundary for the admitted routine, and private candidate/producer
evidence plus counters for acceptance. Loader, kernel, renderer and original
producer behavior can remain unchanged for the admitted no-call phase.
