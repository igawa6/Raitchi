# Minimal Let's Go Android reference bring-up

Source review, 2026-10-24: Let's Go commit `96894fd8e9ea20c1b38cdea53105ec2a335df620`, compared with the existing owned LAS Android baseline. This is a file-level implementation recipe, not a build result. No downloads, compilations or device actions were performed. Thor's owner-controlled manual session remains protected.

## Recommended boundary

Build a separate owned Let's Go reference checkout and **retain its complete ABI3 runtime, four-module loader, dispatcher, hooks, NVN front end and Vulkan renderer**. Port the Android host/bootstrap pieces into it. Reuse the already built LAS Android dependencies and SDL3 source tree; do not merge the two renderers or replace Let's Go's CPU structures to obtain the first reference. This reference can precede the common runtime; two independently compiled references do not satisfy the identical-artifact milestone.

The cheapest first milestone is an asset-free **ARM64 `libmain.so` native link**, followed by a separate APK/launcher. It does not require generating Eevee code. Address translation and generated title code become mandatory before real guest execution, not before this compile/link milestone.

## File-level port recipe

| Owned Let's Go file / new file | Minimum change |
|---|---|
| `android/app/CMakeLists.txt` (new) | Start from the LAS target/dependency mechanics, but compile **Let's Go's** source lists and generated includes described below. Accept a `RECOMP_ANDROID_DEPS` prefix and `RECOMP_SDL_SOURCE` instead of rebuilding/downloading dependencies. |
| `runtime/platform.h` | Exclude `__ANDROID__` from the `__linux__` inclusion of `platform/linux_compat.h`. Android defines `__linux__`, but bionic is not glibc. Keep Let's Go's existing platform API. |
| `runtime/platform/android.c` (new) | Adapt Let's Go's Linux backend to Android using the LAS mmap, unwind/log and `dladdr` patterns. Preserve the **Let's Go** signatures `plat_watch_writes()` and `plat_written()`; safe initial implementations remain false/−1 as in its Linux baseline. Replace `<execinfo.h>`/`backtrace()` with the LAS `_Unwind_Backtrace` pattern. `plat_exe_path()` must identify `libmain.so` with `dladdr`, not Android `app_process` from `/proc/self/exe`. Make `plat_exec()` explicitly unavailable and fail callers clearly. Use an app-owned data/log path. |
| `runtime/main.c` | Include `<SDL3/SDL_main.h>` so the exported entry is `SDL_main`. Add an Android branch to the early guest reservation constructor, recording success and reporting failure later instead of desktop re-exec. Add the short Android `env.txt` reader after paths initialize and then reevaluate launcher skip. Keep all four module init calls and Let's Go's NPDM-derived program ID. |
| `runtime/paths.c` | Before generic `__linux__` handling, choose SDL's Android external app-specific path, with internal fallback; preserve Let's Go's `LARECOMP_DATA_ROOT` override. Add APK resource extraction/lookup using the small Android resource pattern from LAS for icon/Baloo/DejaVu. Do not inherit a LAS save/data directory. |
| `runtime/nvn/vulkan/exec.c` | Guard `<immintrin.h>`, both `scan_max*_avx2()` definitions and the `__builtin_cpu_supports("avx2")` dispatch with an x86 condition. The two scalar index-scanning loops already implement the fallback; select them directly on ARM64. No NEON optimization is required for bring-up. |
| `runtime/nvn/vulkan/vk.h`, `vma.cpp` | Use cached volk with `VK_NO_PROTOTYPES`; make `vk.h` expose volk declarations, and select dynamic rather than static Vulkan functions in the VMA implementation. Configure ImGui with `IMGUI_IMPL_VULKAN_USE_VOLK`; its checked-in backend supports that macro. |
| `runtime/nvn/vulkan/device.c` | Add volk global/instance/device initialization at the correct create boundaries and give VMA `vkGetInstanceProcAddr`/`vkGetDeviceProcAddr` through `VmaVulkanFunctions`. Keep Let's Go's renderer state/queues. Copy **only** LAS's Android host surface/driver bootstrap, not its whole `device.c` or `VkG`. Query unsupported requested features before enabling them. |
| `runtime/gamelib.h`, `gamelib.c` | Android name `liblasgame.so`; search an app-internal generated library and/or a packaged bare library name. Preserve ABI3 tables and Let's Go's expected code ID. A packaged library must be loaded by name and validated through its entry; it has no ordinary filesystem `.id` alongside an unextracted APK entry. Internal file candidates can retain sidecars. |
| Minimal SDL activity/manifest/Gradle (new) | Reuse cached Gradle/AGP/NDK settings and SDL Java sources. Give Let's Go its own application ID/data directory. Load SDL3 then main. A thin activity is enough for the first host reference; avoid copying LAS import/setup/title metadata and RootBoost behavior. Driver selection can use the isolated generic `GpuDrivers` helper plus a narrow JNI bridge once ported. |

