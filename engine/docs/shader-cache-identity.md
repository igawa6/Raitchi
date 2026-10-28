# Shader cache identity

The shared renderer separates replay compatibility from the driver's opaque cache.
`PipeKey` is an application ABI: dynamic vertex input, for example, omits static
vertex fields. Its cached records therefore need stronger identity than the
Vulkan implementation-managed blob.

`exec.c` now writes **LHS7**, retaining exact header/key sizes, complete record
bounds and bounded shader/constant content checks. Its SHA256 identity includes:

- A build-derived full renderer fingerprint. `gpu_cache_fingerprint.py` scans the
  actual renderer compile commands with the real compiler's `-M`, hashing all
  compiled NVN sources, transitive headers/generated shader includes, relevant
  Vulkan/SHA helpers, semantic compile options, compiler binary/version and
  shaderc/NDK/static support archives plus build configuration/tool sources.
- Verified original game module hashes/build IDs, title/program IDs and canonical
  guest module layout; generated AOT code and CPU implementation are excluded.
- Vulkan UUID, vendor/device/driver/API, heap layout, all previous translation and
  rendering modes, plus resolved vote/storage-image-write/clip-distance support.
  The headstart identity also includes the resolved production graphics pipeline
  library mode, independently of the older diagnostic probe mode.
- `LARECOMP_*`, `TU_*`, `MESA_*` and `VK_*` environment entries. Ordering is ignored;
  absent/empty differ; duplicate relevant names or more than 256 entries disable
  persistent caching because `getenv()` ordering would otherwise be ambiguous.
  The narrow diagnostic-only allowlist `LARECOMP_SAMPLING_TRACE`,
  `LARECOMP_LOG_COMMAND_POOLS` and `LARECOMP_FONT_TRACE` is counted and checked
  for duplicate names but excluded from the digest: these flags only add bounded
  trace output/metadata and do not change shader translation, renderer state or
  selected assets. Other flags, including unknown `LARECOMP_*` names, remain hashed.

CPU-only kernel/audio/NCE implementation and UI changes no longer invalidate
headstart. Shared declarations or actual renderer/compiler/config changes still
invalidate conservatively. The generated identity header excludes itself to
avoid a digest cycle; its generator is included. Missing/bootstrap identity
fails closed: headstart recording/replay and SPIR-V disk caching stay disabled.
The native build receipt records the helper and every input digest; packaging
recomputes dependencies and identity. Receipt verification intentionally binds
absolute local provenance paths, even though fingerprint inputs use canonical
source/generated/toolchain names.

SPIR-V source keys retain stage, source, target API, postprocessing and the exact
existing optimization-presence semantics, and now include a separate compiler
fingerprint covering `shaders.c`, its dependencies and the linked shaderc archive.
Keys retain their existing 64-bit hash format. Existing SPIR-V payload checks are
minimal (length/magic); broader bitrot recovery is outside this change.

