# Native module loading gate

`native/module_loader.h` and `.c` add actual POSIX dynamic-library loading around
the [provisional metadata validator](module-abi.md). This is shared loader
plumbing, not the shared game runtime or an integrated launcher. The existing
LAS and Eevee title libraries do not export this descriptor yet.

The caller supplies a library path and an independently prepared runtime-owned
`ModuleAbiProfile`. The loader uses `dlopen(RTLD_NOW | RTLD_LOCAL)`, resolves the
immutable data export `recomp_module_descriptor_v1`, then validates it. It
publishes the descriptor only on success. It never calls a metadata getter or a
guest function. Expected profiles must come from separately verified input/build
manifests; copying the loaded descriptor into the expectation defeats validation.

Errors distinguish invalid arguments, an in-progress open, an already loaded
title, a consumed process, dynamic-library resolution, descriptor lookup and ABI
rejection. ABI errors retain the module/function indices from the validator.
Loader diagnostics are copied into caller-owned storage.

## Process lifetime

The first actual `dlopen` attempt consumes this runtime instance's process state,
including an open or validation failure. A successful handle stays resident until
process exit; rejected handles also remain resident. There is no reset or unload
API. A second title needs a new application process, and a failed session must
also exit before retrying. Invalid null/empty arguments do not consume the state.

The process latch is inside the shared runtime library, rather than a
caller-initialized session object. A mutex protects state changes but is released
before loading, so a constructor reentering the loader receives an in-progress
error instead of deadlocking. Loading another copy/namespace of the runtime,
unloading the runtime itself, or forking after loading does not satisfy the
fresh-process policy.

## Scope of validation

This assumes a trusted, locally built library. `dlopen` can run constructors and
symbol resolvers before descriptor checks. POSIX `dlsym` can resolve a dependency's
symbol and does not establish that it is an ELF data object owned by the selected
file. Pointer extents and immutability are producer obligations. This loader does
not turn arbitrary native libraries into safe inputs.

Packaging integration still needs independently verified files/sidecars, symbol
ownership/type checks, the actual compiled CPU/header contract, generated-code
identity and runtime API requirements. The real app must route every guest-entry
path through the accepted descriptor. The present provisional metadata also does
not encode every SIMD/hook/runtime API distinction identified in the audit.

## Reproduce

```sh
python3 tools/check_module_loader.py
```

The offline utility builds one `libmodule_gate.so` containing the validator and
loader, then loads genuine game-free fixture DSOs through a dynamically linked
runner. Each scenario runs in a fresh host process. Two- and four-module profiles
use the same hashed gate artifact. Missing symbols, unresolved dependencies,
incompatible metadata and attempts to reuse the process exercise the rejection
paths. Constructor/getter/guest/destructor sentinels distinguish host library
loading from guest entry; no guest callback is needed for validation.

The utility repeats host cases with ASan/UBSan and cross-links the gate, fixtures
and runner for Android arm64/API30 with the installed NDK. Android artifacts are
not executed or installed by this utility. Source hashes, artifact hashes and
case results remain ignored under `build/module-loader/verification.json`.

These checks do not demonstrate real title loading, gameplay, Android linker
namespace compatibility or identical-runtime execution of LAS and Eevee. Those
remain acceptance gates in the [platform plan](PLAN.md).

Verified 2026-10-25: all15 scenarios pass188 checks in each optimized and
ASan/UBSan host variant, including an actual concurrent second open and a
constructor-reentrant open. The Android gate, runner and seven fixture DSOs
cross-link successfully; no Android execution was performed for this component.
