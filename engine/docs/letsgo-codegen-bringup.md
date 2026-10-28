# Let’s Go Android codegen checkpoint

Source review: 2026-10-24, Let’s Go commit `96894fd8e9ea20c1b38cdea53105ec2a335df620`. Keep its unmodified ABI3, four-module tables and 1024-byte `Cpu` for the initial reference. The [Android bring-up recipe](letsgo-android-bringup.md) covers the separate runtime/platform port; this note addresses generation, ARM64 compilation and the unresolved address choice.

## Observed checkpoint

The coordinator generated all four private imported modules with upstream `--simd asm`: 972 C translation units in total. Reported function counts are main 60,380, SDK 31,906, subsdk0 17,984 and subsdk1 13,342. It then compiled the first function unit and address table from each module, plus the ABI3 entry unit: nine successful NDK/API30 ARM64 compilations, with no diagnostics. These are coordinator-observed results; this source-review task performed no generation, compilation, downloads or device actions.

That result removes a speculative blocker: the sampled upstream generated code does not require an ARM64 generator patch merely to compile against its own headers. It does not establish compilation of every unit, a complete library/runtime link, Android loading, valid guest mapping or execution. The compile-only entry currently uses header-default `GAMELIB_ID="dev"`; it is not evidence of a matching production code identity.

## Reproducible recipe

From the workspace root, the existing host-only utility is the authoritative reproducible generation/probe path:

```bash
python3 tools/build_letsgo_codegen.py --generate --compile-probes --jobs 4
```

It checks source pins, uses the owned `work/letsgo-baseline` checkout, and reuses the cached LAS generator Python environment. Its checked package versions are Capstone 5.0.9, Unicorn 2.1.4 and LZ4 4.4.5. It downloads nothing. Generated code, hashes, logs and objects remain under ignored `work/letsgo-baseline/build/android-codegen`; no generated code is a public deliverable.

The inputs are the existing private extracted main/SDK/subsdk0/subsdk1 files. The exact per-module generator arguments are:

| Module | Canonical base | Arguments beyond input/output |
| --- | --- | --- |
| main | `0x7000000000` | `--simd asm` |
| SDK | `0x7004000000` | `--simd asm --hle runtime/hle/hle_symbols.txt` |
| subsdk0 | `0x7006000000` | `--simd asm` |
| subsdk1 | `0x7008000000` | `--simd asm` |

The wrapper passes absolute paths for the HLE list and runs the generator from the owned `tools` directory. Its equivalent invocation shape is:

```text
<cached-python> <owned-source>/tools/gen.py <module> <private-exefs>/<module>
    <canonical-base> <private-build>/generated/<module> --simd asm
    [--hle <owned-source>/runtime/hle/hle_symbols.txt, SDK only]
```

This follows the pinned [Makefile generation rules](../sources/letsgo/Makefile). The wrapper’s source/input/options fingerprint and generated-file hashes must remain current; changing mode, module inputs or headers requires regeneration. The generator’s host-based default would select portable C on this x86 host, so **explicit `--simd asm` is required** for this reference.

The nine probes use NDK `27.2.12479018`, compiler `aarch64-linux-android30-clang`, Let’s Go’s own `runtime` include directory, and these flags:

```text
-O1 -fPIC -fvisibility=hidden -fsigned-char -fno-strict-aliasing
-Wno-return-type -Wno-unused-label -I <owned-source>/runtime
-c <generated-unit-or-tools/gamelib_entry.c> -o <private-object>
```

`-fsigned-char` preserves the tested desktop plain-char assumption on Android ARM64. Use the NDK Android target rather than the Makefile’s generic `ARCH=aarch64` Linux cross target. Do not inherit `-march=x86-64-v3` or Linux desktop link flags.

The next larger code-build step, after the asset-free Android runtime link, is to compile **every** generated C unit with these same headers/options, compile `tools/gamelib_entry.c` with a non-`dev` code ID shared by that runtime, and link one private Android game library against that runtime’s exported services. Check all generated direct-call references, four function tables, required globals, `lasgame_tables`, and the runtime’s matching ID before packaging. The current wrapper implements generation and samples only; it has no full-runtime or full-game link target. A proposed link must use Android shared-library/16 KB alignment settings and the correct Android loader namespace rather than unresolved desktop executable symbols.

## SIMD and direct-call constraints

