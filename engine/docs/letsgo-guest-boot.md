# Eevee guest boot on Thor

Verified 2026-10-25 local time. The separate Android reference now executes all
four Eevee1.0.0 modules and renders its controller-selection prompt with readable
text at approximately30 game frames/s on the AYN Thor. Both the stock Adreno740
driver and the pinned bundled Turnip show the corrected prompt. No game buttons
were injected. Gameplay beyond this screen, listening tests and save/reload
remain unqualified; the shared-runtime identical-artifact gate remains open.

## Complete private code build

The [private builder](letsgo-gamelib-build.md) compiled972 generated C units plus
a fresh ABI3 entry and linked a144,223,296-byte library with code identity
`4a34b9127ec182c1`. One6,666-case dispatch function exposed a long-running NDK18
optimization problem. A targeted Clang23 Android-target/NDK-sysroot O1 variant
resolved it;972 completed standard objects were reused. Exact compilers, flags,
module IDs, input hashes and object provenance remain in private build evidence.

The title library is streamed directly into this app's internal files, checked
against its private build hash, and loaded in a fresh process against the
matching installed runtime APK. Generated code is never staged in a public
device folder or committed. The runtime logs the correct module inventories:

| Module | Functions |
| --- | ---: |
| main | 60,380 |
| SDK | 31,906 |
| subsdk0 | 17,984 |
| subsdk1 | 13,342 |

All four load and relocate, then initialize SDK/subsdk0/subsdk1 before main. The
NPDM-derived program ID matches Eevee. The runtime indexes all27,818 RomFS files.
See [input preparation and memory evidence](letsgo-device-startup.md).

## Font and missing-text corrections

The first guest boot stopped at `no usable system font substitute`: desktop
font paths were unavailable on Android. The platform integration now resolves
the already packaged DejaVuSans.ttf through `rt_resource()` as a final Android
fallback, preserving explicit/user-supplied fonts' precedence.

The controller page then rendered its images at30fps but no text. Verified
Turnip selection alone did not fix this, nor did disabling the separate
high-resolution glyph cache with `LARECOMP_FONT_SCALE=1`.

A controlled dump found legible Latin glyphs in the original2048x1024 R8 atlas
(3,480 nonzero pixels, maximum255). The debug frame submitted49 glyph instances
sampling it. The captured vertex shader reads position/color/UV data through
guest global-memory loads. Without whole-heap host import, the desktop-derived
renderer supplied a4KiB zero buffer instead of those instance records.

The [renderer port](letsgo-renderer-port.md) now snapshots the bound read-only
storage ranges and supplies a compact address map. It avoids unsupported shader
int64 instructions on this GPU. CPU game code and its ABI remain unchanged.
Captured-shader translation/SPIR-V checks, snapshot bounds/sanitizer tests and
arithmetic-reference checks passed before device testing. Corrected screenshots
show the complete controller prompt with both actual Turnip and stock drivers.

This fallback explicitly rejects shader global stores and snapshots above its
16MiB/device-range limit. Later scenes may expose required extensions; no
general GPU-writeback or all-game compatibility claim is made.

## Reproduction and owner handoff

```sh
python3 tools/build_letsgo_android.py --apk --jobs 4
python3 tools/build_letsgo_gamelib.py --build --jobs 4
# Once installed and with prepared inputs:
python3 tools/run_letsgo_boot.py --serial <device>
```

Device-writing helpers refuse an active owner session. `run_letsgo_boot.py`
verifies the installed APK, four NSO hashes and generated-code contract before
replacing the app-internal library. Its isolated baseline uses30fps,720p,
bilinear, no frame generation, optional AA/effects/interface pack off, handheld
input emulation and original font-cache scale. Existing custom-driver selection
is retained. It sends no game input and sets no automatic close.

`tools/configure_letsgo_driver.py --serial <device> --driver stock|bundled`
changes only this app's next-process driver selection. It never changes the
system driver. The final owner handoff uses **stock**, since the memory fallback
restores controller text without requiring Turnip in this tested scene.

The owner session is protected again under `evidence/manual-session.json`, now
for `com.recomp.letsgo.test`. Next manual route: press a controller button,
continue through the opening, check readable text, physical controls and audio;
later establish movement and save/reload. The coordinator can read logs without
stopping the app. No more device updates while the owner tests it.

Exact receipts/logs/screenshots remain ignored under `evidence/letsgo-boot/` and
`evidence/letsgo-launch/`. Key observations: the initial font failure,
stock/Turnip missing-text comparison, original-atlas diagnostic, corrected
Turnip run and final corrected stock run. None establishes gameplay performance
outside the controller page.

## First manual-route failure

The owner subsequently reported blinking graphics and an app close after a popup.
Read-only collection found the process stopped and a native fatal when `hwopus`
requested FFmpeg's missing Opus decoder. The dependency recipe requested Opus but
disabled its swresample dependency, so FFmpeg silently omitted the decoder.
The [isolated audio dependency fix](letsgo-audio-fix.md) now builds and links into
a replacement APK without changing generated-code identity. Device playback
verification remains pending; the original handoff does not qualify the intro.

The same run records 17 skipped draws while pipelines were building. This is a
possible contributor to visual pop-in, not a demonstrated explanation of all
reported blinking. Diagnose the remaining graphics behavior separately after
removing the audio fatal. No device restart was performed during evidence capture.
After the owner explicitly authorized an update/reopen, the corrected APK reached
controller selection again without a startup fatal. Manual controls were handed
back to the owner; the post-controller route still needs verification.

The subsequent [presentation investigation](letsgo-presentation-fix.md) found
that Android returned ten swapchain images while the renderer kept only eight.
Corrected image/view/semaphore storage and checked enumeration remove the recorded
whole-window black flashes: controller selection and the title movie each have
zero entirely black samples in separate240-sample captures with normal settings.
Opus decoders open and movie decoding advances without a fatal. Normal controls
are restored; later gameplay and listening/save checks remain pending.

The owner then reported the game working but hotter than the emulator. The
[CPU investigation](letsgo-power-investigation.md) identified and corrected a
spinning Android UI event wait. A matched controller-scene measurement reduces
process CPU from123.15% to27.02% at approximately30fps. Normal gameplay and thermal
comparison against the emulator remain separate qualification work.