Do **not** copy the whole LAS `platform/android.c` unchanged: it additionally needs `PlatSysInfo`, core-role APIs, `plat_written_pages`, `rt_crash_explain`, Android dialogs and other declarations absent from Let's Go. Its JNI file also embeds the LAS Java class path and many optional import/build/save/font APIs. A thin adapter implementing the smaller Let's Go platform contract is less work for this baseline.

## Exact native target and cached inputs

Use NDK `27.2.12479018`, `arm64-v8a`, API30, C++17/static STL, and optimized debug (`RelWithDebInfo`). Existing reusable inputs under `work/las-baseline`:

- `build/android/SDL3`: SDL3 3.4.18 source and matching Android Java source.
- `build/android/deps`: FFmpeg9 static archives (`avformat`, `avcodec`, `swscale`, `avutil`), shaderc combined archive, Vulkan headers, VMA and volk. The configured FFmpeg already enables Opus, needed by Let's Go's `services/hwopus.c`; no separate libopus dependency is implied by that service.
- `build/android/deps/turnip`: bundled driver files if later needed. Reuse **its code/files**, not the active LAS app's settings or data.
- Cached AdrenoTools source exists under LAS's CMake `_deps/adrenotools-src` staging directory. If custom-driver support is included, supply this pinned source directly with its existing submodules; avoid a new FetchContent network operation.

CMake source inventory must follow Let's Go's Makefile:

```text
runtime/*.c
runtime/hle/*.c
runtime/services/*.c
runtime/nvn/*.c
runtime/nvn/maxwell/*.c
runtime/nvn/vulkan/*.c + *.cpp
runtime/platform/android.c                 # exclude linux.c/windows.c
runtime/ui/*.mm                           # LANGUAGE CXX, -x c++
imgui core + imgui_impl_sdl3.cpp + imgui_impl_vulkan.cpp
cached volk.c
```

Generate `smaa_hlsl.inc` using the existing Makefile/LAS CMake rule and add `third_party/smaa/Textures` plus the generated include directory. **Do not copy LAS's shader-variant include/code-ID rules**: Let's Go has a different shader preparation/code-generation contract. Include Let's Go `third_party/fidelityfx/include`; `fsr3.cpp` already supplies non-Windows unavailable/false behavior, so it can compile without the Windows DLL. Keep optional FSR3 disabled; omitting this C++ unit outright would leave `fsr3_*` references unresolved unless replaced by an explicit stub.

Link `main` to SDL3-shared, the five cached native archives above, Android/log/m/dl, and AdrenoTools only if its loader is included. bionic supplies pthreads. Keep runtime functions exported for the generated title library; use LAS's `--exclude-libs,ALL`, `-Bsymbolic`, `-z,nostart-stop-gc`, `-z,max-page-size=16384` pattern and verify the `hle_impls`, `services`, `resets` registries survive linking. Keep the game library separate, absent from this first native target.

Proposed reproducible first command **after the owned CMake recipe exists** (not currently runnable/unmodified-source build instructions):

```bash
cmake -S "$LETSGO_WORK/android/app" -B "$LETSGO_BUILD/native" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 \
  -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DRECOMP_ANDROID_DEPS="$LAS_WORK/build/android/deps" \
  -DRECOMP_SDL_SOURCE="$LAS_WORK/build/android/SDL3"
cmake --build "$LETSGO_BUILD/native" --target main -j4
```

The flags named `RECOMP_*` above are the proposed new CMake interface. Static review did not execute this command or rule out additional compiler diagnostics. The source has no supported Android Makefile target; overriding `CC`/`ARCH` alone does not create an APK-compatible build.

## Hard blockers versus execution gates

