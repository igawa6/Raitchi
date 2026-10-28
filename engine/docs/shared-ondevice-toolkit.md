# Shared Android build toolkit

`tools/prepare_shared_ondevice.py` prepares actual ARM64 Android executables and Python extensions. It does not access a device or include game files, keys, generated game C, a title library, or `libmain.so` in its asset archive.

The existing LAS generator virtual environment and pip wheels were host binaries. The required Android inputs were absent from local caches. The exact upstream pins were available and downloaded with SHA256 verification: CPython 3.14.8's Android package; Zig 0.17.0 for ARM64 Linux and the build host; Capstone 5.0.9, Unicorn 2.1.4 and python-lz4 4.4.5 source archives; and the upstream LLVM/Unicorn license texts. Together these inputs occupy 139,179,117 bytes. No alternative versions were substituted.

## Reproduce

```sh
python3 tools/prepare_shared_ondevice.py
python3 tools/prepare_shared_ondevice.py --fetch --build --jobs 2
python3 tools/prepare_shared_ondevice.py --package-only
```

The first command inventories verified local inputs. Fetching is explicit and serial; the build is offline and accepts one or two CPU jobs. `--cache-dir` can supply previously downloaded inputs by their pinned filename. `--ndk` overrides the cached NDK r27c location. `--package-only` verifies the existing stage, refreshes the producer script and runtime-bound metadata, and repacks without rebuilding Python or Zig.

All heavy output stays under ignored `build/shared-ondevice/`. The utility copies the reviewed pinned Python/Zig recipes into its own work directory, caps their CPU jobs, and blocks unexpected recipe downloads. The original source checkouts and baseline caches remain untouched. The native Python launcher uses the upstream isolated configuration, fixed hash seed, unbuffered output, and `$ORIGIN` library lookup.

## Integration contract

`toolkit.json` is the APK packager index. Its `native_libraries` map contains workspace-relative source paths, hashes and sizes for `liblaspython.so`, `libpython3.14.so`, `libcapstone.so`, `libunicorn.so` and `libzig.so`. Its `assets` map names `ondevice.zip`. The regular APK build supplies the identical common runtime separately.

Extract `ondevice.zip` into the app's internal `files/ondevice` directory. It contains:

- `pyhome/lib/python3.14/`: trimmed standard library, native modules and generator dependencies.
- `generators/{las,eevee}/`: each pinned generator's Python sources and public configuration headers/tables.
- `cckit/`: trimmed matching Clang headers, NDK Android API 30 headers/stubs/CRT/builtins, common CPU/ABI headers, title-specific SIMD headers, and linker flags.
- `shared_ondevice_build.py` and `shared-build-kit.json`: the producer and its complete file hashes, original-module metadata, common contract and runtime hash.
- `licenses/` and a public dependency self-test.

Java reads `python_home` and `python_paths` from the kit metadata to set `LASPY_HOME` and `LASPY_PATH`. Capstone and Unicorn require `LIBCAPSTONE_PATH` and `LIBUNICORN_PATH` pointing to the app's native library directory. Generator subprocesses also need their selected generator's `tools` directory on `LASPY_PATH`; the producer owns that setup.

The native programs must be extracted into `ApplicationInfo.nativeLibraryDir` by APK installation. Android app-data execution restrictions make that essential for the static Zig executable. Python extension modules remain in the unpacked internal kit and are loaded as libraries. The existing upstream `liblaspython.so` name is retained as an executable compatibility filename; it does not select a title.

## Validation and limits

Host preparation successfully built the Android launcher, ARM64-only Capstone, AArch64-only Unicorn, and python-lz4 extensions. All five native toolkit files identify as AArch64 ELF. Matching host Zig compiled and linked a public fixture using the staged minimal headers, common CPU memory/exclusive helpers, compiler builtins, and the actual common runtime for both title profiles.

A separately authorized Thor shell test then passed with these exact toolkit binaries: Python imported and used Capstone, Unicorn and LZ4; both pinned generators ran `--help`; Zig Clang/LLD 22.1.8 compiled and linked the public CPU3 exclusive-helper fixture on-device; Python `ctypes` loaded that library and verified a successful atomic increment from 41 to 42. The target reported ARM64 and 4 KiB pages. Exact commands, toolkit hashes and logs remain in ignored local evidence. Only a marked temporary directory was used and cleaned up; no application, input or performance setting was changed.

The verified Capstone 5.0.9 source archive retains `CS_VERSION_EXTRA = 7` in its Python binding, so that binding reports `5.0.7` during the smoke test. The archive hash and source version are recorded independently; no package substitution occurred.

Before adding the producer, the asset archive measured 10,718,884 bytes and the five native tools together measured 181,066,544 bytes. The installed toolkit APK measures approximately 138.5 MB and contains no games. Final archive sizes and all hashes are authoritative in `toolkit.json` and `build.json`; Zig accounts for 171,691,896 native bytes. APK compression and installation storage differ from these sizes.

