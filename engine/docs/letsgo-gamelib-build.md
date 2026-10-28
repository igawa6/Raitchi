# Private Let's Go Android game library

`tools/build_letsgo_gamelib.py` builds the complete four-module AOT library for the owned Android runtime. It never downloads dependencies, builds/packages an APK, or contacts a device. Generated code, inputs and every output must remain private.

Validate only:

```bash
python3 tools/build_letsgo_gamelib.py
```

Compile/link with the maximum four concurrent compiler jobs:

```bash
python3 tools/build_letsgo_gamelib.py --build --jobs 4
```

`--jobs` accepts 1–4; `--ndk` can select the installed r27c directory. Defaults use the existing generated tree in `work/letsgo-baseline/build/android-codegen`, the Android headers/entry in `work/letsgo-android`, private imported NSOs, and `build/letsgo-android/native/libmain.so` with its native manifest. `--alternate-clang` defaults to the already installed `/usr/bin/clang`; it is used only for the explicitly selected per-unit variant below, with the NDK sysroot and an Android target.

Before compilation, the utility checks:

- The generation fingerprint, unshifted ARM64 SIMD-asm profile, ABI3 and 1024-byte CPU contract.
- Recorded generation-source hashes and exact agreement of generator Python, CPU/SIMD headers, HLE list and entry source between generation and Android checkouts. Deliberately adapted platform/library headers are checked through the Android runtime identity instead.
- All generated C hashes and the exact four-module file inventory/tables.
- Each private NSO's SHA256 and full 32-byte build ID against the prior private import/audit evidence; all four canonical bases match the generation manifest.
- The actual runtime library hash and its recomputed `game_code_identity`, using the same source/profile identity calculation as the native-runtime build script.

The default compiler is NDK r27c/API30 with `-O1 -fPIC -fvisibility=hidden -fsigned-char -fno-strict-aliasing`, and the runtime manifest's explicit `GAMELIB_ID`. It compiles all generated units plus the **Android checkout's fresh entry**, preserving optimization level O1 for every unit. It does not reuse the earlier `dev` entry probe. The entry additionally checks CPU size/alignment and ABI3/four-module table size at compile time. Upstream function-entry hooks and the 1024-byte CPU stay intact.

Actual compilation exposed one huge 6,666-case PC-switch unit, `subsdk0_289.c`, that remained CPU-bound in NDK18 after many minutes. LICM-only and LICM-plus-scheduler-bound probes each exceeded 90 seconds. A separately bounded **Clang 23 Android-target** probe passed in 47.43 seconds with the same O1/ABI settings, explicit NDK sysroot, and these per-unit options:

```text
-mllvm -disable-licm-promotion
-mllvm -dag-maps-huge-region=500
-mllvm -opt-disable=loop-load-elim
-mllvm -has-predecessor-max-steps=2048
```

This one-unit policy is automatically initialized when no private policy file exists. The actual compiler executable hash/version, Android target/sysroot, header dependencies and exact extra options are stored with the object and in the final manifest. Existing valid standard objects always take precedence; the other 972 completed NDK objects were preserved and reused during the controlled restart. An independent `main_320.c` LICM-only probe passed in 30.57 seconds, but its already completed original object was retained.

Ordinary uncached units have a default 120-second timeout, followed by one 180-second targeted LICM/scheduler retry. `--unit-timeout` and `--retry-timeout` adjust those limits. The selected per-unit flags are persisted in ignored `compile-policy.json`; these choices never change global flags or invalidate unrelated object keys. Compiler processes run in owned process groups so a timeout also terminates the NDK wrapper's child compiler. No O0 fallback is applied.

The content cache includes each source hash, CPU/SIMD/library header hashes, compiler driver/version/NDK metadata, code identity and compile flags. Compiler dependency files record hashes for **all** headers, including system headers; cached objects are reused only when these hashes and the object hash still match. Objects and metadata are published through temporary files and atomic rename. A process lock prevents two builders sharing the cache concurrently. Completed objects survive an interrupted or failed run; rerunning the same command reuses them. No wildcard of old objects is linked: the response file contains exactly the current validated units. Larger files are scheduled first to reduce the tail.

The final link uses the actual current runtime, `--no-undefined`, SONAME `liblasgame.so`, `-Bsymbolic` and 16 KiB maximum page size. This requires references to resolve through linked dependencies; it does not mean ELF's dynamic undefined-symbol table is empty. A runtime-only renderer rebuild during compilation is accepted only after its latest manifest/library and unchanged generated-code contract are revalidated before link. A changing runtime during link aborts publication.

Outputs under ignored `build/letsgo-gamelib/`:

| File | Purpose |
|---|---|
| `liblasgame.so`, `.so.id` | Private AOT library and runtime code identity sidecar |
| `build.json` | Full module IDs/hashes, runtime/library hashes, compiler/options, current object list and ELF verification |
| `progress.json` | Phase, completed/compiled/reused counts and errors |
| `objects/` | Resumable atomic object/metadata cache |
| `compile-policy.json`, `licm-probes/` | Targeted compile mitigations and private probe evidence |
| `logs/`, `link.log`, `driver.log` | Private per-unit, linker and invocation diagnostics; console progress prints counts rather than generated source |
| `library-elf.txt` | Read-only ELF inspection report |

ELF checks require ARM64 shared-object type, the intended SONAME, `DT_NEEDED libmain.so`, at least 16 KiB PT_LOAD alignment, exported `lasgame_tables`, and hidden generated `f_70...` functions. Linking/ELF inspection does not execute `lasgame_tables` or the game. Device input/import/installation, app loader behavior and gameplay remain separate parent-owned steps; this builder cannot interfere with an active manual Thor session.

Actual build completed on 2026-10-25: **973 objects** (972 generated units plus fresh entry), **144,223,296 bytes**, with the runtime code identity `4a34b9127ec182c1`. The controlled resumed invocation reused **972 objects** and compiled only the remaining targeted unit, then linked successfully with `--no-undefined`. ELF checks passed, including `DT_NEEDED libmain.so`, 16 KiB alignment and the exported entry. Completion is recorded by private `build.json` and `progress.json` state `complete`; invocation details are in `driver-resumed.log`. No APK or device action was performed by this builder.
