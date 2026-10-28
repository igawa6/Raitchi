# LAS Android baseline build readiness

Audit date: 2026-10-24. Reference: `sources/las-android`, commit `cca791a3e7218e1dac95e7d4a11b8f97dba3ea51`. This audit read source and local tool/cache metadata only. It did not download dependencies, compile, install an APK, launch an app, or verify gameplay. The AYN Thor target parameters supplied for this task are Android 13, ARM64, 4 KiB pages, 16 GB RAM; this report does not independently verify them.

## Smallest baseline

The smallest useful Android application baseline is the **runtime/launcher APK without the on-device compiler kit or a game library**. `android/app/build.gradle` defaults `onDeviceBuild` to false, and game code is optional through `lasGameLib`. It builds `libmain.so`, `libSDL3.so`, and the AdrenoTools hooks. The font/icon assets are repository resources, not game inputs. Building this APK does not require an NSO, ROM, keys, saves, Python generator packages, Zig, translations, a profile, or randomizer assets.

The necessary source-provided sequence, to run later in an owned working copy (reference checkouts remain read-only during audit), is:

```bash
export ANDROID_HOME="$HOME/android-sdk"
export ANDROID_SDK_ROOT="$ANDROID_HOME"
export ANDROID_NDK_HOME="$ANDROID_HOME/ndk/27.2.12479018"
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk
export PATH="$ANDROID_HOME/cmake/3.31.6/bin:$JAVA_HOME/bin:$PATH"

# WORK is the separately prepared implementation/build checkout.
cd "$WORK"
printf 'sdk.dir=%s\ncmake.dir=%s\n' "$ANDROID_HOME" "$ANDROID_HOME/cmake/3.31.6" > android/local.properties
bash tools/android/build_deps.sh
cd android
./gradlew :app:assembleDebug -PlasTestId
```

Expected APK: `android/app/build/outputs/apk/debug/app-debug.apk`; separate application ID: `com.lasrecomp.android.test`. `-PlasTestId` uses presence, so omit it only when intentionally selecting the normal package. Do not add `-PonDeviceBuild=true`, `-PlasGameLib`, or translation properties for this baseline. The explicit local CMake selection above avoids relying on AGP's implicit CMake discovery/download; the source specifies minimum CMake 3.22 but does not pin a Gradle CMake version. This complete command sequence remains untested. The dependency script uses every host CPU and has no job-count option, so parent should schedule its potentially substantial native builds deliberately.

The repository's `build_deps.sh` always checks/fetches the declared archives before checking individual installed outputs. Its shaderc step runs `python3 utils/git-sync-deps`, and app CMake uses a recursive Git FetchContent dependency. These steps require network/cache preparation; this is not currently an offline build.

## Local readiness observed

| Item | Local observation | Consequence |
|---|---|---|
| JDK | OpenJDK <address>; Java and javac installed | Matches source Java 17 requirement |
| Android SDK | `$HOME/android-sdk`; platform Android 36 and build-tools 35.0.0, 36.0.0, 36.1.0 present | `compileSdk`/`targetSdk` 36 available; SDK environment variables were unset |
| Android NDK | Exact `27.2.12479018` present; clang reports 18.0.3 | Use this toolchain, not system clang 23.1.1 |
| CMake/Ninja | SDK CMake 3.31.6 plus Ninja present; system CMake 4.4.4 also present | Select SDK CMake explicitly; dependency compatibility with system CMake 4 untested |
| Gradle | Only wrapper distribution 8.13 found in cache; source wrapper pins 8.14.3 | Wrapper download required unless supplied separately |
| Android Gradle plugin | Only 7.3.0 and 8.9.1 found in module cache; source pins 8.11.1 | AGP and its transitive artifacts require resolution |
| Native dependencies | No reference `build/` output directory | SDL3, FFmpeg, shaderc, Vulkan headers, VMA, volk, bundled Turnip and AdrenoTools preparation missing |
| Shell prerequisites | bash, make, git, curl, shasum, tar, unzip, xxd, pkg-config, Python and Ninja found | Dependencies have the main build utilities; full script execution remains untested |
| Optional archive utility | `zip` not found on PATH | On-device `make_assets.sh` needs it; smallest baseline does not |
| System Python | 3.14.7; capstone import reports 5.0.7 while distribution metadata reports 5.0.9; lz4 4.4.5; no unicorn or PyInstaller | Irrelevant to launcher APK; use a fresh pinned host generator venv rather than trusting mixed system metadata |
| Reusable source archives | Only unrelated SDL3 3.4.10 archive found by filename search under home | Does not satisfy SDL3 3.4.18 pin; HTTP pip cache was not established as reusable |
| Repository assets | Baloo2, DejaVu and icon files present; no submodule entries reported | Baseline resource task can use checked-in assets |

Recheck without side effects using `python3 tools/check_android_build.py`. It reads file presence and Python package metadata, and runs only `git rev-parse`; it never invokes Gradle, downloads, builds, ADB or an app. Presence alone does not establish a complete or valid cache.

