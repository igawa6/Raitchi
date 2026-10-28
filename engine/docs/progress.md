# Implementation progress

Updated 2026-10-25. Both titles completed on-device compilation and executed
their published libraries against identical common runtime bytes in fresh Thor
processes. The owner reports shared Eevee running fine; its existing save loaded
into Oak’s lab at 30 fps. Shared LAS passed a short Mabe Village controller,
movement, menu, manual-save/reload and home/sleep-resume route. Lighting and
opening-video regressions are corrected, and the owner confirms water now
renders correctly. BowWow’s complete body renders at 30 fps; the owner has since
selected Balemuni Apex v2 Ultimate Turnip 26.3, which also passed a short LAS
rendering check. Longer routes and lifecycle qualification remain open.

The separate library launcher and foreground on-device compiler service are
implemented. Actual app-UID generation and cancellation preserve the prior
working library. LAS compiled 773/773 objects from scratch; Eevee completed
973/973 with four workers, including 303 verified reused objects. Native bridge
diagnostics now execute on actual runtime guest threads with independent TLS
and stateful SVCs. Original SDK startup and full native game execution remain
pending. Echoes inputs are audited, but no Echoes runtime support is claimed.
Existing reference apps, settings and saves are preserved; authorized diagnostics
use the isolated shared app. Earlier entries below describe evidence at the time,
not the current acceptance status.

## Completed

- Shared Eevee startup on Thor exposed and fixed SDK exception-context allocation, an unguarded LAS save-editor callback, NULL/stage-specific NVN program binding, vertex-only shader handling, and NVN present pacing without an initialized VI event. Host binder 35 checks and pacer 7 scenarios pass optimized and ASan/UBSan runs. A static-vertex-input setting restores readable graphics on the stock Adreno driver; normal constant-buffer optimizations remain enabled. Loaded runtime/game hashes match the verified artifacts. The new manual session uses separate data and saves, no scripted input and no automatic close. Shared LAS execution, later Eevee gameplay/save routes and efficiency remain pending. See [shared Android integration](shared-android-integration.md).

