# Raitchi and continuous native execution

Raitchi provides one Android library and settings interface for the existing
shared runtime. The package identifier is retained so installed games, builds,
saves and graphics drivers survive the rename. The app follows the device's
system theme with pink accents and an original lychee/controller icon.

## Execution modes

| Mode | CPU execution | Current role |
| --- | --- | --- |
| Continuous NCE (`continuous-nce`) | Original module text executes at its guest addresses; patched service, TLS, counter and compatibility sites return to the host | Launcher default at the owner's request; compatibility testing continues |
| Recompiled (`aot`) | Previously generated ARM64-to-C code compiled for Android | Selectable alternative; existing working path retained |
| Native blocks (`native-blocks`) | Short native prefixes with software control/Unicorn fallback | Diagnostic backend; not exposed as a normal launcher choice |

Continuous mode no longer returns to a software dispatcher for ordinary ARM64
branches, calls and loops. Hardware exclusives execute natively with ordering
upgrades. Invocation-owned assembly contexts preserve all guest GPR/vector/FP
state, restore Android TLS, X18 and stack before host C, and support nested
callbacks. TLS and counter patches currently still use full host exits; this is
less optimized than Eden's inline patches.

The retained LAS/Eevee extracted-folder preparation path still loads the compiled game library for verified
descriptors and setup metadata. Its generated executable segments are made
non-executable during native gameplay. This establishes original-code execution;
it does **not** yet remove the initial game preparation/compilation dependency.

The new native-only setup path accepts original module metadata without a game
DSO or generated function tables. Echoes passed the real-file host loader
preflight and loaded a controllable Suthorn Ruins scene on Thor through this path;
its graphics limitations and remaining device checks are tracked separately in
[the Echoes bring-up report](echoes-native-bringup.md). The launcher import flow
now also offers [direct NSP/XCI native preparation](direct-nsp-native-loading.md), which avoids game C compilation and full RomFS extraction. Echoes has passed that path on Thor; direct-package LAS/Eevee gameplay still needs device qualification.

Both CPU modes use the same NVN-to-Vulkan renderer. Native CPU execution does
not eliminate shader translation, Vulkan pipeline creation or their first-visit
stalls. Driver- and renderer-keyed caches reduce repeated work.

## Integration and lifecycle

The planner patches selected original module text before threads start. Validated
nearby islands preserve module bases; descriptors become read-only, code becomes
RX, and publication failures roll back. All fake HLE entries exist before later
startup callbacks register their implementations. Static call-return resumes and
separate bounded per-Cpu caches avoid rewriting code executing on another CPU.

Real KThreads use the shared kernel, services and HLE implementations. Linux
signal entry restores host TLS before calling C. On Android, the assembly front
is installed through `sigaction` resolved from `libc.so`: ART's interposed signal
handler otherwise reads private guest TLS first. The previous ART action is
chained after host state is restored. Render workers use the same installation
wrapper so they cannot overwrite the process-wide front.