## Exact dependency and packaging split

Source evidence: `tools/android/build_deps.sh`, `android/app/CMakeLists.txt`, `android/app/build.gradle`, `android/gradle/wrapper/gradle-wrapper.properties`.

| Dependency | Pin / build role |
|---|---|
| Gradle / AGP / NDK | 8.14.3 / 8.11.1 / r27c (`27.2.12479018`) |
| SDL3 | 3.4.18; unpacked into `build/android/SDL3`; Java SDLActivity sources and shared native library built from same source |
| FFmpeg | 9.0.2; static PIC, LGPL-only configuration, no network/programs; selected WebM/MOV codecs/demuxers; runtime links avformat, avcodec, swscale, avutil |
| shaderc | 2026.3; static `libshaderc_combined.a`, glslang/SPIRV dependencies populated with upstream sync script |
| Vulkan headers / volk | Vulkan SDK 1.4.350.1; volk source compiled into runtime; Vulkan entry points loaded dynamically |
| VMA | 3.3.0 headers |
| Turnip | `turnip_v24.3.0_R9v2.zip`, checked SHA256; included in launcher assets by the dependency script |
| AdrenoTools | `8fae8ce254dfc1344527e05301e43f37dea2df80`, CMake FetchContent recursive Git checkout; `main_hook` and `hook_impl` packaged beside runtime |

Archive pins in dependency scripts include SHA256 checks. Wrapper properties do not specify `distributionSha256Sum`. CMake fetches AdrenoTools by commit but depends on recursive upstream submodules. Complete reproducibility, artifact digests and availability of those fetched inputs remain to be recorded by the actual build.

The runtime uses static C++ STL, links Android/log/m/dl and statically incorporates FFmpeg/shaderc. Its own symbols remain exported for game-library references and backtraces; `--exclude-libs,ALL` hides symbols originating from static dependencies. `-Bsymbolic` binds internal calls. Runtime and hook link flags request 16 KiB maximum page size, which is compatible in principle with the stated 4 KiB device; actual PT_LOAD alignment/packaging and loader acceptance must be checked on produced artifacts. SDL3's final alignment is upstream-controlled and uninspected because it has not been fetched/built.

## Game library: host route

Source evidence: `tools/android/build_game.sh`, `tools/android/gamelib_id.sh`, `tools/gen.py`, `runtime/game_addresses/versions.h`.

`tools/android/build_game.sh <game-folder>` needs the player's extracted `exefs/main` and `exefs/sdk` NSOs, a previously built matching `libmain.so`, NDK r27c, and a Python venv with capstone **5.0.9**, unicorn **2.1.4**, lz4 **4.4.5**. It creates that venv and installs packages when absent. Python 3.12/3.11 are preferred, otherwise system Python is used; installed system capstone does not satisfy this pinned environment. Runtime play also needs the player's actual game files, including ROMFS; generating code alone does not establish a playable installation. Source address tables select supported game versions by main build ID; script header's v1.0.0 wording is narrower than the actual build-ID-aware implementation.

The generator processes SDK at `0x7004000000` and main at `0x7000000000` with `--simd asm --shift 0x6000000000 --split-dispatch 512 --hot hot_functions.txt`. Android's guest window is shifted down by `GUEST_SHIFT`; generated guest addresses and runtime header must agree. Flags include `-O2`, signed char, PIC, hidden visibility and scheduler tuning. Cold units use `-O0`; split-dispatch units disable LICM promotion. Clang22-only optimizer switches are probed before use with host NDK clang18.

Output: `build/android/gamelib/arm64-v8a/liblasgame.so` and `.id`; generated C and objects remain private build outputs. Link uses `-lmain`, SONAME `liblasgame.so`, `-Bsymbolic`, 16 KiB maximum page size. This is a title library coupled to the runtime's exported functions and header layout, not a standalone executable.

Important pending build check: `build_game.sh` finds `libmain.so` through `android/app/build/intermediates/cxx/*/*/obj/arm64-v8a/`, whereas Gradle sets native staging under `build/android/cxx`. Confirm the app build actually populates the lookup path before a game build. `--push` hardcodes the **normal** package ID; it must not be used for the isolated `.test` baseline. `--apk` also invokes Gradle without forwarding `-PlasTestId`. Parent should handle these explicitly in its implementation workflow.

## Game library: on-device route

Source evidence: `tools/android/ondevice/`, `runtime/gamebuild.c`, `LasActivity.buildTools`, `docs/android-ondevice-build.md` (upstream claims, not local validation).

`tools/android/ondevice/build_all.sh` runs Python build, Zig fetch, compiler-kit assembly and asset archiving on the host. An APK with `-PonDeviceBuild=true` includes:

| Artifact | Purpose |
|---|---|
| `liblaspython.so` | Executable PIE launcher for CPython, `$ORIGIN` RUNPATH |
| `libpython3.14.so` + stdlib/extensions | Official CPython Android 3.14.8 archive; host Python3.14 optionally generates bytecode |
| `libcapstone.so`, `libunicorn.so`, lz4 modules | NDK-built generator dependencies, versions 5.0.9 / 2.1.4 / 4.4.5 |
| `libzig.so` | Renamed static Zig 0.17.0 aarch64-linux executable; clang22 and lld run on Android; not a conventional shared library |
| `ondevice.zip` | Python home, generator/address/patch metadata and compiler kit |
| Compiler kit | Matching clang headers, trimmed NDK sysroot headers, API30 crt/libc/libm/libdl stubs, compiler-rt builtins, runtime headers/entry, HLE list, `link.rsp` |

The builder compiler runs `clang -target aarch64-linux-android30 -nostdinc` with the supplied headers. This preserves the Android ABI including reserved x18. The linker runs `ld.lld`, links to the already installed app's `libmain.so`, and uses NDK builtins and bionic stubs. Programs must be real extracted native-library files, so this packaging sets `extractNativeLibs=true` and legacy native packaging. The no-kit baseline maps libraries from APK and `gamebuild_available()` is false.

Java unpacks assets to internal `files/ondevice/` by app version; generator children run SDK then main, compile units concurrently, throttle starts according to memory availability, and support resumable caching. Default job count follows cores; `LARECOMP_BUILD_JOBS` can override it. The resulting library and `.id` are installed in internal storage, not the external game folder. 16 GB total RAM does not guarantee compile peak/thermal behavior; no timings or memory claims from the upstream phone are verified for Thor. Building the kit introduces the Unicorn GPL-2.0 binary and bundled license/source obligations; the script includes license texts. This audit makes no distribution approval.

## Runtime loading and shared-platform implications

Source evidence: `runtime/gamelib.h`, `runtime/gamelib.c`, `tools/gamelib_entry.c`, `android/app/src/main/java/com/lasrecomp/android/LasActivity.java`.

SDLActivity loads libraries in order `SDL3`, `main`. Game lookup then tries `LARECOMP_GAME_LIB`, internal `files/liblasgame.so`, and finally bare `liblasgame.so` for a packaged library. Files normally require a matching sidecar ID before load; explicit environment/APK candidates are checked through the loaded entry. It uses `dlopen(..., RTLD_NOW | RTLD_LOCAL)` and `dlsym("lasgame_tables")`.

`lasgame_tables` is the mandatory exported entry. It returns ABI **1**, the full code ID, sorted main/SDK guest-address arrays, corresponding native function-pointer arrays and counts. Optional instrumentation exports `lasgame_pgo_dump`. The full ID combines a 16-hex-digit hash of generator/header/options inputs with the first eight bytes of the main NSO build ID (16 uppercase hex digits). ABI mismatch or ID mismatch rejects the table. `-fvisibility=hidden` otherwise hides generated functions.

There is no `dlclose` in this loader: failed/mismatched candidates stay mapped and successful tables are process-global. Source therefore supports the project requirement to use a fresh Android process per game session. ABI1 alone is insufficient for a common runtime: title assumptions remain in runtime headers/HLE/address metadata and the emitted library explicitly depends on `libmain.so`. The first extraction must preserve the AOT contract while defining a stable runtime export/adapter boundary; merely renaming either library does not establish compatibility between two titles.

## Thor launch gate and next small verification

Android 13/API33 exceeds minSdk30; ARM64 is the sole packaged ABI. Manifest requests Vulkan1.1, while renderer uses 1.3 core behavior and a defined Android extension fallback for older drivers. Thor driver capabilities, custom Turnip suitability, guest memory mappings and actual startup are pending device tests. Do not infer compatibility from manifest/compilation alone.

**Source behavior requiring action before launch:** `LasActivity.onResume()` calls `RootBoost.start()`. `RootBoost.enabled()` defaults `root_boost` to **true** if `su` exists; even its balanced mode writes GPU frequency settings. An unchanged APK can therefore change performance settings on a rooted target, contrary to workspace inventory rules. Parent must disable this in the owned baseline or seed the isolated app's `perf` SharedPreferences with `root_boost=false` before its first launch. Separate application ID alone does not disable it. No device mutation was attempted here.

The cheapest useful asset-free native compile check is the existing game entry translation unit, using only checked-in headers and the cached NDK. Parent may run this with output outside the reference checkout:

```bash
cd "$WORK"
mkdir -p "$BUILD_EVIDENCE/native-probe"
"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang" \
  -O2 -fPIC -fvisibility=hidden -fsigned-char -Iruntime \
  -c tools/gamelib_entry.c -o "$BUILD_EVIDENCE/native-probe/gamelib_entry.o"
"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf" \
  -h -s "$BUILD_EVIDENCE/native-probe/gamelib_entry.o"
```

Expected limited result: ARM64 relocatable object with default-visible `lasgame_tables` and unresolved generated table symbols. This checks the installed compiler, Android headers and entry ABI declarations, not a linked runtime or an executable game. Follow with dependency preparation, a full isolated launcher APK, static ELF/APK inspection, then controlled launch with RootBoost disabled; parent owns builds and installation. No build result is claimed by this audit.