- Three bounded agents independently audited runtime compatibility, Android build readiness, and Thor/input availability. Their source references and findings were reviewed by the coordinator.
- All three upstream revisions are locked and `tools/verify_sources.py` passes. Reference checkouts remain unmodified.
- Android/ARM64 synthetic fixtures compiled with the installed NDK r27c and ran on the physical Thor through a small native runner.
- Both fixtures loaded with `RTLD_NOW | RTLD_LOCAL`. Arithmetic overflow/carry flags and direct memory helper checks passed against each upstream's real headers.
- The Thor reported 4096-byte pages. A one-page mapping at LAS Android's shifted base `0x1000000000` succeeded; a second `MAP_FIXED_NOREPLACE` mapping was correctly rejected. This does not verify the entire guest window or a zygote/app process.
- A separate owned LAS baseline checkout is prepared. RootBoost is forcibly disabled in this baseline; upstream source references and existing device settings/installations are preserved. Dependency build concurrency is limited to four jobs. Exact patch is stored as private build evidence.
- Pinned Android native dependencies and the isolated launcher APK built successfully. APK signature verification passed; application ID is `com.lasrecomp.android.test`, ARM64, minSdk30/targetSdk36.
- Both owned baseline packages imported on the host. LAS matches base build `AE16F71E002AF8CB059A9A74C4D90F34BA984892`; Eevee matches base build `0A3F0D21E67DAD80BB39BEEAEDBB0BAB00000000` (20-byte display form). Complete 32-byte IDs and file hashes are kept in private input evidence.
- The actual-input audit accounts for all strong static import candidates within each selected source profile. Let's Go has eight main imports requiring subsdk0, confirming the need for a four-module loader. This is candidate resolution evidence, not implementation correctness or dynamic service coverage.
- The LAS game library generated and compiled successfully with four jobs. A private APK containing that library built, passed signature verification and installed separately on Thor.
- LAS game data was extracted directly on Thor into the test app's external data directory using a 20 KB DEX importer, avoiding transfer of the full 6.3 GB extracted tree over Wi-Fi. Native app startup independently confirmed access to the data.
- The real Android app loaded `liblasgame.so` (86669 main and 34982 SDK functions), mapped both modules at shifted addresses, displayed the ready launcher, rendered the opening movie and reached the game title presentation. Captured app logs contain no fatal startup entry. Stock Adreno 740 Vulkan1.3.128 was selected; no custom driver or root clock-control feature was needed for this startup evidence.
- Follow-up input fixtures reached file selection. Stock-driver menu glyphs were invisible; the same fixture with the pinned bundled Turnip restored readable text/icons. This qualifies the observed workaround, not general driver correctness.
- A new isolated File 1 was created and loaded in a fresh process into the opening house. In a later owner-assisted run, logs recorded movement, an exit/warp request and a committed autosave. The process was alive at the 100.14-second checkpoint, then the timed harness closed it. Outdoor rendering is still pending confirmation.
- The owner received an uncapped, unscripted LAS session, protected from automated input, app stops and replacement. The owner subsequently reported it running fine and explicitly finished the session before the Let's Go device test. See the [manual reference route and evidence](las-manual-reference.md).
- The owner's LAS report supported advancing Let's Go bring-up while keeping the remaining specific LAS qualification checks open. LAS manual-session protection ended when the owner explicitly finished; the current protected session is Eevee.
- A [provisional native module validator](module-abi.md) passes 78 synthetic checks in optimized and sanitizer host builds, with Android arm64 compile-only verification. It has not been integrated into a real game loader.
- All four imported Eevee modules generated through the pinned Let's Go generator with explicit ARM64 SIMD assembly: 60,380 main functions, 31,906 SDK, 17,984 subsdk0 and 13,342 subsdk1, across 972 C units. This retains upstream's unshifted addresses and ABI3 / 1024-byte CPU.
- NDK r27c / API30 compiled the first function unit and address table of each Let's Go module plus its ABI3 entry (nine objects) without diagnostics. This samples generated code; the remaining units and runtime are not compiled/linked by this check. Private fingerprints, source/input hashes, generation logs and object hashes are under `work/letsgo-baseline/build/android-codegen/`.
- The compile-only entry uses the header's default `GAMELIB_ID="dev"`. It establishes no runtime/library identity match; a real linked library needs the matching build fingerprint and manifest.
- The bounded [Android bring-up recipe](letsgo-android-bringup.md) identifies bionic/header, x86-intrinsic, SDL startup and Vulkan loading changes before an Android runtime link. The full guest-window reservation or consistent address shift remains a separate execution gate.
- Two bounded implementation agents completed the Let's Go Android host and renderer portability changes. The coordinator integrated them, resolved additional full-build diagnostics, and successfully linked the complete asset-free native runtime against cached dependencies.
- ELF checks confirm AArch64, required exported entrypoints, retained HLE/service/reset registries, 16 KiB load alignment and only SDL3/Android platform dependencies. A separate ARM64 `com.recomp.letsgo.test` APK builds offline and passes signature verification. No generated game library is packaged. See the [build results](letsgo-android-build.md) and subsequent device observations below.
- After the owner explicitly finished LAS play, the separate Let's Go APK installed/launched on Thor. Full unshifted guest-window reservation succeeded; app-UID proc maps independently confirmed the complete range remains read/write after Vulkan initialization. A second fresh startup also succeeded. Stock-driver frontend text renders; a compositor-transform fix corrected its initial90-degree rotation. See [device startup](letsgo-device-startup.md).
- Private Eevee inputs transferred into the separate app:27,824 files, four NSO hashes/file count verified as app UID. The unmodified upstream RomFS builder then packed its required raw image on-device;27,818 file entries and4,428,561,283 data bytes match the source, with readable image/hash verified. No keys were transferred.
- The complete private AOT library linked successfully:972 generated units plus a matching ABI3 entry,144,223,296 bytes. A targeted Clang23/O1 variant handled one pathological dispatch unit while972 standard NDK objects were reused. The private manifest binds full module IDs/hashes, compiler/flags and current object inventory.
- All four real Eevee modules load, relocate and initialize on Thor. The runtime indexes27,818 RomFS files and starts actual guest NVN rendering. An Android packaged-font fallback resolves its initial system-font failure.
- A compact read-only GPU guest-memory mirror fixes missing controller text: original glyph pixels and draw calls were valid, but desktop-style host-import fallback supplied zero instance data on this GPU. Shader/snapshot/arithmetic checks pass. Corrected controller prompts render at approximately30fps on both pinned Turnip and stock Adreno740. This verifies this scene, not later gameplay or GPU writes. See [guest-boot evidence and handoff](letsgo-guest-boot.md).
- The owner-controlled session is protected again, now for the separate Eevee app using the stock driver. No buttons were injected and no timer closes it. Controls, intro/audio, movement and save/reload are the next manual route; common-runtime extraction remains pending.
- The owner subsequently reported blinking graphics and an app close after a popup. Read-only logs identify an explicit missing-Opus-decoder fatal when `hwopus` is requested. The inherited FFmpeg configuration disabled swresample, which silently disabled its requested native Opus decoder. An isolated offline rebuild includes Opus/swresample and is linked into a signature-verified replacement APK; the working LAS dependency cache and generated game-code identity are preserved. After explicit owner authorization, the update installed and returned to readable controller selection without a startup fatal. At that point, post-controller playback and the blinking cause were still pending; subsequent presentation tests below verify advancing movies and resolve the black flashes. See [audio fix](letsgo-audio-fix.md).
- The [native loader gate](module-loader.md) now opens real synthetic DSOs and validates their exported metadata before publishing function tables. Fifteen host scenarios pass188 checks each in optimized and ASan/UBSan variants, including rejection, fresh-process enforcement, constructor reentry and concurrency. The same gate artifact supports two-/four-module fixtures; all Android arm64/API30 gate/runner/fixture artifacts cross-link. No real game exports this new descriptor yet; app integration and execution-contract/sidecar verification remain pending.
- The [shared-runtime migration checklist](shared-runtime-migration.md) selects the LAS cache CPU, per-session address layouts, both hook mechanisms and a four-module registry. Current independent game libraries cannot be relabeled compatible; new shared-contract objects and adapter extraction are required.
- The owner prioritized fixing constant whole-image blinking before further launcher integration and authorized controlled diagnostics. Eight-second recordings reproduced47/239 fully black samples, including the FPS overlay. Disabling the render thread or changing acquisition/presentation mode did not eliminate it. The driver returned ten swapchain images while the renderer silently retained only eight image/view/semaphore slots. Corrected bounded storage, checked enumeration and index guards now preserve all returned images. With original mailbox/semaphore settings, controller selection and the title movie each capture0/240 black samples. Native Opus decoders open and movie frames advance without a fatal. Normal unscripted controls are restored for the owner; audible output and later gameplay remain unqualified. See [presentation fix](letsgo-presentation-fix.md).
- The owner reports Eevee working fine but hotter than the emulator; comparison conditions are unknown. Read-only sampling found a near-full-core SDL UI thread and clock/joystick polling dominating the profile. The port now applies LAS Android's existing event-poll/4ms idle-sleep workaround. An owner-authorized matched controller-scene comparison drops process CPU from123.15% to27.02% at approximately30fps, without changing mutex/renderer/settings or generated game code. This measures CPU waste, not watts or an Eden temperature advantage. See [CPU/temperature investigation](letsgo-power-investigation.md).
- The owner subsequently reports the corrected Eevee app much cooler and authorizes shared-runtime work first. The active manual session remains protected; this shared-core milestone performs no device actions.
- The [common execution foundation](shared-aot-execution.md) now implements CPU identity3, LAS's per-CPU cache, two-/four-module dispatch, both hook mechanisms, optional profiling, nine SDK libc wrappers, immutable per-session layout and a combined layout/metadata/dispatch gate. Exactly the same core bytes serve three synthetic producer profiles. All79 scenarios pass in optimized and ASan/UBSan host variants; Android arm64/API30 core, fixtures and runners cross-link. Preflight validates the complete independent profile before selecting/loading; corrected retries and concurrent setup are tested. Standalone metadata/profile tests now pass123 checks; the existing native loader scenarios also remain passing.
- [Actual generated-code probes](shared-aot-codegen-probes.md) compile12 selected LAS/Eevee function/table objects against the common CPU without diagnostics, preserving each title's shift and required hook capabilities. This is compile-only evidence; complete manifests, all-unit rebuilding/linking and guest execution remain pending. LAS's historical cache provenance gap is explicitly retained.
- The [complete legacy ELF import inventory](shared-aot-imports.md) measures56 distinct runtime names, with no unclassified imports or missing declared dependencies. The common core supplies15;41 require real OS/title implementations. Legacy libraries remain incompatible and are not relabeled as new producers.
- A bounded [Echoes source audit](echoes-source-audit.md) pins its current source separately, without builds/assets/device actions. It identifies differing module order and main-image size, five-argument SDK startup, 128-bit atomic helpers and writable GPU-memory requirements. These inform future contracts; Echoes is not a third supported game. Both working references and their generated caches are preserved.
- The owner authorizes full shared Android integration. The [common runtime](shared-android-integration.md) now combines session layout, checked NSO loader/relocation/init policy, CPU lifecycle, HLE/kernel/services and one NVN/Android backend. LAS-specific input/render/save/UI callbacks are gated; Eevee retains required handheld input, Opus, audio/movie and GPU-memory behavior. All work remains host-side while manual-session protection is active.
- Fresh LAS generation closes its historical cache provenance gap. New LAS773-object and Eevee973-object libraries compile/link against the same common runtime, export only the new descriptor data object, and pass sidecar/ELF/strong-import checks. Both references' original caches are preserved. Runtime-only corrections reuse the new object caches and rebind both manifests to the same final runtime bytes.
- The joint component checker now passes94 scenarios per optimized/sanitized variant; the actual loader passes26 rejection/execution-structure scenarios per variant, and production hash/movie wrapper checks pass. A source-defined HLE stand-in host harness loads all six original NSOs with verified hashes and exact generated address metadata, using non-executable, non-replacing mappings; it makes no guest calls. Captured shader probes pass six translation/SPIR-V cases without GPU execution.
- The separate `com.recomp.shared.test` APK builds/signature-verifies offline, contains no game code, preserves the exact common runtime/SDL/hook-library bytes, and disables RootBoost/on-device compilation. Installation, actual common-runtime execution and gameplay remain pending owner authorization to end the protected session. See [integration results and reproduction](shared-android-integration.md).