The design was checked against the pinned Eden/yuzu architectural references in
[the NCE audit](native-nce-next-step.md), including
[Android signal interposition](https://github.com/eden-emulator/mirror/blob/305f40100d76ec57b13c2f03eafd490db83691cb/src/common/signal_chain.h).
The new adapters are independently implemented. LAS compatibility replacements
retain attribution to the upstream MIT-licensed patch implementation.

Native saves/settings use a separate `-nce` namespace. The launcher seeds its
saves once from the ordinary session and retains later native progress separately.
A PID-bound stop request asks the window loop to stop and join KThreads; Android
then completes its normal SDL Activity/service teardown. A paused SDL Activity is
resumed to process a launcher stop request. Force close remains an explicit user
action after a timeout.

## Compatibility and graphics findings

Original execution must also apply required generator compatibility patches.
LAS now has selected-version opcode checks and replacements for frame steps,
frame interval, dynamic resolution, camera/aspect setup and selected safety fixes.
Optional randomizer/cheat generator changes are not advertised as native support.

An early CPU crash left the custom driver's startup marker pending. Subsequent
runs silently fell back to Qualcomm's system driver and showed dark/checkered
LAS graphics. Re-enabling the owner's verified MrPurple T30 restored bright
rendering. Qualification now checks the actual loaded driver in logs and process
mappings, rather than inferring it from the installed ZIP or preferences.

Eevee exposed a shared texture-cache bug: a tracked write consumed its generation
but could still be rejected by a sparse sample hash. A small glyph update outside
sample positions then remained stale. Tracked dirty pages now force a full hash;
unchanged hashes avoid uploads and clean textures retain their fast path. The
regression reproduces the old failure for both hash implementations, including
an atlas larger than the periodic hash budget.

The inherited Android renderer
halved all square 2048 depth targets, an optimization originally qualified for
LAS's sun shadow map. The default is now limited to LAS; other titles retain
their original depth-map size, matching the upstream Eevee policy. The owner
confirmed that Eevee's textures and shadows were fixed while testing normal
texture caching and the corresponding full-size-shadow setting. This does not
disable shadows or bypass Vulkan comparison-sampling validation.

## Validation boundaries

Host checks cover the patch planner, transactional image publication/rollback,
context layout, signal routing, real registry enumeration, runtime orchestration,
LAS replacements, settings/save isolation and sparse texture updates. Sanitized
runs accompany the native C checks. Synthetic tests do not establish game support.

On the owner's Thor, physical context and combined signal/image probes passed,
including uninterrupted loops, full register state, nested calls, guarded stacks,
real dirty-page retries, asynchronous returns and render-worker registrations.
Actual LAS original-code gameplay and movement reached Mabe with generated code
NX-guarded and the T30 driver verified. Thirty-fps gameplay was observed; selected
sixty-fps village runs are observations, not a matched maximum-performance claim.
The owner completed a physical-control quit; the kernel reported 44 threads
stopped in 4 ms. Eevee original-code execution reached the laboratory battle at
30 fps before the texture-cache correction. The corrected default-cache route
and full-size shadows subsequently received owner confirmation on Thor. Small
LAS black fragments remain a separate open graphics issue. Captures show hard
fragments at paving/grass edges in the first opaque scene colour pass, before
blur/compositing. Disabling the half-register optimization did not remove them.
The owner also reported remaining fragments during a depth-tolerance diagnostic;
the active driver changed during that comparison, so it is not a matched causal
test. The investigation was deferred at the owner's request to prioritize Echoes.
Diagnostic shader flags were removed from future launch settings.

No whole-game compatibility, thermal advantage, universal Switch compatibility
or absence of shader stutter is claimed. Both supported titles retain the existing
game/version validation and preparation flow. Detailed private logs, saves and
device identifiers remain in ignored local evidence.

An already-release single store-exclusive whose status and data operands both
name the zero register remains bit-identical and executes on the host. Arm
classifies this overlap as constrained unpredictable even for register 31;
Raitchi does not reinterpret it as a valid-operand exemption or invent a store
result. Unordered and other overlapping forms remain rejected. This preserves
the original hardware instruction but does not establish identical constrained
behavior across processors; an actual execution fault requires separate diagnosis.

Original NPDM system-resource metadata now configures the generic kernel before
SDK startup. A zero pool keeps the existing disabled physical-mapping policy;
nonzero pools are bounded by the NPDM maximum and charged separately from user
physical pages. GetInfo 17 reports this HLE's allocated page-inventory metadata,
not an exact Horizon slab/page-table allocation or the amount of mapped memory.
The entire declared pool is reserved against the process's physical budget.

Physical maps use a serialized inventory of the owned alias reservation. Mapping
preserves existing normal pages, zeroes new holes, rejects foreign mappings and
checks the combined heap/physical budget. Partial unmaps release backing while
retaining the virtual reservation; QueryMemory reports normal and free runs.
This implementation enables that path on 4 KiB host pages and rejects enabled
VAMM on larger host pages until subpage protection/backing semantics are added.
The preexisting process memory allowance and fixed code/stack estimate remain;
this does not claim complete Horizon memory-resource accounting.

Validated owned-alias backing/protection failures terminate the process: host
MAP_FIXED or multi-VMA protection failures cannot promise reversible effects.
Controlled callback-failure tests prove inventory rollback only for callbacks
that explicitly guarantee failure atomicity. Reset releases alias backing and
restores its initial read/write reservation before clearing process resources.

Generic loading now supports RELR and GNU-hash metadata, with an owned lead-in
for NCE banks and versioned preparation migration. See
[generic native loading](generic-native-loading.md) for bounds and scope.
