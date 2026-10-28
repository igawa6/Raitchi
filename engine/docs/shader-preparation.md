# Shader preparation and first-use coverage

Implemented on 2026-10-28 in the common Android runtime. Device testing remains
paused at the owner's request; candidate50 is the last verified installation.

Candidate53 subsequently adds an opt-in [direct SPIR-V compiler](direct-spirv.md).
The scheduling/profile work described below remains in place; its performance
discussion records the investigation that preceded that experimental compiler.

## Scheduling

The former Android compiler pool was fixed at three workers. The inspected Eden
source (`raitchi-eden-shell`, commit
`8c22b71a9b1413e26020be576be1f96dc44c7bd1`,
`src/video_core/renderer_vulkan/vk_pipeline_cache.cpp:GetTotalPipelineWorkers`)
clamps a configured Android count to 2–8 and bounds it by hardware threads minus
one. It also has a one-worker workaround for drivers with broken parallel builds.
This source comparison does not establish the settings of the owner's Eden APK.

Raitchi now chooses allowed CPU count minus one, with an Android maximum of eight
and a minimum of one. A process allowed on eight CPUs selects seven compiler
workers. `LARECOMP_VK_JOBS=1..16` remains an expert override. An invalid override
uses the automatic policy. Shader translation remains on one protected thread:
the translator's global string/resource state is not reentrant.

The startup loading gate can use the entire compiler pool. After Continue or
completion, new background preparation jobs are limited to two. Already running
jobs drain; this transition does not interrupt driver calls. Foreground work has
queue priority. Prewarm graphics module compilation now also uses background
priority, so it cannot silently bypass that limit.

Cold portable replay admits up to 16 independent recipe dependencies. It submits
a pipeline only after its program is ready. Builder threads never occupy a slot
waiting for a program that is queued behind them. Fresh owners alone increment
progress and write successful current-cache records. Compute pending-byte pressure
defers admission and retries after existing work completes. The existing strict
cache preload already batches work and remains intact.

## Sources and profiles

Before replay, startup merges exact metadata profiles into the learned discovery
journal. Profiles contain render state, input keys (including integer texture
masks), shader hashes and code/constants lengths. They contain no shader bytes,
host pointers, SPIR-V or device-specific binaries. They are scoped to the same
verified game/version identity as the discovery journal. Explicit wire fields,
record/header SHA256, a 4096-record cap and atomic writes guard the files.

Runtime paths, relative to the selected session unless absolute:

- `cache/shader_profile.bin`: learned metadata beside the journal.
- `<data-root>/shader-profiles/<game-version-identity>.bin`: matching imported or
  learned catalog outside derived caches.
- `LARECOMP_SHADER_PROFILE`: optional explicit profile path.
- APK `shader-profiles/<game-version-identity>.bin`: optional bundled metadata,
  read directly through Android's asset manager without local-file shadowing.

Only a missing exact recipe triggers source inventory. A completely warm profile
does not reopen the NSP or scan files. On a miss, the selected NSP's logical
RomFS is enumerated without extraction; extracted folders are also supported.
The scanner recognizes BNSH blocks in `.bfsha`, `.ptcl`, `.arc`, and verified
`exefs/main` NSO read-only/data segments. Missing archives or unsupported packing
remain coverage misses. Exact reads recheck stage, code/constants partition and
hash before bytes enter the journal. Index/source readers are released before
portable replay starts.

Inventory uses bounded chunks and 64-bit logical offsets. Limits include 1 GiB
of scanned source, 16 MiB per stage, 4096 sources, 65536 stage entries, and 64 MiB
per NSO segment. RomFS metadata is validated before callbacks. Directory sources
do not follow symlinks. Limits retain foreground fallback rather than guessing.
Metadata tables may appear after the file-data region; validation uses the
logical image bounds and rejects any actual file extent that intersects a table.

The current Echoes 1.0.0 catalog contains 475 observed recipes (466 graphics,
nine compute). Host validation resolves all 261 unique exact stage spans from
the original main NSO and NSP, and reconstructs all 475 recipes byte-for-byte
against the learned journal. This covers the captured startup and Castle Town
route only. The cold inventory scanned about 843 MB on the host; first-launch
preparation still costs scanning and compilation, and may lengthen startup while
moving known first-use work out of gameplay.

Candidate55's bounded Thor check started with an empty discovery journal and
no local/global profile. The APK catalog resolved all 475 recipes, then built
192 programs, 388 graphics pipelines and nine compute pipelines before guest
startup. The source/profile phase took 2.369 seconds and replay 20.449 seconds.
The Castle Town fountain/rift save and short movement were visually verified.
After load confirmation, six extra module jobs logged 3 ms combined and six
graphics jobs each logged 0 ms (rounded per-job diagnostics). Settled gameplay
sampled 29.9–30.1 fps with a 45 ms maximum frame and zero new compilation jobs;
the save-loading interval still had a 733 ms maximum frame. This is one scene,
not whole-game coverage or a controlled comparison with candidate53 or Eden.
The selected Balemuni driver reported no GPL extension, so only ordinary-pipeline
fallback was exercised on Thor. The GPL path remains off by default.