## Native ABI findings verified on Thor

| Field | LAS Android | Let's Go |
| --- | --- | --- |
| Game library ABI | 1 | 3 |
| Cpu size | 66560 bytes | 1024 bytes |
| GameLibTables size | 64 bytes | 152 bytes |
| Module capacity | 2 | 4 |
| Guest address shift | `0x6000000000` | None |
| PC/vector offsets | 264 / 512 | 264 / 512 |
| CPU alignment | 16 | 16 |

These are incompatible generated-code contracts despite the common exported entry name. Rebuilding both titles against one explicit contract is required. The fixtures contain no game code. They run under the Android shell, not an installed app: APK extraction, app linker namespace behavior, GPU rendering, full guest initialization and gameplay are not established by this test.

Reproduce compilation with `python3 tools/run_native_probe.py`. Add `--serial <device>` to push/run only the synthetic fixtures in their dedicated temporary device directory. Local results are stored under ignored `evidence/native-probe.json`.

## Reproduction and private artifacts

- `python3 tools/build_las_baseline.py --deps --jobs 4` prepares native dependencies in `work/las-baseline`; private log: `evidence/las-deps.log`.
- `python3 tools/build_las_baseline.py --apk --jobs 4` builds an asset-free launcher/runtime reference.
- `python3 tools/build_las_baseline.py --game --jobs 4` compiles local game code; `--package-game` builds the private APK. Its output contains generated game code and must stay private. The current APK hash and size are recorded in `evidence/las-build.json`.
- `python3 tools/import_on_thor.py --title las` builds only the game-free DEX importer. Add `--serial <device>` to perform a new isolated device import; existing extraction directories are refused. See [device importer](device-importer.md).
- Native app evidence: `evidence/las-launcher.png`, `evidence/las-intro.png`, `evidence/las-title.png`, `evidence/las-app.log`, `evidence/las-app-status.json`, and private build/import manifests. `tools/inspect_las_app.py --serial <device> --screenshot` captures only the isolated app's current log and primary screen. The saved status is the pre-stop observation, not a claim that the test remains running.
- `python3 tools/build_letsgo_codegen.py --generate --compile-probes --jobs 4` prepares the owned Let's Go checkout, reuses installed pinned Python packages, generates all modules and compiles the nine probe objects. It performs no device actions and downloads no packages. Repeated generation reuses matching source/input/options and output hashes; interrupted generation has no valid completion manifest.
- `python3 tools/build_letsgo_android.py --build --jobs 4` links the host-port reference; `--apk` also builds/signs its separate asset-free APK offline. Three patches replay from the pinned source with identical built file contents. `tools/start_letsgo_reference.py` refuses device changes while the manual-session marker is active and never automatically stops a launched app.
- The Gradle/CMake build does publish `libmain.so` at the path expected by upstream `build_game.sh`; the build-audit lookup concern is resolved for this actual build.

