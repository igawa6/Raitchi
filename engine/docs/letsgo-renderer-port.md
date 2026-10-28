# Let’s Go renderer Android/ARM64 portability patch

The owned `work/letsgo-android` checkout has a renderer portability patch against Let’s Go commit `96894fd8e9ea20c1b38cdea53105ec2a335df620`. The reproducible diff is [renderer.patch](../ports/letsgo-android/renderer.patch). It covers Vulkan `exec.c`, `vk.h`, `vma.cpp`, `device.c`, `shaders.c` and Maxwell `translate.c`, `shader.h`; the pinned `sources/` tree is unchanged. Later bounded fixes below address evidence from the separate Android reference.

This is preparation for an asset-free native `libmain.so` link. It retains Let’s Go’s renderer, queues, formats, shader pipeline and ABI3 code contract. It neither changes the unshifted guest layout nor loads game code. No device, driver selection or display operation was performed while preparing it.

Changes:

- `exec.c` limits the x86 intrinsics include, AVX2 scanners and CPU-feature dispatch to x86 builds. ARM64 uses the existing scalar 16/32-bit index scans, including primitive-restart exclusion. The scalar algorithms were retained verbatim. Its asynchronous-copy wait loop retains x86 `pause`, uses AArch64 `yield`, and falls back to `SDL_Delay(0)` elsewhere; the cached SDL headers do not expose `SDL_CPUPauseInstruction`.
- Android `vk.h` includes volk declarations; desktop builds retain Vulkan prototypes. After SDL creates the Vulkan window, `create_instance()` initializes volk using **SDL’s own** `vkGetInstanceProcAddr` by default, or the explicitly requested AdrenoTools private loader described below. The SDL stock surface path remains the default. Instance commands are loaded immediately after instance creation, before debug setup or enumeration; device commands are loaded immediately after device creation, before queue/allocator operations.
- Android VMA uses static functions off / dynamic functions on. Its allocator receives `vkGetInstanceProcAddr` and `vkGetDeviceProcAddr` through `VmaVulkanFunctions`. Desktop VMA settings remain unchanged.
- Android device setup queries a separate support chain for the requested Vulkan 1.1/1.2/1.3 features and advertised optional extension features. Missing dynamic rendering, synchronization2 or timeline semaphore support is fatal. Other requested bits are filtered and logged before device creation; custom-border, instance-divisor, device-fault, pipeline-statistics and present-wait runtime flags follow the filtered values. Extension names may remain enabled with their feature bits false. Desktop feature setup is preserved.

The Vulkan 1.3 device/API requirement remains. This patch does not add the LAS backend’s Vulkan 1.1/1.2 extension compatibility route. Disabling an optional unsupported feature prevents requesting an unavailable bit; it does not prove that every guest shader or rendering use has equivalent fallback semantics. That remains a runtime/rendering acceptance gate.

The parent CMake target must provide:

```text
VK_NO_PROTOTYPES
VK_USE_PLATFORM_ANDROID_KHR
IMGUI_IMPL_VULKAN_USE_VOLK
```

Compile the existing cached `volk.c` exactly once and add its `include/volk` directory so `<volk.h>` resolves. Keep the cached Vulkan/VMA headers, SDL3 Vulkan-capable shared library, shaderc/FFmpeg libraries and existing C++ runtime configuration. All Vulkan-using translation units and ImGui must share the dynamic-loading definitions. The custom-driver follow-up additionally requires the cached AdrenoTools libraries below; no new JNI bridge, external downloads or static Vulkan 1.3 loader imports are introduced.

## Explicit custom-driver path

The coordinator subsequently booted all four guest modules on the stock driver. The inspected controller-selection screenshot shows its images at the expected orientation but missing guest text. LAS’s previously observed Turnip workaround motivates this bounded driver comparison; it does not establish the cause of the Let’s Go defect or the result of that comparison.

Android `device.c` now supports an explicit AdrenoTools custom-driver request, using LAS’s private-loader plus matching Android-native-window surface pattern:

| Environment variable | Contract |
| --- | --- |
| `LARECOMP_GPU_DRIVER_DIR` | Absolute directory containing the driver in this app’s internal files, such as its `files/gpu_driver/current/`; not external/shared storage |
| `LARECOMP_GPU_DRIVER_LIB` | Driver library basename, required with the directory |
| `LARECOMP_GPU_DRIVER_HOOKS` | Optional absolute override for the installed app’s actual `nativeLibraryDir`; otherwise derived from the loaded `libmain.so` path via `plat_exe_path()` |

Unset all three for the original SDL stock-loader/surface path. Any supplied variable constitutes an explicit request; incomplete or empty configuration fails clearly. Directories are normalized with trailing slashes because AdrenoTools concatenates them with filenames. The requested driver and both hook libraries must exist and be readable.

