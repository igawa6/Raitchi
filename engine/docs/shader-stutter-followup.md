# Shader stutter follow-up, 2026-10-26

## 2026-10-27: immutable shader captures and a matched restart

The cache previously accepted compiler constants only inside the main guest
heap. Wonder also stores valid shader data in separately mapped memory. The
runtime now captures the exact instruction and declared constant spans under
the kernel mapping lock, then hashes, translates and records immutable bytes.
The snapshot store shares identical stages across variants, with a 32 MiB
retained budget and a 16 MiB temporary limit. Validation or budget failure
retains foreground compilation without admitting an incomplete cache record.
Cache compatibility and BRX validation remain enforced.

A matched Wonder title test on candidate21 used the same APK, environment and
driver for both launches. The cold launch reported a longest frame of 890 ms.
The restart restored 37 programs and 40 pipelines before play, reported no new
compilation during the title transition, and reached a longest transition
frame of 29 ms before settling at 60 fps. The initial warm statistics include
77 preparation jobs completed before the guest started; they are not 77
in-frame builds. This qualifies the title route only, not complete gameplay or
unseen shader combinations. Private logs are `candidate21-cold-title.log` and
`candidate21-warm-title.log` under `evidence/wonder-event-20261027/`.

The native preparation screen already runs in the game process with the
selected Vulkan driver. Cached preparation is distinct from discovering every
possible pipeline in a game; new render-state combinations may still compile
on first use.

The owner reports more stuttering than the previous day. Existing measurements
confirm severe first-use compilation stalls, but there is no matched recording
of yesterday's route, cache state and settings. A warm scene at 30 fps does not
disprove this regression report.

## Measured evidence

The log named `echoes-owner-stutter-now.log` actually belongs to **LAS**, as its
launch header confirms. Both the driver pipeline cache and headstart records
were rejected on that launch. The longest interval reports a 4,026 ms frame and
3,944 ms of waiting. Its detailed slow-frame sample reports 3,983 ms total,
including 3,581 ms of pipeline creation, 112 ms of shader compilation and 12 ms
of game CPU work. These samples use different reporting boundaries and must not
be treated as an exact decomposition of the same interval.

Of 34 detailed LAS slow-frame samples, 31 include pipeline creation. The logger
rate-limits samples and omits some loading frames; these counts are not frame
percentiles. Private evidence is in
`evidence/shader-prep-20261026/owner-stutter-component-audit.json`.

The separate Echoes castle capture shows broad dark patches across several
surfaces. Its late steady intervals show 30 fps and no compilation, whereas
earlier loading stalls are omitted by the slow-frame logger. Persistent castle
corruption and intermittent first-use compilation are separate observations.

## Foreground translation correction

Six original Echoes depth vertex shader variants have five valid BRX targets
occupying exactly their declared 20-byte constant span. The bounded translator
rejected the optimized table when it reached that span's end and generated
different GLSL from the previous foreground translator.

Exact span exhaustion now terminates a nonempty, validated table without
reading padding. Empty spans, partial entries and out-of-range initial offsets
remain rejected. All 30 BRX samples examined across 118 unique graphics stages
now produce byte-identical GLSL to the previous foreground path. Optimized,
ASan/UBSan and generated SPIR-V checks pass. The owned sample audit also validates
823 container stage layouts. This proves translation parity, not a fix for the
castle image or a reduction in driver compile time.

## Cache changes built; device validation pending

The installed candidate 6 uses the whole runtime binary as part of headstart identity;
unrelated CPU/audio updates therefore force another headstart preparation. The
driver pipeline tag uses a smaller translator fingerprint, so it is incorrect
to say every CPU update also discards that cache.

Candidate 7 separates three kinds of compatibility: renderer dependency
identity for headstart records, compiler identity for source-keyed SPIR-V, and
device/driver compatibility plus integrity for opaque Vulkan pipeline data.
Game, rendering mode and environment checks remain required for replay.
Compatible driver data can then survive renderer updates even when headstart
records must change. This does not supply pipelines for states never encountered
before and does not promise a stutter-free first visit.

## Device validation still required

Build and archive a reviewed candidate before requesting a device takeover.
Owner play remains protected by `evidence/manual-session.json`; read-only
captures do not authorize installation or input.

Use the same game save, scene, driver and device settings for comparison. Record
first visits separately from repeated movement after a fresh-process replay.
Check actual cache acceptance and preparation completion, then frame intervals,
pipeline creation, shader compilation and the affected castle image. Preserve
the previous working APK for comparison. New scene compilation must not be
hidden by skipped draws or a warmed-only performance claim.

Candidate 7 passed the full Android native build, Eden release APK build, lint
and unit tests. The integrated bounded-constant and preparation-gate host checks
also pass after the LHS6 identity change, including Android compilation of the
actual rendering execution unit. The 16,564,744-byte release APK and native build
receipts are archived in `evidence/shader-prep-20261026/` as
`prototype-7-release.apk`, `apk-7.json` and `native-build-7.json`.

The owner then explicitly authorized installation and Echoes/LAS restart and
movement checks. The installed APK SHA256 matches the archived candidate.
Echoes imported its 5,523,301-byte legacy driver blob; a fresh restart loaded
6,559,005 bytes and accepted 209 program / 539 pipeline headstart records with
zero unresolved entries. LAS imported 4,191,141 bytes and subsequently loaded
7,917,973 bytes with 252 program / 388 pipeline records, also zero unresolved.
The startup log's elapsed count describes queuing/reader work; it must not be
presented as an independently measured end-to-end preparation time.

A short Echoes post-replay sample contained six new intervals at 30 fps,
36–50 ms maximum frame time, zero compilation and zero skipped draws. LAS also
rendered Field gameplay at 30 fps in cumulative logs. Its attempted movement
sample ended in the native Settings overlay and is excluded as a controlled
route. Owner-assisted changes of position/save and differing first-visit cache
states prevent a matched comparison with yesterday. These checks establish
persistence and bounded rendering behavior, not a general stutter fix.

The owner identified a sharp spatial boundary near the town fountain. Captures
immediately before and after a short northward movement show clean pavement
becoming broadly mottled while the affected view remains at 30 fps, with zero
compilation/waiting/skipped draws in the later intervals. The owner also reports
that indoor corruption persists. Candidate 7 therefore does not fix the image
defect. The next diagnostic is the existing gated, non-submitting material
probe at clean and affected views, followed by a targeted depth replay only
after an affected draw is identified. A screenshot does not establish a shadow,
AO or depth-bias cause.