Successful journal commits export metadata unions on the writer thread. An
existing same-version profile retains unresolved variants as new learning is
added. Incompatible/corrupt destinations are preserved; valid profiles from other
paths can still seed work. Unchanged metadata is not rewritten. A stale local
profile can produce a warning while the matching version-specific global profile
continues to work.

Host maintenance:

```sh
python3 tools/shader_profile_tool.py export --journal JOURNAL --output PROFILE
python3 tools/shader_profile_tool.py inspect --profile PROFILE
```

Install a validated metadata profile under its reported identity in the data-root
directory, or select it with `LARECOMP_SHADER_PROFILE`. The export tool validates
the original journal and reports recovered prefixes; it never prints shader code.
The optional APK catalog is allowlisted by
`ports/shared-android/assets/shader-profiles/manifest.json`. Packaging validates
the metadata codec, exact game/version identity, count and digest. Runtime asset
reads are bounded before allocation. Imported unresolved metadata is retained
alongside learned profiles, so partial source inventory cannot erase it. Profiles
describe observed scenes; they do not establish complete game coverage. Newly
learned profiles are also produced automatically during ordinary play.

## What this changes during gameplay

A supplied matching profile lets first-launch preparation resolve known shader
code from the owner's files and build the exact variants before guest startup.
Faster batching also shortens rebuilding those variants after an invalidation.
Without a learned/imported profile, a brand-new game still has no pipeline-state
inventory; startup does not invent one. Source code alone cannot determine all
texture types, vertex layouts, attachment formats, blend states or compute sizes.
Unseen variants therefore still compile at first use. Asset streaming and CPU
stalls are separate from shader compilation.

The owner's reported Eden speed advantage is still unmeasured on a matched cold
route. Source evidence shows Raitchi's Maxwell-to-GLSL-to-shaderc/SPIR-V path and
Eden's IR-to-SPIR-V emitter differ. The experimental direct emitter currently
handles a limited vertex-shader subset; all observed fragment work still used
the GLSL fallback in the five candidate53 device routes. Removing GLSL therefore
has not established a material loading improvement. It also cannot be assumed
to dominate. An earlier LAS slow-frame capture documented in
`shader-stutter-followup.md` attributed 3581 ms to driver pipeline creation and
112 ms to shader compilation. That older sample is not a timing of this build
or the owner's latest route. The next performance experiment must time all
stages separately and evaluate driver pipeline reuse/graphics pipeline libraries
as well as front-end translation. An opt-in reusable graphics pipeline library
path is now available with `LARECOMP_VK_GPL=1`. It requires the driver's extension,
feature and fast-linking property; otherwise it uses ordinary pipelines. Four
bounded process-lifetime caches use exact immutable subset keys. Failed library
creation, exhausted capacity and failed links all retry the ordinary complete
pipeline. No draw is skipped. This remains disabled by default pending device
correctness and performance comparison. Pipeline diagnostics now report every
build when `LARECOMP_LOG_PIPE_BUILD` is enabled, including library reuse and link
timing; older slow-only logs cannot provide a complete time breakdown.

Metadata recipes can be reused by another installation of the same game/version;
each device builds its own driver pipelines. Cross-game sharing can be correct
when shader contents and every compilation input match, but no global cross-game
recipe bank is implemented here. A future bank needs complete content validation
and coverage accounting; it cannot apply one game's arbitrary state to another.

Continue releases the UI wait and background work retains ownership. Closing the
window drains preparation before device teardown and cache saving. Source inventory
currently has no UI cancellation flag; shutdown can also wait for its bounded
scan. No required draw is omitted to disguise compilation latency.

## Validation

Focused checks exercise actual production helpers with synthetic data/work:

- Portable replay: real pthread queues, dependency overlap, one-worker liveness,
  deduplication, compute backpressure, failures, cancellation/drain and successful
  strict-cache next-launch reuse.
- Profile wire codec, independent SHA/field parsing, exact keys, scope/corruption,
  union preservation, clean writes and metadata CLI.
- Actual startup bridge with synthetic directory, logical RomFS and main NSO,
  cold imports, warm no-source behavior and exact source negatives.
- RomFS traversal bounds/cycles, chunk-boundary BNSH, offsets above 8 GiB,
  symlink rejection, LZ4 NSO and Android cross-linking.
- Worker policy, loading/gameplay capacity and foreground priority.

Each focused C gate runs optimized and with ASan/UBSan. Synthetic queue timing
proves overlap, not a Thor speedup or an Eden performance comparison. Native/APK
build receipts and per-check results live under
`evidence/shader-preparation-20261028/implementation52/`.
