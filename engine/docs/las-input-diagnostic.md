# LAS title/input diagnostic

2026-10-24, read-only review of the existing LAS baseline source, SDL3 source and saved app evidence. No device actions, downloads, rebuilds or input trials were performed by this diagnostic.

## Follow-up result: input works; compare drivers next

Parent's `reference/20261024T144555Z` fixture successfully skips the opening and reaches file selection using `180:A:30,360:L:60,360:R:60,600:L:90,600:R:90,1200:L:90,1200:R:90`. Its screenshot shows paper menu panels with invisible game text/icons, while runtime overlay text renders. This supersedes the earlier uncertainty about native title transition below. `native.log` confirms `[pl] system font .../files/res/DejaVuSans.ttf (757076 bytes)`, stock Adreno740 Vulkan1.3.128, no `shaderInt64`, and no `uniformAndStorageBuffer8BitAccess`. The source disables unsupported requested features during device creation; those warnings are leads, not proof of the glyph failure.

### Select the existing bundled Turnip without rebuilding

There is **no force-bundled-driver environment variable or preference**. On Thor, `GpuDrivers.auto()` does not select the bundled driver because the system advertises Vulkan1.3. `use=true` alone also does nothing unless a valid installed driver exists. The no-rebuild route is to install the same bundled files into the test app's **current** driver slot and set its selection preference (`android/.../GpuDrivers.java:find,selected,config,auto,bundled`). Parent owns all following device actions:

1. Stop only `com.lasrecomp.android.test`; preserve its existing driver directory and driver preferences, if any. Keep the same successful input fixture/settings for both comparison sessions, with no further A confirms.
2. Stage `meta.json` and `vulkan.ad07xx.so` from the existing `work/las-baseline/build/android/deps/turnip/` (or identical `assets/turnip/` files in the built APK) into **internal** app-owned `files/gpu_driver/current/`, using the debug package's `run-as`. Do not put them in external `Android/data/.../files`: loading requires private internal storage. Do not overwrite an existing selected driver without preserving it. The library is about 10.7 MB; no new download is needed.
3. Set internal `shared_prefs/gpu_driver.xml` to select it. The minimal exact XML is:

   ```xml
   <?xml version='1.0' encoding='utf-8' standalone='yes' ?>
   <map>
       <boolean name="use" value="true" />
   </map>
   ```

   Merge `use` if other preferences already exist; preserve the original for rollback. Android SharedPreferences may restore an existing `.xml.bak`, so account for a preserved backup rather than leaving a conflicting stale backup beside a manually changed XML. Write as the app UID through `run-as`; no root or permission changes are needed.
4. A deliberately selected fresh attempt must not retain `files/gpu_driver/current/.probe`. That marker is normally created by `config()` and removed by native code after 60 successful presents. If left by a failed attempt, `config()` disables `use` on the next start and runs stock. Preserve any old marker as failure evidence before an intentional retry.
5. Start a **fresh app process**. Source automatically prepares `libmain_hook.so`/`libhook_impl.so` from APK libraries into `files/gpu_driver/.hooks/` when needed; manually staging the driver does not require preparing hooks. Require `[vk] GPU driver: .../gpu_driver/current/vulkan.ad07xx.so` **and** the subsequent Mesa/Turnip device/driver identification in the startup log. A load/instance failure can fall back to stock; changed XML or absence of a crash alone does not prove Turnip ran.

The bundled metadata identifies **Mesa Turnip Driver Revision 9v2, Vulkan 1.3.296**, library `vulkan.ad07xx.so`, minApi 27. The native loader uses `adrenotools_open_libvulkan`, `volkInitializeCustom`, and its own custom-driver window-surface path (`runtime/nvn/vulkan/device.c:149–185`). This trial changes only the isolated test app's driver choice. It does not replace the system GPU driver or another emulator's driver. Roll back by restoring preserved app driver/preferences state, or set `use=false` while stopped; on this Vulkan 1.3 Thor, unselected means stock. RootBoost remains disabled in the owned baseline.

