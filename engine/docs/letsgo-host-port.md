# Let's Go Android host port

Implemented in the owned `work/letsgo-android` checkout against pinned Let's Go commit `96894fd8e9ea20c1b38cdea53105ec2a335df620`. Reproducible changes: [host.patch](../ports/letsgo-android/host.patch). Sources under `sources/` remain unchanged. The new Android platform layer is adapted from the upstream MIT-licensed Linux and LAS Android implementations; upstream `LICENSE` and `LICENSE-LASRecomp` attribution stays intact. No third-party libraries or their licenses were modified.

The port preserves **ABI3, its 1024-byte Cpu, all four module tables, and the original unshifted guest address window**. It does not implement the common runtime or Android address translation.

Changed files:

- `runtime/platform.h`: Android no longer includes glibc compatibility definitions through `__linux__`.
- `runtime/platform/android.c`: implements the existing Let's Go platform API with checked fixed-address reservation, whole-window reset, bionic process helpers, monotonic waits, `dladdr`, NDK unwinding, ARM64 signal context and app-owned file logging. Untracked writes return false/−1 as in the upstream Linux baseline. It does not change CPU/GPU frequency, priority or scheduler policy; no Java/JNI or root APIs are required.
- `runtime/main.c`: exports `SDL_main`, records Android reservation failure without re-exec, reads data-root `env.txt`, and refuses startup before loading guest code if reservation failed. Environment lines use `getline`, avoiding the earlier LAS 1024-byte script-line limit. Its `realpath` output buffer uses `PATH_MAX`, as bionic fortify requires. Desktop startup retains its existing path.
- `runtime/paths.c`: chooses the calling Android app's external storage, with internal fallback; the process environment's `LARECOMP_DATA_ROOT` override remains supported. Extracts optional `Baloo2-SemiBold.ttf`, `DejaVuSans.ttf`, `icon.png` assets into internal `res/` using SDL streams and a temporary file. Existing extracted resources are reused. It adds no title-data import or on-device compiler.
- `runtime/gamelib.h` / `.c`: Android uses `liblasgame.so`. Lookup is explicit `LARECOMP_GAME_LIB`, app-internal library, then packaged bare name. Internal files retain sidecar checks; packaged entries use the ABI3/code-ID check in `lasgame_tables`. Android can retry lookup while the launcher is up. Generated game code is not included.

Integration requirements: compile `runtime/platform/android.c` instead of `linux.c`/`windows.c`; use the NDK Android ARM64 target and cached SDL3 headers/library. Link Android `log`, `dl`, `m`, normal NDK unwind support and SDL3. No extra host-specific preprocessor definitions or LAS platform APIs are introduced. APK assets should carry the three named repository fonts/icon; missing optional resources are skipped. This does not replace the independent Vulkan/CMake port.

The native file log is `<data root>/lasrecomp.log`, with the previous file retained as `.prev`. Logcat tag `LetsGoRecomp` announces the path and reports fatal notices; it does not mirror every file-log line. `env.txt` is read after data-root selection, so `LARECOMP_DATA_ROOT` inside that file cannot move the already selected directory. Packaged title code must be built against this runtime's ABI3 headers and expected `GAMELIB_ID`; renaming a LAS ABI1 library is insufficient.

Validation performed host-only:

- Individually compiled `platform/android.c`, `main.c`, `paths.c`, `gamelib.c` with NDK r27c `aarch64-linux-android30-clang`, `-O2 -g -fPIC -fsigned-char -fno-strict-aliasing -Wall -Wextra -Werror -Wno-format -Wno-unused-function`, using cached SDL3 3.4.18 public headers. All four passed. Objects remain ignored under `build/letsgo-host-probe/`.
- Initial standalone checks omitted bionic fortify; the integrated build exposed the upstream 1024-byte `realpath` destination in `main.c`. After changing it to `PATH_MAX`, all four owned translation units passed their actual CMake `compile_commands.json` commands, including `-D_FORTIFY_SOURCE=2 -O2`, Android sysroot, stack protection, format-security checks and integration definitions. Only object destinations were redirected to ignored `build/letsgo-host-probe/fortify/`. The other owned `realpath` destination in `paths.c` already used `PATH_MAX`. No complete build was run by this host-port check.
- `llvm-nm` confirmed global `SDL_main` in the startup object.
- `git diff --check` passed for owned tracked changes. `git apply --check ports/letsgo-android/host.patch` against the pinned reference passed, including new `android.c`; no patch was applied to the reference.

No full runtime link, APK, Android execution, signal handling, resources, library loading or gameplay is claimed by these checks. The owner-controlled Thor session was not queried or changed. Before any future guest execution, the complete original `[0x7000000000,0x7320000000)` window must be safely reservable or a consistent shift must be implemented; this port deliberately fails rather than overwriting occupied memory or trying to re-exec Android's app process.
