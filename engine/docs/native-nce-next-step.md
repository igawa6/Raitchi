# Next CPU step: continuous native execution

This is the historical design audit of the earlier native-block backend.
The implemented continuous backend and its current validation are described in
[Raitchi and continuous native execution](raitchi-continuous-nce.md).

The owner's comparison with Eden is correct: our current experimental backend
does much more host work than Eden's NCE. Reaching Mabe proves original game code
can execute through our runtime; roughly 0.2 fps does not establish a practical
CPU backend. Further lookup/cache tuning cannot remove its basic cost: returning
to C, materializing architectural state, planning and dispatching after short
native prefixes and interpreted instructions.

This read-only audit uses the official Eden mirror, pinned to
[`305f40100d76ec57b13c2f03eafd490db83691cb`](https://github.com/eden-emulator/mirror/commit/305f40100d76ec57b13c2f03eafd490db83691cb).
The mirror links to the official
[Eden repository](https://git.eden-emu.dev/eden-emu/eden).
No Eden build, production edit or device operation was performed.

## What Eden actually does

| Concern | Pinned source behavior | Difference from our current engine |
| --- | --- | --- |
| Native execution duration | `ArmNce::RunThread` enters guest execution and returns for a halt/SVC/interrupt. Ordinary branches, calls and loops remain native; there is no fixed 64-instruction host-dispatch interval. | Our block planner and context gateway run repeatedly during ordinary game computation. |
| Code preparation | `Patcher::PatchText` scans module text before execution; `RelocateAndCopy` resolves near-branch patch islands. Loader integration reserves patch sections, and process loading gives text/patch sections host RX permissions. | We currently copy individual safe prefixes into separately cached executable islands. |
| SVC | A patched SVC saves guest state and original PC+4, restores host SP/TLS and host registers, and returns a supervisor-call halt. A post-handler trampoline restores guest state and resumes. | Our real `rt_svc(Cpu*,...)` can remain the service endpoint, but needs a long-run native exit/resume adapter. |
| Guest TLS | Guest-mode hardware TPIDR_EL0 points to a private native-control structure. Patched guest TLS reads/writes access its shadow TPIDR fields. Assembly restores Android TPIDR before entering host C. | Our current bridge preserves Android TLS throughout and reserves/chooses a context GPR for each block. |
| Counters | The patcher replaces CNTPCT and CNTFRQ reads; counter scaling is emitted as native arithmetic using a precomputed factor. | A clock read need not enter C for every occurrence. This source does not show an equivalent CNTVCT patch; our adaptation must preserve our own counter contract explicitly. |
| Exclusives | Original exclusive instructions remain hardware instructions; the patcher upgrades their ordering for the original A57 assumption. | No Unicorn atomic lowering or per-instruction host CAS dispatch is involved in normal NCE execution. |
| Stack/registers | Guest SP is the actual hardware SP during guest execution. Full guest GPR/vector/FP state is saved at exits; there is no permanently reserved X19 context register. | Our block bridge repeatedly switches/materializes these states. |
| Interrupts/faults | Signal entry assembly recognizes guest mode and restores host TLS before C handlers. Saved `ucontext` carries the guest state; signals can stop a running loop. Alignment faults use a targeted instruction interpreter. | Removing block safe points requires a real asynchronous return mechanism and integration with our existing dirty-page/crash handler. |

Source anchors: [run/interrupt/context lifecycle](https://github.com/eden-emulator/mirror/blob/305f40100d76ec57b13c2f03eafd490db83691cb/src/core/arm/nce/arm_nce.cpp#L198),
[entry and signal assembly](https://github.com/eden-emulator/mirror/blob/305f40100d76ec57b13c2f03eafd490db83691cb/src/core/arm/nce/arm_nce.s#L15),
[patch classification](https://github.com/eden-emulator/mirror/blob/305f40100d76ec57b13c2f03eafd490db83691cb/src/core/arm/nce/patcher.cpp#L101),
[SVC exit/resume](https://github.com/eden-emulator/mirror/blob/305f40100d76ec57b13c2f03eafd490db83691cb/src/core/arm/nce/patcher.cpp#L486),
[TLS and clock patches](https://github.com/eden-emulator/mirror/blob/305f40100d76ec57b13c2f03eafd490db83691cb/src/core/arm/nce/patcher.cpp#L594),
[exclusive ordering](https://github.com/eden-emulator/mirror/blob/305f40100d76ec57b13c2f03eafd490db83691cb/src/core/arm/nce/patcher.cpp#L370),
[NSO patch loading](https://github.com/eden-emulator/mirror/blob/305f40100d76ec57b13c2f03eafd490db83691cb/src/core/loader/nso.cpp#L179),
and [host executable permissions](https://github.com/eden-emulator/mirror/blob/305f40100d76ec57b13c2f03eafd490db83691cb/src/core/hle/kernel/k_process.cpp#L1282).

## What to adapt

Adapt the CPU-side architecture while keeping our real KThreads, loader identity
checks, Horizon service implementations, title instruction hooks and NVN/Vulkan
renderer. No GPU-emulator transplant is required for this CPU change.

The useful pieces are the instruction classifiers, ahead-of-execution patch and
relocation machinery, guest/host context layout, long-run entry/exit assembly,
and guest-mode signal entry. Their current implementations depend on Eden's
`Kernel::KThread`, `KProcess`, `ArmInterface`, `System`, memory manager, Oaknut and
clock utilities. They are design/reference components, not a library that can be
linked unchanged into our C runtime. The inspected files also carry GPL license
tags; literal source reuse must retain the applicable licensing and attribution.

Our integration additionally needs these explicit adaptations:

- Patch existing HLE/override function entries and fake HLE targets to exit into
  our `Cpu*` service ABI. Ordinary native function calls must not consult the
  host lookup index. These HLE exits are specific to our runtime and are not
  supplied by Eden's SVC patcher.
- Patch every registered title instruction hook while preserving its original,
  skip and branch-target semantics. Native execution must not bypass the LAS
  fixes currently applied by the generated instruction hooks.
- Keep fixed selected module/data addresses. Our producer profiles and relocated
  pointers cannot silently acquire Eden's inserted-pretext base shift. Reserve
  explicit nearby patch storage, within ARM64 branch range, without colliding
  with modules, heaps, TLS or renderer mappings.
- Restore Android TLS, host SP and host X18 before any existing C HLE, signal
  handler or unwind path. Eden's displayed host context saves X19–X30; it is not
  sufficient evidence to omit our explicit host-X18 preservation requirement.
- Chain GPU dirty-page handling and crash diagnostics through a guest-aware
  assembly signal entry. Preserve original-PC mapping and genuine fault
  reporting. Do not adopt Eden's permissive failed-data-fault instruction skip
  as our correctness policy.
- Preserve nested HLE → `guest_call` → native execution on the same actual Cpu,
  and ExitThread unwinding to our real `thread_main`. Keep invocation frames and
  code alive until host-thread joins.

The existing `GuestContext` and native-control structure make the dependency
boundary concrete:
[guest context](https://github.com/eden-emulator/mirror/blob/305f40100d76ec57b13c2f03eafd490db83691cb/src/core/arm/nce/guest_context.h#L15),
[per-thread native parameters](https://github.com/eden-emulator/mirror/blob/305f40100d76ec57b13c2f03eafd490db83691cb/src/core/hle/kernel/k_thread.h#L669),
and [fault policy](https://github.com/eden-emulator/mirror/blob/305f40100d76ec57b13c2f03eafd490db83691cb/src/core/arm/nce/arm_nce.cpp#L114).

## First integration test

Implement a separate opt-in continuous-native module mode. Scan and patch the
actual selected SDK/main text once, preserve its original address layout, and
enter the real production KThread at the existing SDK startup entry. Let normal
branches and nested calls run on original executable ARM64 pages until a patched
SVC, HLE, title hook or interrupt boundary. Reuse the existing service/renderer
calls after restoring host state.

The first production gate should repeat the already reached path: normal SDK
startup → game initialization → title → the cloned Mabe save → rendered movement.
Measure host exits per frame, time between completed frames, native run duration,
and unsupported/fault counts. Require a substantial reduction in CPU exits and
interpreter use before attributing any speed improvement. Validate stop/resume,
blocking HLE, nested callbacks, virtual TLS, FP state and concurrent native
exclusive operations as part of that run. Keep the original C-AOT installation
and current block backend selectable for comparison.

Do not mix patched text into the current Unicorn/block engine casually: it reads
instruction bytes from the same original-address mappings. It would see branch
patches rather than original SVC/TLS instructions. Retain an original-word escape
map for an intentional mixed fallback, or select the existing backend in a fresh
process. Neither a copied leaf nor another microcache benchmark establishes the
continuous-execution design's benefit.