### Existing graphics diagnostics if the driver comparison is inconclusive

Run the identical fixture/settings with stock and Turnip first, preserving a screenshot, driver log and feature warnings from each. Do not combine renderer changes with this comparison. Overlay text uses ImGui/its own font path; a working overlay does not verify the guest's font atlas sampling or guest shader output.

| Existing env hook | Purpose / limits |
|---|---|
| `LARECOMP_LOG_SKIPS=1` | Logs skipped-draw counts/reasons every 2 s; distinguishes absent/unready programs from executed-but-invisible draws (`exec.c:5994+`) |
| `LARECOMP_DEBUG_FRAME=700`, `LARECOMP_DEBUG_FRAMES=1`, `LARECOMP_LOG_FSHASH=1` | Pick a present already known to be in the file menu. Dumps that frame's color render targets to private `build/pass_<frame>_NNN.png`, pass attachment formats, draw shader addresses, blend/depth/channel-mask state and fragment texture dimensions/formats; hashes help correlate the same guest shader across drivers (`device.c:1823+`, `exec.c:1557+,1730+,6084+,6620+`). Use one frame; dumps stall rendering and contain private game images. |
| `LARECOMP_DEBUG_DATA=1` | Adds vertex-stream and fragment constant-buffer sample values to the debug-frame draw log; use only after identifying a missing glyph draw |
| `LARECOMP_DUMP_UBO=<fragment-code-address-hex>` | Dumps the chosen fragment shader's UBO data in the debug frame; address comes from the preceding draw log, not a shader hash |
| `LARECOMP_VK_VALIDATION=1` | Requests Khronos validation **if a layer is already available**. The baseline does not establish that the APK/device supplies it; do not add strict `VALIDATION_STOP` or install/download layers merely for this first comparison. |

Text atlas code supports NVN R8 and RG8 formats and maps NVN swizzles to Vulkan image-view component mappings (`vulkan/textures.c:fmt_of,renderer_texture_view`). There is no dedicated font-texture fix toggle. The standard font is already supplied; recoloring optional controller button-letter textures does not fix missing game glyphs.

If both drivers fail identically, existing **single-variable** probes are available without a rebuild: `LARECOMP_HALF_NATIVE=0` disables Android native-fp16 guest shader instructions; `LARECOMP_VK_SCALAR_CBUF=0` disables scalar-layout runtime-indexed constant-buffer words; `LARECOMP_VK_FMTLIST=0` disables the phone-default mutable image format list. Their source identifies the affected paths (`device.c:801+`; `textures.c:404+`), not this particular symptom. Test each separately from a clean process and retain matching screenshots; none is an established fix.

For a texture-refresh hypothesis, `LARECOMP_TEX_FULLHASH=1` forces full content checks (presence enables it; `=0` still enables it), `LARECOMP_VK_TEX_WATCH=0` disables page-write-tracking skips, and `LARECOMP_VK_NO_TEX_MEMO=1` disables fragment texture lookup memoization. Full hashing still skips upload if bytes are unchanged; it is not an unconditional upload toggle. Use these only after the one-frame diagnostic identifies a suspect atlas. Missing64/8-bit feature warnings alone do not establish stale texture data or justify changing these paths.

The touch setting `touch_all=1` is independent of driver and guest-text rendering. Keep the parent's intended title-context A/B controls unchanged for the comparison; no touch-layout edits are needed.

## Recommended next trial: native script, no rebuild

The existing APK reads `env.txt` from its **data root**, normally `/sdcard/Android/data/com.lasrecomp.android.test/files/env.txt`. It reads the file during fresh native-process startup, after `rt_paths_init()` changes the working directory (`runtime/main.c:193–209,263–275`; `runtime/paths.c:default_home`). Editing the file during a session does not reload it.

Use `LARECOMP_INPUT` to inject guest controller state directly. This bypasses Java delivery, SDL device classification, keyboard mapping, event batching and overlay capture. First qualify only the transition to file selection, with **L+R and no A presses**:

```text
LARECOMP_NO_LAUNCHER=1
LARECOMP_INPUT=600:L:30,600:R:30,900:L:30,900:R:30,1200:L:30,1200:R:30,1500:L:30,1500:R:30,1800:L:30,1800:R:30,2100:L:30,2100:R:30,2400:L:30,2400:R:30,2700:L:30,2700:R:30,3000:L:30,3000:R:30,3300:L:30,3300:R:30,3600:L:30,3600:R:30
LARECOMP_NO_USER_INPUT=1
```

Parent should preserve any existing `env.txt`, merge these keys for the isolated test package, start a fresh process, capture its **file log from startup** and screenshots, then restore the file before physical-control/save qualification. The ordinary source title test uses L+R (`tools/linux/smoketest.sh:109–115`); older exploration scripts use ZL+ZR, so use the current smoke-test pair first. This bounded fixture deliberately does not confirm a file, enter a name, create a save, warp, or press A repeatedly. It allows the initial movie to finish naturally. Timing may need extension if startup/title takes longer than the last scheduled press.

Expected log during a hold: `[input] id 0 buttons 0xc0 ...`, followed by `buttons 0` on release. A visible transition after those masks establishes native title-input handling. It does not establish Android/physical control delivery or save correctness. If the masks appear but the title does not advance, input delivery below the HLE is working and the next investigation is the guest title/controller state, not Android key injection.

The Android env reader has a **1024-byte line buffer**. Keep each whole `KEY=VALUE` line shorter than 1023 bytes; the above fixture is comfortably within it. Do not paste the long desktop smoke sequence verbatim. Values are plain text, with no shell quoting or spaces around `=`. `LARECOMP_INPUT` disables real input by default; explicitly remove it for normal play or use the second mode below.

## Frame clock and evidence markers

`runtime/hle/input.c:749–815` parses `frame:BUTTON:frames`, updates an atomic script mask, and holds a button when `start <= frame < start+duration`. Two entries at the same start/duration combine into a simultaneous chord. The clock is `total_frames`, incremented once per guest Vulkan present (`runtime/nvn/vulkan/device.c:1786–1796`). It is neither milliseconds, HID sampling count, FFmpeg decoded frames, nor frame-generation output. The existing log reports roughly **30 presents/s**, so a 30-frame hold is approximately one second and frame3600 is approximately two minutes of presented game time. Use the `[vk] ... frame N ...` line to assess actual progress.

Mask values follow `runtime/hle/input.c`: A=`0x1`, B=`0x2`, L=`0x40`, R=`0x80`, L+R=`0xc0`, ZL+ZR=`0x300`, Plus=`0x400`. `fill_state()` logs button changes only when `LARECOMP_INPUT` exists (`:934`); its sparse `GetNpadState` lines alone do not show that any button was pressed.

## Second trial: observe Android/SDL controls without synthetic presses

After the native title transition is understood, this env configuration injects no guest presses while enabling existing event/mask diagnostics:

```text
LARECOMP_NO_LAUNCHER=1
LARECOMP_INPUT=0:A:0
LARECOMP_NO_USER_INPUT=0
LARECOMP_LOG_EVENTS=1
```

The zero-duration A entry never becomes active. Native Npad button-change logging remains enabled because the environment variable exists. The explicit `NO_USER_INPUT=0` permits physical and injected Android events. `LOG_EVENTS` reports SDL event-family counts every five seconds; keyboard events appear in bucket `0x300` (`device.c:1991–2013`). It does not identify individual keys.

For a deliberate keyboard check, parent can hold K for roughly one second using `input keyboard keyevent --longpress KEYCODE_K` on the focused primary game surface, then release/wait and inspect the app log. This Android injection path remains unverified here; use the native fixture above to obtain deterministic guest holds. Repeat L+R as one overlapping chord, not two independent keyevents. A frame-script using `LARECOMP_HOST_INPUT=600:sdlkey:Q:30,600:sdlkey:E:30` is another already compiled option: it traverses SDL/menu/bindings but skips the Java boundary; retain `LARECOMP_INPUT=0:A:0` to log its guest masks.

