# Echoes of Wisdom: continuous-native bring-up

Status date: 2026-10-26. Echoes executes original ARM64 code on Thor without a game DSO or generated function table. The current retry renders the title/menu, loads an existing isolated-session autosave into Suthorn Ruins and supports initial movement, bomb use, door interaction, jumping and stairs after the completed-future notification race and shader/graphics-store fixes. A new manual save has loaded in a fresh process. This establishes initial gameplay and one write/reload cycle; complete graphics, audio, broader repeated-session reliability and performance remain unqualified.

## Scope and inputs

The selected baseline is **base v0**, program ID `01008CF01BAAC000`. The available update is not part of this attempt. Original files, hashes, detailed binary evidence, keys and device identifiers remain in ignored local storage.

| Load / symbol lookup order | Module | Public build prefix | Mapped bytes, 16 KiB aligned |
| --- | --- | --- | ---: |
| 0 | main | `a581406bc9858651` | 67,125,248 |
| 1 | subsdk0 | `e0d8adcc446cf14d` | 11,059,200 |
| 2 | subsdk1 | `ff0149d0b5e882b8` | 7,028,736 |
| 3 | sdk | `3053b22935e32ddf` | 14,696,448 |

Main exceeds 64 MiB by 16 KiB, so LAS/Eevee's SDK placement would overlap it. The [Echoes adapter](../native/adapters/echoes/profile.c) declares its own layout, module recognition prefixes, program identity, constructor order and isolated state namespace.

The compression-aware pinned extractor produced **10,963 RomFS files**, **431 directories**, and **8,032,371,686 file bytes**. All six ExeFS files match the prior base import. Distributed resource samples were checked against the decompressed source. This does not establish Nintendo signatures, NCA integrity-layer verification or a whole-RomFS cryptographic digest. The shared filesystem serves the extracted tree directly; a duplicate raw RomFS image is unnecessary for this attempt.

## Native-only preparation and loading

[prepare_native_game.py](../tools/prepare_native_game.py) prepares original-module metadata from an extracted ExeFS:

```sh
python tools/prepare_native_game.py --title echoes --exefs /path/to/exefs --output /path/to/echoes.native
```

This compiles a small host metadata checker. It does not translate game instructions to C or compile a game DSO. The helper obtains policy from the actual C adapters, validates the native contract and every original NSO/LZ4 image, and publishes the manifest atomically without replacing conflicting metadata. The produced Echoes manifest is 544 bytes.

The versioned [native manifest](../native/runtime/native_manifest.h) identifies original hashes, full build IDs, module ranges, CPU3 and the native runtime contract. It contains no generated-function counts, generated-code hashes, function pointers or library digest. Stable module IDs are independent of array order; the adapter enforces the title's declared lookup order.

The production [session loader](../ports/shared-android/runtime/gamelib.c) accepts this metadata only with continuous NCE. It verifies the adapter and complete original-file hashes, activates/reserves the layout and binds an original-text registry. It never calls `dlopen` for a game library. The [NSO loader](../ports/shared-android/runtime/loader.c) independently rechecks raw bytes, decodes bounded images, resolves symbols and preflights every relocation and init/fini target before copying bytes into guest memory. Native initializer validation uses original text ranges rather than generated function boundaries.

The existing compiled-descriptor setup path remains available for LAS/Eevee. Their generated executable segments remain NX-guarded during NCE. Native-only sessions instead record that no generated game DSO was requested or bound. Both paths use the same continuous image manager, kernel, HLE and renderer.

## Echoes policies and services

- SDK16.2 uses five-argument `Start(thread,args,init_sdk,init,fini)`. SDK constructors run early; late initialization is subsdk1, subsdk0, main. An additive kernel startup API supplies x4 while retaining the existing four-argument ABI.
- The IPC overlay supports TIPC close15 and commands16+ alongside the existing HIPC/domain/handle paths.
- Audio output records use `0x90` for REV9+ and retain `0x10` for legacy revisions.
- Eevee's controller-finger behavior is explicitly limited to Eevee; it no longer suppresses Echoes' physical ZR.
- CASP pair atomics are distinguished from monitor-based LDXP/STXP instructions. Valid CASP orderings remain unchanged for hardware execution; invalid encodings still reject. This removes four SDK instructions incorrectly rejected by the old classifier.

