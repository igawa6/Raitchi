# Actual generated-code shared CPU probes

Historical bounded probe before [full Android integration](shared-android-integration.md).
Both production title libraries have since rebuilt all units against the common
contract, including fresh LAS generation. The header hash and provenance limits
below describe this earlier12-object checkpoint.

Verified 2026-10-25: **12/12 selected objects compile without diagnostics** with NDK r27c, ARM64/API30, against `native/runtime/aot_cpu.h`. This exercises cached real generated code, rather than synthetic functions alone. It does not generate new code, build a game library, link a runtime, load a module or run on Android.

```sh
python3 tools/probe_shared_aot_codegen.py --compile --jobs 2
```

Omit `--compile` to validate the selected caches/provenance only. Jobs are restricted to one or two; default is one. `--ndk` can select the locally installed pinned NDK27.2.12479018. There are no downloads or device operations. Original checkouts, generated sources, reference objects and libraries are unchanged.

| Profile | Modules sampled | Objects per module | Shift | Capabilities |
| --- | --- | --- | --- | --- |
| LAS Android | main, SDK | First numbered function unit + address/function table | `0x6000000000` |1: instruction hooks |
| Let's Go | main, SDK, subsdk0, subsdk1 | First numbered function unit + address/function table |0 |2: function-entry hooks |

Private per-profile `cpu.h` wrappers include an immutable copy of the common header, selecting only the original shift and hook capabilities. They add no missing helper implementations or compatibility shims. Shared CPU identity3 means66,560 bytes, alignment16, `excl_hi` at336, vectors at512 and the LAS4,096-entry cache at1,024; the actual common header asserts the complete layout. Both profiles retain their original ARM64 assembly SIMD generation. Compilation uses LAS O2 / Let's Go O1, PIC, hidden visibility, signed char, no strict aliasing and reserved x18. LAS's recorded scheduling/split/cold-unit policy is applied where relevant.

The tested common header SHA-256 is `cef797c6144c8b29c266321f7192a09e5ac9164ed41411b77b389a4014e8d821`. A run copies/hashes the header and refuses completion if the live header changes during compilation. A different header must be probed again.

## Provenance distinction

Let's Go has an original completed generation manifest. The utility verifies its internal fingerprint, pinned source commit, recorded generator/header source hashes, ARM64 assembly/unshifted ABI3 options and module-base declarations. Each of the eight selected source hashes must match that manifest. It does not rescan all remaining generated units or rehash original game input files; their recorded declarations are preserved as provenance, not represented as freshly verified assets.

The older LAS cache has **no historical generation manifest with per-unit hashes**. The utility reproduces its legacy generator/header/options identity in Python, compares the existing library identity, cached main build-ID record and successful generation log, and hashes the four selected source files now. Its result is explicitly an **observed legacy cache snapshot**, not historical certification that those exact bytes were produced in the original build. This limitation was accepted for the bounded compile probe. It must not become the trust model for a production shared-title library: the new producer needs a complete fresh manifest and original-module verification.

All selected source hashes, legacy/input identities, source paths, diagnostics, objects and full undefined-symbol inventories remain in ignored local build output. No game IDs, input paths or generated code are reproduced in this document. An atomically updated private `probes.json` records compiler binary/version/NDK properties, exact per-object command/flags, common-header and wrapper hashes, CPU identity, source/object hashes and inventory hashes/counts. `complete` becomes true only after all twelve objects succeed. Individual compiler invocations time out after180 seconds; errors remain in private logs instead of printing generated-source excerpts.

## What the object imports establish

The selected LAS main object imports `rt_hook` and the additional title helpers `rt_aspect_site`, `rt_fovy`, `rt_frame_steps`, `rt_game_dynres`, `rt_interval_choice`. These need implementations and an intentional export path through the LAS adapter/common runtime. A runtime exporting only generic dispatch plus `rt_hook` would not satisfy even this sampled producer. LAS selected objects do not import the function-entry hook globals/functions.

The selected Let's Go function objects import `g_fnhook` and `rt_fnhook`, confirming that capability2 preserved the generated function-entry hook boundary. Both profiles also require generic dispatch/trap helpers. NDK emits outlined atomic references such as `__aarch64_cas4_acq_rel` and `__aarch64_cas8_acq_rel`; the final link must resolve them through the toolchain builtins/link policy. They are host compiler dependencies, not new guest services.

Table objects contain references to their modules' generated functions, as expected. Successful table compilation checks the shared `GuestFn(Cpu *)` declarations but resolves none of those references. Full inventories are recorded privately per object; this sample is not a complete runtime export allowlist.

Remaining gates: complete producer manifests, all-unit compilation, a common library entry/metadata contract, final undefined-symbol resolution and ELF checks, actual four-module loading/init, fresh-process Android execution and both gameplay/save routes. A successful sample cannot establish game compatibility, helper semantics, performance or an identical-runtime milestone.