Interpretation:

| Observed | Implication |
|---|---|
| No keyboard bucket or native mask changes | Android event delivery/focus/device path remains suspect |
| Keyboard bucket changes, no guest mask | Check overlay/name-entry capture, current bindings and event kind; bucket includes text events |
| `0xc0` reaches Npad, no title transition | Investigate game/controller/title state; Java delivery is bypassed or proven for that press |
| Native fixture works, Android keyboard does not | Narrow investigation to Java/SDL/focus/bindings |
| Android keyboard works | Qualifies that delivery path only; Thor physical gamepad is still separate |

## Source path and short presses

`LasActivity` does not override key dispatch/down/up. Its SDL surface's `onKey()` delegates to `SDLActivity.handleKeyEvent()` (`build/android/SDL3/.../SDLSurface.java:234`; `SDLActivity.java:1467–1543`). SDL drops fallback-flagged events; known joystick devices try controller handling first; remaining down/up events reach `onNativeKeyDown/Up`. SDL's Android key table maps Android K/Q/E/Enter to SDL K/Q/E/Return, and the JNI path calls `SDL_SendKeyboardKey`. Default native keyboard bindings are K=A, Q=L, E=R, Return=Plus (`runtime/hle/bindings.c:DEF_KEY`). Current INI bindings can override those defaults. Android physical pad defaults follow Xbox face **labels**: south=A, east=B, west=X, north=Y; do not assume desktop Nintendo-by-position defaults.

The native UI loop calls `input_user_blocked`, then `menu_handle_event`, then `input_handle_event`. An open overlay or name-entry dialog can consume keyboard input. Plain Enter normally reaches Plus when no overlay captures it; Alt/GUI+Enter toggles fullscreen. Back/F1/Escape may open the runtime overlay and change where subsequent keys go.

Native input explicitly latches down/up pairs that occur between game polls (`input.c:211–237,864–875`): an unseen release is delivered to a subsequent Npad poll for the rest of the present. Therefore a plain short Android keyevent is **not automatically lost merely because it is brief**. Delivery, focus and correct simultaneous buttons still need evidence. Movie skipping separately samples the currently held buttons via `input_buttons_now()`, which does not include the short-tap latch; a longer hold is useful specifically for skipping an opening movie.

## Movie looping and fonts

The saved log shows an active attract cycle: `opening.webm` opens at19:45:31, `title.webm` at19:46:26, another opening at19:47:00, title at19:47:29, opening at19:48:03 and title at19:48:07. Vulkan presents continue at30fps with level `-`. This supports a progressing title/attract state, not a frozen decoder. The HLE extractor reports real EOF and the guest requests each source; the title movie does not automatically enter gameplay. Upstream's smoke test explicitly supplies L+R before file selection. The precise guest-state cause of the unsuccessful manual trials remains unproven.

`runtime/hle/movie.c:ex_fill` handles A/B/Plus on `opening.webm`: it logs `[movie] opening: A/B/+ -> Plus (skip)` and may seek to the last half-second if a first-viewing game ignores Plus. This hook is only for the opening, not the title/file menu. The saved log contains no such skip marker; it does not prove that the injected K/Enter reached a held native state during the opening.

The only relevant saved font warning, `[texpatch] no system font for the button letters: Xbox letters stay Nintendo's`, concerns optional **button-letter texture replacement**. It is not a missing standard game-text font error. `runtime/hle/pl.c` supplies bundled DejaVu Sans as the final standard-font fallback and aliases missing shared-font types to an available font; the APK carries DejaVu. The saved title screenshot has the movie logo with no visible prompt, which by itself cannot distinguish movie timing, prompt rendering or missing glyphs. No source/log evidence currently ties missing fonts to the failure to reach file selection. First obtain the native `0xc0` transition evidence, then assess any prompt/text issue independently.