The actual original-text scan now reports zero unsupported classifier words across all four modules. This is not a reachability or successful execution proof. A standalone Thor test passed 96 CASP compare-success/failure checks over both element widths and all four orderings; this does not qualify concurrent memory ordering or guest context integration.

## Validation already completed

Asset-free optimized and sanitizer checks cover native manifests, original NSO decoding, native dispatch lifetime, original-text initializer validation, metadata preparation, five startup arguments/constructor order, TIPC and audio revisions, input policy and CASP classification. Actual overlays and the common runtime/APK build for Android ARM64. Existing AOT regression cases remain passing.

The real Echoes host preflight used production native session setup, the actual layout/reservation and registry, real SHA256 and the production loader. It loaded four original modules, checked **574,273 RELA entries** and **1,643 non-null init/fini array targets**, with zero game DSO loads, generated functions or guest calls. Source-enumerated HLE address stand-ins covered all relocation-referenced strong imports. Stand-ins do not establish working HLE behavior; original code was not executed on the host.

## First device attempt

The verified common runtime loaded four original modules, registered 93 original-definition host bindings and enabled continuous NCE with no game DSO requested or bound. Logs show SDK early initialization, the declared subsdk1/subsdk0/main late sequence, new guest threads, account/input services, and the complete 10,963-file resource index with actual reads. The active driver was the owner's selected Balemuni Apex v2 Ultimate on Adreno 740.

The first actionable failure was `unimplemented strong guest import nvnProgramGetSubtileSize`. This came from dynamic NVN procedure-address lookup, which is separate from relocation-referenced imports. A zero unresolved-relocation census therefore did not cover it. Rendering did not begin. An explicit diagnostic-only lookup mode now binds named fatal traps for unresolved procedure queries. This allowed the bulk function table to finish without claiming those functions are implemented; any actual call remains fatal. None of the unresolved traps was invoked during the subsequent observed startup.

The subsequent attempt submitted 17 black frames and stalled in `std::future<void>::get()`. A bounded read-only sample showed the promise READY flag set, its mutex unlocked, and a waiter still asleep. A deterministic actual-kernel-body test reproduced a lost notification: the old implementation released the mutex before publishing the waiter flag. It lost 20/20 controlled notifications; publication before unlock woke 20/20. The corrected order matches the primary Atmosphere and Eden kernel implementations. The device retry passed that stall and continued submitting thousands of frames. The repeatability and the rest of the game remain unqualified.

The earlier black-image retry identified immediate HSET(P), VOTE and RED shader instructions, alongside an unsupported SUST compute instruction. Immediate HSETP, VOTE and RED translation and fragment writable graphics-mirror synchronization/readback are now installed and tested on Thor. That retry still failed a SUST compute instruction. The subsequent fix proves its handle origins, emits real storage-image writes and binds supported writable Vulkan views with GPU visibility barriers. The device retry eliminated the steady skipped operations. This is a bounded diagnostic observation, not a census of every shader or path in the game. Presentation counters and their inherited speed labels are not performance or simulation-speed measurements.

Known EoWRecomp GPU compatibility changes were reused rather than replacing the shared renderer: a compact guest sampler builder prevents an 80-byte host descriptor from overwriting a 64-byte game object, nonzero Z-cull metadata avoids zero-sized allocations, and retained 8x host command shadows preserve queued handles without writing enlarged records into guest chunks. SDK16.2 compute headers and BFE.BREV/FLO/LEA.HI translation fixes are also included. Their source/host checks are separate from the device images below.

## Current device evidence

Echoes now imports and launches through Raitchi's real Android document picker and **Play** button. The native-only NSP path prepares approximately 39.3 MiB in eight private files, retains the selected package, and serves the original 8,033,071,180-byte RomFS without an extracted resource tree. Its 544-byte manifest matches the existing native preparation flow. See [direct package loading](direct-nsp-native-loading.md).

Fresh-process maps and logs show original ARM64 NCE, no generated game DSO and the owner's Balemuni driver. On Thor, the package-backed title movie now decodes and visibly renders; the save menu loads an existing isolated Suthorn Ruins save. Movement, the first door, bomb use, jumping a barrier and climbing stairs have responded during the observed session. The scene presented at 30 fps with zero skipped operations at the owner's current 1x/1080p setting. These observations cover the tested scene, not general performance or complete accuracy.