[The lifter](../sources/letsgo/tools/lifter.py) lowers integers/control flow to C and batches FP/SIMD/CRC instructions into AArch64 `.inst` inline assembly, loading/storing guest vectors at the audited `Cpu` offsets. Its scratch register set is x9–x17, leaving Android’s reserved x18 untouched. Guest TLS register accesses are lowered through `Cpu`; they do not overwrite the host thread pointer. [The CPU header](../sources/letsgo/runtime/cpu.h) supplies host FPCR/FPSR access on AArch64, and [thread startup](../sources/letsgo/runtime/kernel.c) resets FPCR. Keep those pieces together. Successful assembly does not establish that every emitted instruction is supported by the eventual host CPU: `.inst` emits encodings directly.

`--simd c` is an upstream-supported alternate lowering and explicitly includes `simd.h` in generated units even on AArch64. It stores guest FP state in `Cpu` and changes generated code and build identity. It is a later diagnostic alternative, not a necessary fix suggested by the successful asm samples. Do not mix modes silently or infer runtime FP equivalence from compilation.

[Generated function entries](../sources/letsgo/tools/gen.py) call `FN_ENTER` except explicit HLE wrappers. This macro can invoke `rt_fnhook` and return early; generated direct calls/tail calls also reach it. [The hook runtime](../sources/letsgo/runtime/fnhook.c) implements title-build address translation and original-call bypass. Retain these hooks in the generated functions: moving them only into `rt_dispatch`, or using LAS Android’s profiler-disabled `FN_ENTER`, loses direct-call behavior. Keep Clang’s `musttail` contract and Let’s Go’s own headers. The sampled units compiled; whole-program call coverage remains unchecked.

## Required runtime ARM64/Android changes

There is no new generator patch justified by the current samples. Runtime changes are still required before a native Android link, as detailed in the [file-level recipe](letsgo-android-bringup.md):

- Guard the unconditional `<immintrin.h>`, AVX2 routines and x86 CPU feature builtin in [Vulkan execution](../sources/letsgo/runtime/nvn/vulkan/exec.c); ARM64 can use the existing scalar index scans.
- Exclude Android from [the glibc compatibility header path](../sources/letsgo/runtime/platform.h). Supply Android unwind/log/data/resource/process handling instead of glibc `<execinfo.h>` and desktop executable/re-exec assumptions.
- Provide the SDL Android entry and an Android guest-reservation failure path. Preserve the four module constructors and NPDM-derived title identity.
- Use dynamic Vulkan loading/VMA/ImGui integration for API30, retain required linker registration sections, and add an Android game-library name/search route while preserving ABI3 tables and code-ID checks. Keep the initial graphics port scoped to host initialization; do not replace Let’s Go’s renderer with LAS’s implementation.

These are source findings and proposed port work, not successful runtime-build claims. No changes were applied by this review.

## Original window versus shift

For generation and host compile/link bring-up, retain the original `[0x7000000000, 0x7320000000)` window. It avoids changing absolute literals, instruction patch sites, function-hook mappings and guest-pointer predicates while isolating compiler/linker failures. Nothing in an ARM64 compilation requires choosing the shifted policy first.

For eventual Android guest execution, original-window feasibility remains **unverified**. [The Linux reservation implementation](../sources/letsgo/runtime/platform/linux.c) already uses `MAP_FIXED_NOREPLACE`, checks exact placement, and releases an incorrectly placed hint. [LAS’s Android source](../sources/las-android/runtime/cpu.h) documents why its zygote-derived mappings prompted a lower window. That establishes a concrete collision risk, not proof of the current dedicated Let’s Go app’s address layout.

Retaining the original layout is permissible only after safely reserving the **entire** window in the actual dedicated app process with its actual libraries/driver setup, before guest entry. A shell process, a single-page probe or a successful native link cannot establish that. Never replace `MAP_FIXED_NOREPLACE` with destructive `MAP_FIXED` to force the initial reservation. Android cannot obtain a fresh zygote address layout through the desktop re-exec retry loop; reservation failure must stop guest startup cleanly.

If that gate fails, port a coherent shift instead: Let’s Go currently has neither `GUEST()` nor `gen.py --shift`. Preserve canonical function/patch identities while translating generated address values, loader/module bases, relocations, kernel ranges, dispatch/HLE addresses, hook lookup/build maps, raw title addresses and guest-pointer tests consistently. Replacing only `MAIN_BASE`, supplying a lower generator base, or substituting the LAS generator/header pair would silently break that contract. All four modules must be regenerated with a distinct mode/address identity.

Recommendation: finish the no-game Android runtime link with upstream ABI3 and the unshifted compile-only codegen reference. Defer full game-library compilation and the shift port until that link is reviewable; settle execution address policy with a later dedicated-app reservation gate. The owner’s manual Thor session remains protected; this recipe authorizes no device action and claims no Let’s Go Android support.