## Inputs and open gates

- Both selected base inputs are imported and verified. Echoes executable modules have a host audit; no Echoes generation or device execution is claimed.
- Both selected title libraries execute the same actual runtime bytes in fresh Thor processes. This establishes shared execution, not the full reference routes or a performance advantage.
- The owner reports Eevee running fine. LAS outdoor lighting/movie/water regressions are corrected; BowWow is observed at 30 fps with Turnip 26.2 R5. The older no-Int64 driver still needs further complex-shader qualification.
- Both titles completed real app-UID compilation and booted their published on-device libraries against identical final runtime bytes. LAS compiled 773/773 from scratch in approximately 628 seconds. Eevee completed 973/973 with a four-worker resume: 303 verified objects reused and 670 newly compiled; that final resume took approximately 896 seconds, excluding earlier phases and updates.
- LAS controller-node navigation, stick movement, in-game Items/System menus, manual save to previously empty File 2, fresh-process save reload, and short home/sleep-resume cycles now pass in Mabe Village. Audible output, longer lifecycle stress, later scenes and matched efficiency remain open; Eevee loaded its existing save into Oak’s lab at 30 fps, but still needs new-save, battle/capture and later-scene routes. Device work follows the manual-session guard.

## Current follow-up evidence

- Both title sessions mapped the same actual runtime bytes, beyond host linkage checks. Owner-reported Eevee play and observed LAS outdoor rendering do not establish the full reference routes or efficiency qualification.
- [LAS graphics investigation](shared-las-graphics.md): writable global-memory translation restores lighting on Turnip without shaderInt64; restoring the original Turnip `nolrz` startup default fixes opening video and owner-confirmed water. BowWow now renders with the owner’s newer Turnip 26.2 R5; the older driver’s complex uint32 shader path remains unqualified.
- [On-device toolkit and service](shared-ondevice-toolkit.md): real Android executables, original-module verification, resumable objects, cancellation and atomic library/sidecar publication are implemented. Both full title builds and execution of their published outputs are now verified; APK relocation reuse and four concurrent compiler processes were also observed.

