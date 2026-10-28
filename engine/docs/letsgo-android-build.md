# Let's Go Android native reference

Initial native link verified 2026-10-24; expanded build and device checks verified
2026-10-25. Bounded agents implemented the host, renderer and private-code builder;
the coordinator integrated and tested them with cached dependencies. This page
describes the separate asset-free runtime APK. Actual guest execution is recorded
in the [Eevee boot report](letsgo-guest-boot.md); the shared-runtime milestone
remains open.

## Reproduce

```sh
python3 tools/build_letsgo_android.py --build --jobs 4
python3 tools/build_letsgo_android.py --apk --jobs 4
```

The tool verifies source pins and prepares an owned `work/letsgo-android` tree.
It applies [host](../ports/letsgo-android/host.patch),
[renderer](../ports/letsgo-android/renderer.patch) and
[integration](../ports/letsgo-android/integration.patch) patches without modifying
the pinned references. Already applied patches are recognized. CMake and the APK
shell live under `ports/letsgo-android/`; outputs remain ignored under
`build/letsgo-android/`. No ADB calls exist in the build tool.

NDK r27c / API30, CMake 3.31.6, JDK17, Gradle8.14.3 and AGP8.11.1 reuse the local
LAS baseline toolchain. Native dependencies are the already built SDL3, FFmpeg,
shaderc, Vulkan headers, volk, VMA, AdrenoTools and LinkerNSBypass. Eevee now uses
an [isolated FFmpeg build](letsgo-audio-fix.md) with native Opus/swresample; the
frozen LAS dependency cache is preserved. Gradle runs with
`--offline`; dependencies are reused locally. Worker count defaults to four. The APK shell has no
RootBoost or other clock-control code.

Keep the upstream ABI3, 1024-byte Cpu, direct function hooks and four-module
loader. The runtime's code ID hashes generator/header inputs and the selected
ARM64/assembly/unshifted profile. This is a reference identity, not the complete
future manifest with every compiler flag, title input and runtime API identity.
The synthetic module-descriptor validator is not yet integrated.

## Changes and checks

- Android host startup, app-owned paths/resources, native logging, unwind support,
  `SDL_main` and internal/packaged title-library lookup. See [host notes](letsgo-host-port.md).
- ARM64 scalar index scans and pause/yield handling; SDL-stock-loader-backed volk,
  dynamic VMA functions and Vulkan feature filtering. See [renderer notes](letsgo-renderer-port.md).
- Integration adds direct POSIX includes to movie/mod code and an Android fallback
  to the packaged DejaVu font. The realpath output
  buffer uses PATH_MAX to satisfy bionic fortify. FSR3's existing unavailable
  non-Windows implementation compiles with per-source `-fdeclspec` for its SDK
  declarations; this does not enable FSR3.
- Full native link succeeds with `--no-undefined`. ELF inspection confirms an
  AArch64 shared object, exported SDL_main/rt_dispatch/rt_fnhook/rt_svc, retained
  hle_impls/services/resets sections, and 16 KiB PT_LOAD alignment.
- DT_NEEDED contains SDL3 and Android platform libraries only (android/log/m/dl/c).
  There is no direct libvulkan dependency; Vulkan entrypoints use volk.
- Separate APK `com.recomp.letsgo.test`, minSdk30/targetSdk36, ARM64 only, builds
  offline and passes apksigner verification. Its native inventory is exactly
  libSDL3.so, libmain.so, libmain_hook.so and libhook_impl.so. No generated game
  library is packaged; the private title library is installed separately into
  app-internal storage.
- All three patches replay cleanly from the pinned source. All 17 changed/new
  source files match the actual built tree. The guarded launch helper rejects
  an active owner-controlled session before any ADB command.

Local evidence: `evidence/letsgo-android-build.log`,
`evidence/letsgo-android-apk.log`, `evidence/letsgo-android-elf.txt`,
`evidence/letsgo-apk-badging.txt`. Exact native/APK hashes, sizes and dependency
hashes are in `build/letsgo-android/native-build.json`. Unstripped native build
outputs retain debugging data.

## Device results and remaining gates

`tools/start_letsgo_reference.py --serial <device>` refuses to install or launch
while an owner-controlled session is active. Otherwise it installs only the
separate Let's Go reference and observes it without inputs or an automatic stop.
Its `--observe-only` mode permits read-only observation during manual play.

The original complete `[0x7000000000,0x7320000000)` window must be safely reserved
inside this actual app process. Initial mmap uses MAP_FIXED_NOREPLACE and checks
exact placement. Success is explicitly logged; a collision stops startup before
guest loading. There is no Android re-exec retry or implemented shift. See the
[address policy](letsgo-codegen-bringup.md).

The full window and stock-driver frontend are verified in the
[startup report](letsgo-device-startup.md). All generated units now compile and
link into the private title library. Eevee boots to readable controller selection
on both stock and pinned Turnip drivers after the font and read-only GPU-memory
fallback corrections; see the [guest report](letsgo-guest-boot.md). The current
stock-driver manual session is protected. Audio/input/save/lifecycle/gameplay
and common-core extraction remain open gates.
