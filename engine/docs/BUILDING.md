# Building Raitchi

No game files, keys, firmware or device are needed to build or to run the
checks. Everything below runs on Linux x86-64.

## Repository layout and contribution checks

The public repository has sibling `engine/` and `shell/` directories. Run the
commands here from `engine/`; in a standalone engine checkout, run them from
its root. Android shell sources live under `shell/src/android/`, while native
engine overlays live under `engine/ports/shared-android/` and `engine/native/`.
Do not edit regenerated `work/` outputs. Shell restoration and staging follow
[SOURCE-OVERLAY](SOURCE-OVERLAY.md), not an assumed independent shell URL.

For a first game-free contribution, run a bounded subset:

```sh
python3 tools/run_host_checks.py -k shared_hash
```

Check the runner's actual JSON results, including skips. A subset validates its
named scope; it does not establish a full host pass or device compatibility.
Documentation-only PRs can verify links and described examples without building
Android or rerunning unrelated runtime checks. ARM host results must record the
host architecture and source revision; the x86-64 instructions below are not an
unqualified ARM certification. See the [contributor backlog](CONTRIBUTOR-ROADMAP.md)
for reported ARM follow-ups.

## Prerequisites

| Tool | Version | Needed for |
|---|---|---|
| Python | 3.11+ | everything |
| git | any recent | fetching pinned references |
| clang / clang++ | any recent (host) | host checks |
| pkg-config and SDL3 development files | SDL3 | keyboard host checks |
| glslangValidator, spirv-val | any recent | shader host checks |
| JDK | 17 (`JAVA_HOME`) | Java host checks, Android build |
| Android SDK | platform `android-36`, build-tools `36.0.0`, CMake `3.31.6` | Android build |
| Android NDK | r27c (`27.2.12479018`) under `<sdk>/ndk/` | native build |

The scripts default to `~/android-sdk`; pass `--sdk` to use another location.

## 1. Fetch pinned references

```sh
python3 tools/fetch_sources.py
python3 tools/verify_sources.py
python3 tools/verify_sources.py --lock echoes-source.lock.json
```

This clones the public upstream projects listed in `sources.lock.json` at their
exact commits into `sources/`. They are read-only references and the base of
the native build. Do not edit them.

## Host checks

```sh
python3 tools/run_host_checks.py           # all
python3 tools/run_host_checks.py -k kernel # a subset
```

These compile synthetic tests with the host compiler (optimized and with
AddressSanitizer/UBSan) and write only under `build/`. The pipeline-library check
also uses a CPU Vulkan ICD (Mesa lavapipe), Vulkan loader development files, and
the shader tools listed above. Its Android translation-unit checks run only with
`python3 tools/check_graphics_pipeline_library.py --android` after a native build. Missing tools are
reported as **skipped**, not passed. Results: `build/host-checks.json`.

Before publishing anything, run `python3 tools/check_source_privacy.py .` and supply
`--patterns` with a private literal-pattern file for the final check. Also run:

```sh
python3 tools/check_compatibility.py
python3 tools/check_frontend_branding.py --source ports/shared-android --source native
```

## 2. Native runtime (Android ARM64)

```sh
python3 tools/build_las_baseline.py --deps --jobs 4
python3 tools/build_las_baseline.py --apk --jobs 4
python3 tools/build_letsgo_ffmpeg.py --build --jobs 4
python3 tools/prepare_shared_ondevice.py --fetch --build --jobs 2
python3 tools/build_shared_android.py --build --jobs 4
```

These are dependency/bootstrap builds, not game recompilation. `--deps` downloads
verified archives and builds base static dependencies. The separate `--apk` step
runs the LAS reference CMake/Gradle build that creates the SDL3 and AdrenoTools
cache expected by the current shared builder; no `--game` option or game image is
used. Its first Gradle/CMake configuration may need network access. The isolated
FFmpeg build enables and validates the native Opus decoder and swresample.

The current Capstone bootstrap is coupled to the legacy toolkit preparation:
`prepare_shared_ondevice.py` downloads and builds that broader toolkit even when
only Capstone is needed by default NCE. Omitting `--with-ondevice` from the final
app builder keeps the compiler/toolkit out of the APK. A dedicated Capstone-only
bootstrap is not yet provided. This coupling makes first builds unnecessarily
large; do not substitute an arbitrary Capstone binary for the verified inputs.
All native dependency outputs are regenerated locally rather than distributed as
private cache archives. See [source overlay setup](SOURCE-OVERLAY.md).

`build_shared_android.py` stages a copy of the pinned base into `work/shared-android`,
overlays the canonical `ports/shared-android/runtime` and `native/` sources and
builds `libmain.so` and `libraitchiprepare.so` into `build/shared-android/`. Edit
the canonical sources only; `work/` is regenerated on every build.

## 3. Android app

The Android app uses the shell identity recorded in `shell.lock.json`. For the
prepared source release, restore its exact upstream base and hashed source overlay
as described in [source overlay setup](SOURCE-OVERLAY.md). The public distribution
URL is not asserted to be a standalone shell fork or integration commit. A
maintained shell checkout can instead use its actual locked integration commit.
Then run:

```sh
python3 tools/build_eden_shell.py --shell ../shell-with-history --build            # offline
python3 tools/build_eden_shell.py --shell ../shell-with-history --build --allow-downloads   # first time
```

Outputs: `build/eden-shell/raitchi-eden-app-debug.apk`, `-release.apk` and an
`apk.json` receipt with source, library and APK hashes. The Gradle wrapper for
the game-process module is copied from the pinned base at staging time, so
`ports/shared-android/apk/` is not a standalone Gradle project.

### Signing

The builder creates a local test keystore under `private/` on first use (RSA
3072, random password, mode 0600). It is never committed. Release signing keys
for official builds are held by the maintainer and are not part of the
repository or CI.

## Installing on a device

The builder never installs anything. Install the debug APK yourself with
`adb install`. It uses its own application ID and data directory and does not
touch other emulator installs.

## Source snapshot limitation

The prepared filesystem source snapshot omits Git metadata and fetched source
references. Restore the shell over its pinned upstream history using the
[overlay workflow](SOURCE-OVERLAY.md). It is not an immediately buildable archive:
`tools/build_eden_shell.py` checks the shell Git identity, and
`tools/verify_sources.py` checks fetched pinned repositories. Until archive-mode
build support is implemented, build from the canonical engine and shell Git
repositories (with their upstream history), then fetch the locked references.
A build from a newly published checkout still requires a separate clean-checkout
validation; successful development-tree builds do not prove that result.

Three optional checks pin a second NDK (`29.0.14206865`); they are not in the host set.