Use the cached pinned AdrenoTools source/include at revision `8fae8ce254dfc1344527e05301e43f37dea2df80`. Add its `include` directory for `<adrenotools/driver.h>` and link cached `libadrenotools.a` followed by its `lib/linkernsbypass/liblinkernsbypass.a` dependency, with the existing Android/dl/C++ libraries. Package the matching `libmain_hook.so` and `libhook_impl.so` beside `libmain.so`, with `useLegacyPackaging=true` so the files actually reside in `nativeLibraryDir`. The hooks directory must be the installed native library directory, not a convenient private copy. The coordinator owns packaging and staging the already verified driver into the separate app.

The loader call uses only `RTLD_NOW | RTLD_LOCAL` and `ADRENOTOOLS_DRIVER_CUSTOM`. API30 supports its memfd path, so no temporary patch directory is needed. No turbo calls, clock changes, file redirects, GPU memory mapping flags or changes to game/CPU code identity are introduced. The private loader remains alive for the process.

The custom path resolves its own `vkGetInstanceProcAddr`, initializes volk with it, and creates the surface through that loader’s `vkCreateAndroidSurfaceKHR` using SDL’s `ANativeWindow`. Passing this instance to SDL’s stock-loader surface creator would cross incompatible loader boundaries, so the helper chooses the corresponding surface path. Missing configuration/files/entry points or failed instance/surface initialization stops startup; the application does not silently retry the stock driver for an explicit request.

AdrenoTools itself documents possible hook/namespace failures that may expose a system driver despite returning a loader handle. Device selection therefore logs the actual Vulkan driver name, info and ID; the coordinator must confirm the expected Turnip identity before treating a run as a custom-driver result. Loading a handle alone does not prove the requested driver rendered the frame. This follow-up changed only renderer `device.c`, with no CPU, generator or root-runtime-header edits. No device run or full build was performed by this task.

## Launcher orientation correction

On 2026-10-25 the coordinator reported successful stock-driver launcher rendering in the dedicated Android reference, with the complete original guest window reserved. The inspected screenshot shows quarter-turned and stretched launcher content inside a landscape surface. This identifies an orientation problem in the rendered layer; it is not a game-execution result.

The Android swapchain now follows LAS Android’s non-prerotated (`LARECOMP_PRETRANSFORM=0`) path: select `VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR` when advertised, retain the window-shaped surface extent and normal vertices, and let the compositor rotate the layer. Previously `preTransform=currentTransform` declared that the renderer had already rotated its output, although Let’s Go has no corresponding vertex/UI rotation. The desktop transform policy is unchanged.

Android also recognizes the intentional identity/current-transform difference when presentation returns `VK_SUBOPTIMAL_KHR`; it avoids rebuilding the same swapchain every frame for that known mismatch. Window-size changes and `VK_ERROR_OUT_OF_DATE_KHR` still recreate it. The mismatch flag is captured under the existing swapchain lock for the presenting thread. Logs record image/surface/window dimensions and current/selected transforms for the coordinator’s next run.

Full shader/UI pre-rotation is outside this fix. A surface requiring a nonidentity transform without advertising identity support fails clearly instead of silently displaying rotated content. No game, input or shader code changed. This correction was reviewed against the screenshot and LAS source, with patch checks only; rebuilding and device confirmation belong to the coordinator.

## Read-only shader guest-memory fallback

The coordinator’s next capture established that glyph pixels exist in the guest R8 atlas and the instanced glyph draw is present, while text remains missing on both stock and verified Turnip. Its captured vertex shader loads per-instance position, packed color and UV records through global-memory loads. Upstream Let’s Go binds a 4 KiB zero buffer when whole-heap host import is unavailable; LAS Android instead supplies a mapped-range mirror. The missing-data path therefore explains how a valid atlas can still produce invisible glyph geometry/color. The reference logs contain no successful whole-heap import and report no shader int64 support.

The Android fallback implements the read-only part of LAS’s `mirror_table` model, retaining Let’s Go’s backend:

- Android selects it when either whole-heap host import or shader int64 is unavailable. Desktop host-import behavior is retained. The existing GLSL driver constants still contain the original guest storage-buffer pointers.
- Each descriptor set snapshots only its stage’s bound storage ranges; compute also includes bound uniform ranges. Word-aligned ranges are sorted and merged, then packed into the existing submission-owned host-visible transient ring. There is no whole-heap copy, buffer device address, persistent GPU writeback store, or separate mirror allocator.
- SSBO binding 17 references that compact snapshot. A new std140 GMap at binding **21** maps heap-relative byte ranges to packed word offsets; binding 20 remains reserved for existing jitter. The buffer offset respects `minStorageBufferOffsetAlignment`; the metadata respects uniform-buffer alignment. Both writes explicitly flush noncoherent allocations, and the ring’s submission retirement preserves their lifetime.
- Per-set snapshots are limited to the smaller of 16 MiB and `maxStorageBufferRange`. Empty tables read zero; addresses absent from the table also read zero with explicit bounds checks. Invalid heap ranges and oversized snapshots fail clearly instead of making an invalid descriptor or silently omitting a required range.
- Mirrored LDG uses 32-bit low/high carry/borrow arithmetic and a table lookup, including byte/short extension and multiword reads. When shader int64 is unavailable, immediate/three-input carry additions and funnel shifts also use 32-bit helpers, and the generated GLSL drops the int64 extension requirement. These helpers preserve the original bit patterns and carry convention.
- Shader global **stores** are explicitly rejected during mirror-path translation. GPU writeback, CPU/GPU word merging and correctness after storing dispatches are not implemented by this read-only fallback. Passing the controller-text gate does not establish later gameplay support.

Changed GLSL naturally changes its source hash; the SPIR-V cache generation also increments, and pipeline caches use a new `vk_pipelines_ro_map1.bin` filename. Old cache files remain intact. CPU code, generated game units, root runtime headers and game code identity remain unchanged. Use `LARECOMP_FONT_SCALE=1` for the first controlled comparison.

Host validation performed for this fallback: all four modified C translation units compile with the actual Android CMake flags. The captured glyph vertex/fragment instruction streams were reconstructed privately, translated through the modified C translator, and compiled to SPIR-V in desktop-int64, mirror-int64 and Android-uint32 modes. The Android-uint32 binaries contain no Int64 capability. An ASan/UBSan harness of the actual snapshot function verifies packing, overlap merge, alignment, flushing, compute uniform ranges, empty snapshots and rejection of heap/range-limit violations. Emitted arithmetic helpers match 64-bit reference results across 20,000 carry cases and 1.3 million funnel shifts; 20,000 signed address-offset cases match the reference, and a storing shader is rejected as intended. These checks do not execute Vulkan or establish readable text on the device; the coordinator owns that acceptance run.

Coordinator device follow-up,2026-10-25: the controller prompt is now readable at
approximately30fps on both verified Turnip and the stock Adreno740 driver, using
the same private title library/profile and original font scale. This confirms
the missing-instance-data fix for that scene. The final stock-driver session
remains open for manual testing. GPU writes, later scenes and gameplay
performance remain separate gates; see [guest-boot results](letsgo-guest-boot.md).

The later constant whole-window blinking was a separate swapchain-count bug:
Android returned ten images but the desktop-derived renderer truncated storage
to eight. Image/view/completion-semaphore arrays now share a32-image bound;
enumeration results and acquired/presented indices are checked, with explicit
rejection above the bound. The actual returned count is preserved. Normal
mailbox presentation, render threading and GPU-semaphore acquisition are retained.
An optional CPU-acquire-fence diagnostic alone did not eliminate the flashing.
Corrected controller-page and title-movie captures each have0/240 entirely black
samples; see [presentation evidence](letsgo-presentation-fix.md).

The Android UI event loop now also follows LAS Android's event-poll/4ms idle-sleep
pattern. Profiling found SDL event waiting repeatedly polling the clock and
joysticks on a full core. A matched30fps controller-scene test reduces process
CPU from123.15% to27.02% without changing renderer/mutex settings. Desktop event
waiting is unchanged. This is CPU-time evidence, not a measured wattage or Eden
temperature comparison; see [load investigation](letsgo-power-investigation.md).

Recreate the tracked patch from the owned checkout with:

```bash
git -C work/letsgo-android diff --binary -- \
  runtime/nvn/vulkan/exec.c runtime/nvn/vulkan/vk.h \
  runtime/nvn/vulkan/vma.cpp runtime/nvn/vulkan/device.c \
  runtime/nvn/vulkan/shaders.c runtime/nvn/maxwell/translate.c \
  runtime/nvn/maxwell/shader.h \
  > ports/letsgo-android/renderer.patch
```

Apply it to an otherwise pinned owned checkout with `git apply`; the original `sources/` checkouts remain read-only. Whitespace validation and reverse-applicability checks pass against the edited checkout. The coordinator owns full native build/link validation. A targeted full-translation-unit Android compile of `exec.c` passed with no diagnostics using the actual CMake compilation-database flags, with only its output object redirected to a separate probe file. No Android execution, feature-filter behavior on a real driver, stock-driver rendering correctness or game support is established by the patch.