Opaque `VkPipelineCache` data carries no application `PipeKey`. It now uses a
**GPC2** sidecar with ABI/API, device/vendor/driver/UUID, environment, exact blob
length and full SHA256. The standard 32-byte little-endian Vulkan v1 header must
match vendor/device/UUID. A sidecar/header/length mismatch is rejected before
blob allocation; full payload integrity is checked after reading. Renderer and
translator changes do not invalidate this blob. Standard behavior is documented
in [Vulkan's pipeline cache specification](https://docs.vulkan.org/spec/latest/chapters/pipelines.html#pipelines-cache-header)
and [VkPipelineCacheCreateInfo](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineCacheCreateInfo.html).

The shipped beta_4 legacy tag may be imported only with its exact GPU/driver/UUID
tail and a matching Vulkan header. Legacy had no environment/API/integrity fields;
this one-time migration adds those guards only after successful Vulkan import,
without rewriting blob bytes. Any driver-rejected blob and its sidecar are removed
before retrying an empty cache. New blob and sidecar writes each use temporary
files and rename; a crash between renames leaves a digest/length mismatch and
safely starts empty. This is fail-closed persistence, not an atomic two-file swap.

LHS6 and earlier headstart formats are intentionally rejected once for LHS7;
there is no unproven legacy replay migration. This and the compiler namespace
change can cause one cold application preparation even while a compatible opaque
driver cache survives. Never-seen pipeline states can still compile on first use.

Run `tools/check_gpu_cache_compat.py` and `tools/check_shader_cache_identity.py`.
The first uses actual compiler dependency scans and production metadata/header/IO
and SHA code: CPU/UI mutation stability, renderer/header/toolchain invalidation,
missing-dependency errors, disabled bootstrap identity, device/env/header/length
rejection, diagnostic-toggle stability, active renderer/driver toggle invalidation,
duplicate rejection (including ignored diagnostic names), the 256-entry bound,
all-byte corruption/truncation, deferred legacy migration and failed sidecar
publication. Allocator instrumentation checks wrong/missing sidecars and sparse
length mismatch are rejected before allocation. Each optimized/ASan/UBSan
variant passes the assertions reported by the checker plus three bootstrap assertions. The
headstart/SPIR-V harness passes 7,753 headstart and 120 SPIR-V assertions per
variant. These are host behavior checks with explicit GPU/shaderc doubles, not
on-device driver timing or gameplay validation.

Historical evidence follows; older schema/runtime-file identity descriptions
below refer to those specific earlier candidates.

On 2026-10-25 the final common runtime was rebuilt, packaged and exercised in
four fresh Thor game processes with the on-device-built LAS library. Its LHS3
header stayed identical across the same static-vertex profile, changed when
dynamic vertex input was enabled, and returned to the original identity when
static mode was restored. Startup logs recorded incompatible-cache rejection
on mode changes. Visible opening video remained at 30 fps with the owner's
selected newer Turnip driver. Private headers, logs, screenshots and actual
mapped runtime/library hashes are in `evidence/cache-identity-device/report.json`.
These are cache-mode/startup checks; startup latency and broad driver/gameplay
qualification are separate measurements.


The 2026-10-26 Echoes NSP restart tests exposed a separate payload problem: LHS3 recorded instruction bytes up to the constant offset, while BRX jump-table resolution reads constants beyond that boundary. An exact failing cached shader pair matched original assets, but replay read the next fragment header as a jump table and produced different control flow. Pipeline workers then exhausted allocator size classes before the overlay upload faulted.

LHS4 excludes BRX-dependent programs and their pipelines from persistent recording/replay and excludes those programs from content-only live deduplication. Original-memory foreground translation remains available, and other shaders still prepare in the background. Lookup/publication now gives concurrent foreground and background callers one build owner per indexed program or pipeline. ImGui Vulkan errors also fail explicitly instead of leaving failed allocations/maps unchecked.

Asset-free checks cover the actual BRX counterexample and recording gate, concurrent publication, and Vulkan fail-fast integration in optimized/sanitized builds and Android compilation. Thor passed two ordinary launches with identical runtime/environment: new-cache preparation followed by replay of 36 programs and 37 pipelines, manual-save load and movement. The final ten gameplay intervals were 30.0 fps with zero skipped operations; captured process logs showed no Scudo or malloc failures. These bounded results do not remove first-use compilation or qualify every game/driver.

The 2026-10-26 stutter follow-up replaces the exclusion with **LHS5**. NVN's
control section supplies the immutable compiler-constant span bound as c1
(`const_offset`, `const_size`). For foreground shaders whose full declared span
fits the currently mapped guest heap, the content key and record now include
exactly those constant bytes and each stage's span boundaries. Per-draw uniform
buffers and allocation padding are excluded. BRX shaders without verified
constant metadata retain the original foreground path and remain excluded from
persistent replay and content deduplication.

A sized translator entry point bounds every BRX table read to that span; an
incomplete table uses the existing general branch fallback. Original, bounded
and recorded-copy translation produce identical GLSL for the complete synthetic
counterexample. Replay checks the payload's content key before publishing a
program. Fragment unsigned-texture state participates in this key too, and the
input fragment key is preserved while translation derives its vertex-output
mask. LHS3/LHS4 records and caches from previous runtime binaries are rejected by
the existing implementation/driver/environment identity checks, so installing
this runtime causes one cold preparation. Device persistence and frame-time
improvements after that cold start still require fresh-process validation.

`vk_headstart_prepare(progress, cancelled)` provides an optional boot gate after
device setup. It bypasses the background reader's 120-present delay and returns
when recorded shader modules and pipelines have finished building, including
failed or unresolved entries. Progress reports completed builds, not submitted
jobs; total zero denotes cache inspection. Cancellation is a callback, allowing
the caller to use atomic state safely. Cancellation releases the waiter while
submitted work retains its background owner. No known records return promptly. Closing the startup window additionally calls
`vk_headstart_finish()` before saving the driver cache or exiting; Back skips
the wait and continues the same background work.

Run `tools/check_headstart_external_constants.py --android`,
`tools/check_headstart_progress.py`, `tools/check_shader_cache_identity.py` and
`tools/check_shader_cache_ownership.py`. These exercise actual production
functions with synthetic assets and delayed worker doubles, compile actual
translator code in optimized and ASan/UBSan variants, validate generated BRX
SPIR-V, and compile the Android production translation unit. They do not establish
GPU timing, on-device cache hit rates, or gameplay correctness.


## Compute headstart records

LHS7 adds bounded compute recipes containing immutable instruction/declared
constant bytes, stage identity, the shader key and all three local dimensions.
Startup replay validates records and device local-size limits, queues owned
work, and includes compute completion in the preparation gate. Foreground
lookup adopts only an exact ready result; pending or failed work retains the
normal foreground path. Persistence uses bounded owned worker snapshots and
deduplication. It supplies previously discovered states, not every possible
game shader or a guarantee against first-use stalls.