The earlier steady 420 skipped operations per two-second interval were all the same failed compute shader, not missing graphics draw calls. The captured shader now translates six storage-image outputs. A prior 720p corridor retry recorded 182 consecutive steady intervals across 362 seconds at exactly 30 presents/s, with zero skipped operations. Counts and inherited speed labels are not independent measurements of simulation speed.

Actual post-mix stereo float audio samples were non-silent and finite during gameplay, with no clipping in the sampled windows. This establishes mixed output, not acoustic verification or complete effects accuracy. A new manual save committed through the game UI with a correctly rendered thumbnail. Individual SHA256 checkpoints verified three new manual-save files, changed save metadata and unchanged prior autosave data. The real menu subsequently selected the new manual save in a fresh process, loaded Suthorn Ruins and accepted movement. This verifies one write/reload cycle, not every checkpoint or failure mode. The latest runtime update verified all 36 existing isolated save files unchanged across installation, with driver preferences preserved.

The black-heart/red-rectangle defect was traced to NVN RGB565 format0x3c mapped with red in the high bits instead of the low bits. Captured texture hashes matched the original BNTX assets, and the actual heart shader uses the red channel for opacity. Correcting the mapping to Vulkan B5G6R5 fixed the hearts in both the save menu and gameplay on Thor. Original block-height layouts were correct and were retained. A bounded CPU-only material probe supports further diagnosis without GPU readback. Other small artifacts and broader scene coverage remain unqualified.

Fresh ordinary launcher retries exposed a repeatable title-transition crash in the ImGui Vulkan vertex upload. The overlay no longer retains borrowed ImGui geometry across presented frames; it rebuilds each required UI frame while preserving FPS smoothing and same-frame redraw. Actual ImGui idle/input/collection checks passed in optimized and sanitized builds, and one updated Thor run passed the transition and manual-save load. A second cache-replay launch still failed. Process-specific logs show Scudo allocation exhaustion in pipeline workers before the overlay fault; a no-replay diagnostic remained stable through title idle and manual-save load. The exact failing cached vertex/fragment pair matched original resource bytes. Its BRX instructions need constant-buffer jump tables omitted by the old cache; replay instead read the following fragment header and produced different control flow. The new cache policy excludes these external-constant-dependent programs from disk replay and content-only deduplication, retaining original-memory foreground translation. Other programs still prepare in the background. A cache-format change rejects the incomplete old records. Two ordinary Thor launches with identical environment and runtime subsequently passed: the first primed the new cache, and the second replayed 36 programs and 37 pipelines, loaded the new manual save and accepted movement. The final ten measured gameplay intervals reported 30.0 fps and zero skipped operations, with no Scudo/malloc failures in process-specific captures. Both sessions stopped orderly. This validates that bounded repeat sequence; the overlay change alone did not fix the allocation failure.

Diagnostic unresolved-NVN traps remain enabled for Echoes; none was invoked in the qualified startup/scene. Unknown invocations still fail explicitly. The first pre-load archive was truncated and is not trusted for restoration; a later four-file checkpoint was copied and individually SHA256-verified. Original game data, detailed measurements and private captures remain in ignored local evidence.

The same final common runtime subsequently loaded LAS's Mabe Village and Eevee's Oak lab in separate fresh continuous-NCE processes, and movement responded in each. Their existing folder-backed sessions retain verified metadata libraries with generated segments non-executable. This is shared-runtime regression evidence, not direct-NSP qualification for those titles. LAS's previously reported small black artifact remains deferred at the owner's request.

## Remaining device gates

1. Retain explicit errors for unsupported behavior and investigate any newly observed graphical defects using exact material/texture/shader evidence.
2. Broaden movement, combat and cutscene coverage; diagnose any progression stalls before claiming general gameplay support.
3. Broaden save/reload and repeated-session coverage beyond the verified manual-save cycle and two ordinary package-backed launches. Retain independent save namespaces and verify failures do not overwrite committed progress.
4. Broaden LAS/Eevee regression coverage beyond the verified existing-save load and movement checks; their direct-package paths still need device qualification.

Original CPU execution does not remove shader/pipeline compilation. Native mode alone does not make arbitrary Switch titles compatible.

## Provenance