| Finding | Phase / concrete consequence |
|---|---|
| `linux_compat.h` uses `__GLIBC_PREREQ`, and defines APIs already supplied by bionic | **Compile blocker** when included through Android's `__linux__`; fix the platform guard rather than redefining glibc macros |
| Linux `<execinfo.h>` plus unconditional AVX2/intrinsics/CPU feature builtin | **Compile blockers**; Android unwind adapter and existing scalar fallback address these directly |
| Static Vulkan1.3 entry-point references and static VMA | **Link blocker** with an API30 Android loader import library: newer commands must be resolved dynamically. Read-only `llvm-readelf` inspection of the cached r27c API30 ARM64 `libvulkan.so` found `vkCreateInstance`/`vkGetInstanceProcAddr` but no `vkCmdBeginRendering`, `vkCmdEndRendering` or `vkQueueSubmit2`. Linking the desktop `-lvulkan` recipe is insufficient. Volk avoids a newer-minSdk requirement. |
| Missing `SDL_main` export / resource and data-root handling | Runtime bootstrap/packaging gates; a native link can succeed while SDLActivity cannot start the code or paths resolve to desktop locations |
| Unfiltered Vulkan1.1/1.2/1.3 feature requests in Let's Go `device.c` | Device-creation gate. The observed stock Thor lacks some requested bits; copy LAS's query/filter behavior rather than assuming API1.3 means every feature is available. |
| No Android custom-driver loader/surface path | Rendering/driver gate. LAS's observed Turnip workaround is a useful candidate for Let's Go, **not** verified compatibility. A custom loader requires its matching surface creation path; SDL's stock-loader surface cannot be assumed interchangeable. |
| Unshifted high guest window and hard-coded guest addresses | **Major guest-execution gate**, described below; not solved by a successful runtime link |
| No Android title-library build/load route | Guest-loading gate. Build four ARM64 AOT modules against the **same ABI3 headers/runtime**, link against its `libmain.so`, and validate module counts/exports/ID before entry. LAS's existing ABI1 game library is incompatible. |

## Address contract: do not hide this behind compilation

Let's Go reserves `[0x7000000000,0x7320000000)` and directly equates guest/host pointers (`rt.h`, `kernel.h`, `cpu.h`). It has **no `GUEST_SHIFT` facility and no `gen.py --shift` option**. Its generated function addresses, absolute literals, hard-coded hooks/mappings, instruction addresses and guest-pointer range checks use canonical `0x70...` addresses. For example, `fnhook.c:guest_ptr()` checks `0x7000000000..0x7800000000`.

Keeping these addresses is acceptable for a **compile-only reference** and preserves upstream ABI3 behavior. It is not a reliable Android execution plan: Android app/zygote mappings can occupy that high window, and `plat_exec()` cannot obtain a new zygote layout. Before guest execution, either demonstrate safe reservation of the *entire* original window in the dedicated app or implement the consistent Android shift across generator, loader/relocations, kernel ranges, dispatch/hook keys and title pointer/address constants. A one-page shell probe and changing `MAIN_BASE` alone are insufficient. Reusing LAS's shifted generator without Let's Go's ABI3/four-module/function-hook semantics is also insufficient.

This is where the planned common CPU/module/address contract can reduce duplicate work. Preserve an explicit choice of canonical versus mapped address at every boundary; do not let a temporary baseline silently mix the two. All four original module bases are required: main `0x7000000000`, SDK `0x7004000000`, subsdk0 `0x7006000000`, subsdk1 `0x7008000000`. Initialization stays SDK/subsdk0/subsdk1 then main.

## Later semantics, outside the first compile

The reference still needs Android input/profile qualification (Let's Go controller/gesture/six-axis behavior), correct title-specific hooks, skinned models and texture updates, queue/fence/render-thread behavior, movies/Opus/audio, save isolation and reload, and lifecycle recovery. Native shader compilation and full static-import closure do not prove these. Keep optional 60 fps, wild-battle changes, FSR3, interpolation and other visual/gameplay features off for the first comparison.

Next host-only sequence: prepare the owned port, fix the narrow compile blockers, obtain a no-game runtime link, inspect ELF exports/undefined imports/registries, build a separate asset-free APK, then address the AOT/guest-address contract. No install, app stop, input injection or GPU-driver change is part of this recipe while the owner's manual Thor session is active.