## Current native and cache experiments

The [asset-free native bridge](native-arm64-proof.md) passed twice on Thor,
including TLS/x18, callbacks, threads, signals and contended atomic fixtures.
Original SDK [math](native-las-leaf-proof.md) and
[memory/branch](native-las-memory-proof.md) functions matched actual CPU3 entries
on 12 and 180 cases respectively. The subsequent
[production HLE bridge](native-runtime-bridge.md) passed 65 native-to-real-SystemTick
transitions, independent clock bounds and complete preserved-state checks.
These are bounded native experiments; normal game sessions continue to use C-AOT.
The next [native thread bridge](native-thread-bridge.md) passed on Thor using a
separately staged diagnostic runtime: two genuine KThreads joined, two RX
mappings, 48 returning SVC transitions, 16 guest TLS reads, one actual ExitThread
and no return from that exit. All state, identity and opaque-state checks were
zero. An initial test incorrectly equated writable and read-only guest TLS
registers; it was corrected and covered by focused regression checks before
the passing hardware run. Optimized and ASan/UBSan bridge tests each pass 1,739
checks. The installed launcher and working runtime were not replaced.

The opt-in guest-session API uses the actual loader and kernel after a verified
shared session, checks genuine current-thread identity, joins finished host
threads, and rejects reuse in the same process. Its controlled-dependency host
checks pass lifecycle/identity/no-session cases in both build variants. Actual
thread-start failure/duplicate/publication tests pass 118 checks per variant.
The [original SDK SVC wrapper](native-sdk-thunk.md) subsequently passed on Thor:
two native nested calls and returns, two actual C-AOT descriptor comparisons,
50 returning native SVC transitions and 16 TLS reads, with all failure counts
zero. The original RET remains in the relocated fixture; its gateway targets
copied SVC PC+4 and checks the native caller's BL+4 return marker. The actual
library's published receipt binds the audited source and object records, and
the coordinator verified the matching live receipt and cached objects. Host
bridge checks now pass 1,769 assertions per variant; six negative producer
receipt checks reject mismatches. A separate default-mode Thor regression run
also passed with 48 SVC transitions and no original-wrapper/AOT calls.