The installed shared launcher includes the `:build` service. An initial app-UID run completed real LAS SDK generation, then generated 187 main-module units before the UI cancellation test. Cancellation succeeded and the previous compiled library remained ready. The bundled final producer has SHA256 `f7163dbd66573e9bb2ee45ed2bbb82469515abebbf1c14824405f03b4c1815e5`.

A subsequent full LAS build under the app's UID completed 773/773 compilation units, all 773 compiled and none reused, in approximately 628 seconds. Battery temperature started at 37 °C and peaked at 38 °C; this is the battery sensor, not CPU/GPU temperature or a power measurement. The sampled sum of RSS across the app UID peaked at approximately 681 MiB and may count shared pages more than once. The highest observed single-process memory high-water mark was approximately 536 MiB. Private measurements and receipts are indexed by `evidence/ondevice-full-las-current.json`; these are one measured run, not a general performance guarantee.

The resulting on-device-built library booted visible opening video at 30 fps, and its actual mapped-file hash was verified with both the original runtime and a subsequent interim runtime. Its original build receipt remains immutable and identifies the runtime used during compilation; the later boot verification does not rewrite that provenance. The owner initially reported unresponsive physical controls and Configure inputs, then reported “think its working now.” Subsequent coordinator probes using balanced controller D-pad/A events and touch opened configuration. No hidden input fix is claimed, and these startup checks do not establish full gameplay support. Eevee subsequently completed all 973 units through the installed service. The preparation utility itself performs no device actions.

The final LAS library subsequently passed an additional short route with the owner's selected newer Turnip: Mabe Village/BowWow at 30 fps, controller-node menu navigation and stick movement, a manual save to previously empty File 2, reload of that save in a new process, and short home/sleep-resume cycles. These are observed checks with injected events through the actual controller device; physical control feel and audible output still depend on owner testing. File 1 and the original reference installation were preserved.

## Parallel compilation and APK updates

The owner requested faster multicore compilation. The app now selects up to four compiler workers: four with at least 12 GiB total and 3 GiB available memory, two on smaller devices, and one under memory pressure or below 1 GiB available. The choice is capped by the available CPU count. `builds/active.json`, the launcher and the notification report the selected count. Generation remains serial; compiler subprocesses run concurrently. This is a bounded worker policy, not CPU affinity or a promise of linear speedup.

`tools/shared_parallel_build.py` is a separately verified APK asset, outside the frozen toolkit inventory. It verifies the original producer hash, delegates its path/title/timeout validation, and uses its unchanged generation, compilation, cancellation and publication functions. The producer remains byte-identical, so changing the worker policy does not change generated-source identities.

The first device update exposed a further cache miss: Zig reports Android's randomized app installation directory in its version text. The adapter permits reuse when that one `InstalledDir` line is the only specification difference. It verifies the original key, object bytes and every dependency with the original validator, then publishes a copy under the current key with the complete old record in `compatibility_reuse`. Original records remain untouched; the current compiler path and version remain recorded. Changes to compiler bytes, other version text, toolkit files, generation, source, headers or flags remain cache misses.

Four focused host tests cover actual four-task concurrency through the unchanged producer pool, cancellation and path validation, APK relocation reuse using the real compiler-version parser, original-provenance retention, and rejection of changed or corrupted inputs. These use controlled compiler fixtures. On Thor, the corrected adapter reused all 303 completed Eevee objects after the APK update, then ran four simultaneous Zig compiler processes. A scheduling snapshot allowed all eight CPUs and observed the workers on distinct cores 3, 4, 5 and 6. The corrected four-worker resume completed all 973 units in approximately 896 seconds: 303 reused and 670 newly compiled. This is a resume measurement, not a cold four-worker build comparison; earlier two-worker work, a rejected resume and update/testing time are separate phases. The sampled UID RSS sum peaked at approximately 1,620 MiB and the highest observed process high-water mark was approximately 1,679 MiB. Battery readings remained at 39 °C in that final phase. The immutable library and sidecar were atomically published, with private receipts indexed by `evidence/ondevice-full-eevee-current.json`. That published Eevee library then booted readable controller selection/confirmation and the opening movie at 30 fps, then loaded the existing save into Oak’s lab. The actual mapped file hashes verified both on-device-built titles using identical final runtime bytes in fresh processes.

Licenses are included from verified upstream inputs: Python PSF-2.0, Capstone BSD-3-Clause, Unicorn GPL-2.0 with BSD Python bindings, python-lz4/LZ4 BSD, Zig MIT with LLVM's Apache-2.0 exception and musl notices, and NDK notices. The complete Unicorn source archive used for the build is retained in the preparation cache. Public generator source attribution is retained separately for LAS and Let's Go.
