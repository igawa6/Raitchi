# Original game code execution on Android

This optional execution backend runs original NSO code through native ARM64
blocks, software control instructions and a Unicorn instruction interpreter. It keeps
the shared runtime's existing renderer, services, input and audio. It is an
experimental mixed executor, not an all-native CPU or a performance improvement
claim. The working C-AOT path remains the default.

## Execution boundary

`LARECOMP_CPU_BACKEND=native-blocks` selects the backend before the real game
thread starts. `guest_call` and indirect dispatch enter the original-code engine;
host-only lookup resolves existing HLE and overrides without returning generated
guest code. Original SDK definitions with existing host implementations use the
same host services as imports. Standard memory/string definitions use the
already-tested shared helpers.

The actual game library's executable ELF segments are made non-executable during
the native session. Its validated descriptor stays readable. This catches an
accidental generated-code call instead of silently counting it as native work.
Execution permission is restored only after guest threads have joined, for ELF
finalizers. Current setup still loads that existing compiled library for metadata;
removing this setup dependency is separate from replacing its execution.

Capstone plans conservative blocks of ordinary original instructions. Control
flow, PC-relative addressing, TLS/system operations and atomics end a block.
The gateway saves the host ABI, loads the actual current `Cpu`, executes on its
guest stack, and captures full GP/vector/flags/FP state before restoring the host
stack and ABI. Android X18 stays reserved. A context register is chosen from GPRs
unused by the entire block, so ordinary guest X19 operations can execute directly.
Unsupported register use or exhausted context choices decline before execution.

The engine retains a fixed 256-entry decode cache per actual Cpu. Each hit
checks the full bounded original byte span, including the terminating instruction;
PC, span and current hook limit are part of its key. Host lookup, instruction
hooks and live limits still run before lookup. Cached first-instruction metadata
is independent of the block exit, so an unentered native prefix can fall back
correctly without another decode. Resource-admission failures are never cached.
Optimized and ASan/UBSan cache checks each pass 31,182 assertions. The cache
is included in the Thor game run; no speed advantage is established.

The shared immutable code cache remains capped at 32,768 host pages (128 MiB
on this Thor with 4 KiB pages). Once full, each actual Cpu can reuse 16 private
islands owned by its host thread, with at most four Cpu records per thread.
Only idle private islands are rewritten RX to RW to RX; shared code is never
evicted. Thread exit frees idle private pages. Physical forced-capacity probes
with zero and two shared pages each passed 1,664 native calls across four
threads, with eight guards, zero state/protection failures and zero private
pages left after join. They observed 420 and 412 private rewrites respectively.
Host policy checks also cover rejected reentry and allocation/protection failure
before effects; each forced-capacity variant passes 388 optimized/sanitized
assertions. These fixtures establish buffer reuse, not gameplay performance.

The native host-only lookup uses two fixed 8,192-slot indexes, totaling 64 KiB,
for HLE entries and overrides. Each lookup loads the live function; misses are
not cached, and existing concurrent HLE publication/replacement semantics are
preserved. Default C-AOT dispatch keeps its original path. This removes the
previous per-loop scan of 714 HLE entries and 64 overrides observed in the
running LAS session. Full optimized and ASan/UBSan core checks each pass 112
cases across three profiles, including capacity/collision/late-binding and
concurrent publication tests. A synthetic host lookup benchmark does not predict
whole-game performance; the indexed Android build is being tested separately.

Original PC, branches, nested-call LR, guest TLS, FP status and the 19.2 MHz clock
are handled explicitly. Other supported instructions can use Unicorn with
shared host backing. Those fallback instructions are interpreted, not native.
Mappings are checked against readable host VMAs; writable renderer-tracked heap
views retain the existing host write-fault tracking. Each real guest thread has
its own retained engine context. Host HLE calls and callbacks run on the host
stack and can reenter guest execution without replacing KThread identity.

## Corrections found during startup

The first attempts reached SDK initialization and were killed for memory use.
SDK heap wrappers call the original SDK implementation after optional scaling;
interposing those definitions initially sent that call back into the wrapper.
Repeated scaling produced 8/32/128/512 MiB, 2 GiB and 8 GiB host allocations.
An explicit native-original call now bypasses exactly that host override, keeps
the LR-zero return boundary, and preserves nested bypass state. All nine heap
wrappers use it in native mode; C-AOT retains its original call path.

A later threaded run exposed a separate correctness issue: the bundled Unicorn
translation path disables `CF_PARALLEL`. Its exclusive-store compare/exchange
can become ordinary loads/stores, including a failed-store writeback that can
overwrite a concurrent writer. Single-thread exclusive tests did not establish
cross-engine atomic correctness. Original atomic instructions now use shared host atomic operations, with
unsupported forms rejected before interpreter fallback. Value-based LL/SC remains an approximation of hardware reservations;
no exact exclusive-monitor or ABA guarantee is claimed.

## Evidence and remaining scope

The guarded-stack SDK proof passed on Thor with two real KThreads, 50 returning
SVC calls, 16 TLS reads, four guard pages, 68 host-stack observations, 67 native
stack-canary observations and a genuine ExitThread. The standalone dynamic-X19
gateway passed 1,152 native calls across four threads, including full CPU state,
guarded stack writes, forced temporary X17/X30 and unsafe-mask rejection. These
are diagnostic fixtures rather than gameplay evidence.

The physical host-atomic probe passed 412 checks on Thor, including 40,000
contended increments for both 64-bit scalar and 128-bit pair exclusives,
failed-store preservation and host/guest state checks. Optimized and sanitized
host tests also exercise guest lock operations racing a direct host CAS writer.
These establish the tested cases, not exact hardware exclusive-monitor semantics.

Host checks cover the planner, software controls, dispatch selection, real hook
registry, nested original-call routing and heap passthrough behavior in optimized
and sanitized variants. The actual game run executes original SDK/main code, opens RomFS, creates real
guest threads and initializes NVN. On Thor the host-atomic-fixed build renders
the opening, title, save menu and the loaded Mabe Village world. Link, BowWow,
buildings, vegetation, NPCs and the HUD are visible. A guarded stick gesture
changed the live, identity-checked player collision component position from
X=26.33342 to X=26.44959 while Y=10.5 and Z=126.75 stayed unchanged. This
read-only measurement agrees with the visible facing/camera response; it does
not reposition the player or bypass game execution. The loaded generated game
DSO mappings were independently checked as NX during gameplay.

This first world/movement proof used runtime `a23eb2abde08` and code commit
`5c2dbd4`, with native, software, atomic and Unicorn counters active and zero
native-capacity declines. It observed approximately 0.2 fps in the world, after
long initialization pauses. It is a short execution/input proof, not normal
play performance, broad accuracy validation or a completed game qualification.
The subsequent indexed host-lookup runtime `dfcba7b7ac0c` also loaded cloned
File2 into Mabe and passed movement: X=26.25 to X=26.33833, with Y/Z fixed,
all generated game DSO mappings NX, and approximately 0.2 fps. Indexed lookup
improved the lookup seam but did not remove the per-block architecture cost.
The owner has now authorized [continuous NCE](native-nce-next-step.md) through
real gameplay and a matched C-AOT comparison.

Native sessions use a separate `-native` save directory. Existing reference apps,
owner saves and device performance settings stay outside this experiment.
Shader/pipeline work still uses the common renderer; this backend makes no claim
of eliminating shader compilation or improving graphics accuracy, power or FPS.