The next bounded gate is a guarded guest stack with host-stack HLE transitions
and normal-return/ExitThread checks. Full SDK/IPC startup, general original-PC
mapping, complete register handling and a general native backend remain open;
neither performance nor native game boot is established by this fixture.

[Renderer cache identity](shader-cache-identity.md) now binds runtime/driver/modes
and actual SPIR-V optimization selection, and rejects malformed shader spans.
All 7,853 host checks pass per optimized/sanitized variant. Fresh-process Thor
checks preserve matching headers, invalidate mode changes, and restore the
original profile identity. A subsequent warm LAS launch queued 72 programs and
137 pipelines with zero unresolved entries in 220 ms; this measures queue
preparation, not complete startup latency or elimination of all shader stalls.


## Subsequent original-code game backend, 2026-10-25

The owner requested continuation through actual game execution. The guarded
stack gate passed on Thor, then the optional original-code backend was connected
to the actual launcher, loader, SDK startup, services and NVN renderer. It uses
native ARM64 prefixes, explicit software control/system instructions, shared
host atomics and Unicorn fallback. Existing compiled title libraries still
supply validated metadata; their executable segments are NX guarded throughout
native sessions. Normal launcher sessions retain C-AOT.

LAS now renders its opening, title, file selection and the loaded Mabe Village
world on Thor. A cloned autosave was loaded, with Link, BowWow, buildings, NPCs,
vegetation and HUD visible. A guarded stick gesture changed the identity-checked
live player component X position by 0.11617, with Y/Z unchanged, and all generated
game DSO mappings remained NX. This establishes a short original-code world/input
proof. It observed approximately 0.2 fps in the world, with long initialization
pauses; normal playable performance and broader accuracy remain open. Initial title/menu rates were approximately 2 fps and
0.3 fps respectively, with code-cache saturation later forcing millions of
ordinary instructions into Unicorn; these are diagnostic observations, not a
matched performance comparison or a supported gameplay claim.

Startup exposed and fixed two independent bugs: recursively interposed SDK heap
wrappers caused repeated fourfold allocations, and Unicorn's nonparallel atomic
path could overwrite a concurrent writer on a failed exclusive store. The
native-original passthrough bypasses exactly one override; original atomics now
use shared host operations and unsupported forms fail before interpreter entry.
The physical atomic helper passed 412 checks, including contended 64-bit scalar and 128-bit pair
increments and failed-store preservation. The independent dynamic-X19 gateway
passed 1,152 physical calls with full CPU/host-state checks.

A bounded per-Cpu plan cache now passes 31,182 optimized and sanitized assertions
each; it validates the complete original input span on every hit and keeps live
host/hook checks ahead of lookup. Private overflow islands passed two forced-capacity Thor probes, each with
1,664 native calls, four joined threads, eight guards, zero state/protection
failures and no private pages left after join. The decoded and overflow caches
were active in the first world/movement proof; a later indexed host-lookup
build is being tested separately. See [backend implementation](native-game-backend.md).
The optional [SDK relocation accelerator](native-sdk-relocation-fastpath.md)
passed 1,518 actual-descriptor differential checks on Thor but is unbound and was
not used by this game run.
