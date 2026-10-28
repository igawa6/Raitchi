# Raitchi architecture

Raitchi runs original Nintendo Switch game code on Android ARM64 devices. It is
experimental. This page maps the source tree so that contributors can find the
shared code a game needs instead of copying the runtime for a new title.

```
 Android app (Eden-derived shell, Kotlin)         separate repository, GPL-3.0-or-later
   setup / game folders / library / settings / drivers
        |  RaitchiShellBridge (Java)  - shell process never loads libmain
        v
 :game process  LasActivity + SDL3 (Java)          ports/shared-android/apk
        |  JNI (com.lasrecomp.android names are a stable ABI)
        v
 libmain.so  shared runtime                        ports/shared-android/runtime
   CPU        continuous NCE (original ARM64 code)   continuous_*.c/.S, native_*.c
   kernel     threads, sync, memory, SVCs            kernel.c, physical_memory.c, ipc.c
   services   fs, hid, audio, am, vi, ...            services/, hle/
   graphics   NVN -> Maxwell -> Vulkan               nvn/, nvn/maxwell/, nvn/vulkan/
   packages   NSP/XCI reader, RomFS provider         nxpkg.c, native_package_*.c
        ^
        |  title adapters (small C tables)          native/adapters/<title>/profile.c
```

## CPU: continuous native execution (NCE)

Game modules (NSO) execute natively at their guest addresses. Before guest
threads start, a planner patches selected sites (SVCs, TLS reads, counters and
title compatibility sites) so that they return into host C code. Ordinary
branches, calls and loops never leave native execution. The assembly context
switch saves the full guest register state and restores Android TLS, X18 and
the stack before running host code.

- Planner and image publication: `native/continuous-nce/`, `ports/shared-android/runtime/continuous_image.c`
- Context and signals: `continuous_context.{c,S}`, `continuous_signals.c`
- Design notes: [continuous NCE](raitchi-continuous-nce.md), [NCE integration plan](native-nce-next-step.md)

Native execution does not remove GPU work: shaders still have to be translated
and Vulkan pipelines still have to be compiled (see below).

A historical C ahead-of-time path (`aot`, generated C per game) remains for
two early titles. It is optional and is not the default Android workflow.
New games do not need generated C or a new recompiler.

## Kernel and services

`kernel.c` implements Horizon threads, synchronization, address space and
SVCs. Services (`services/`, `hle/`) implement the IPC interfaces games call:
filesystem, HID input, audio renderer, applet manager, display. All titles use
the same implementation. When a game reaches an unimplemented service command
the runtime fails with a diagnostic instead of returning a fake success.

## Graphics: NVN to Vulkan

Switch games draw through NVN. Raitchi implements the NVN API surface the
games call (`nvn/`), decodes their command and resource state, translates
Maxwell shader binaries to SPIR-V (`nvn/maxwell/translate.c`) and records
Vulkan work (`nvn/vulkan/`). Missing shader instructions, texture formats and
API entry points are the most common reasons a new game fails to render.

### Shader translation and caches

1. First use of a shader: Maxwell bytecode is translated to SPIR-V, then the
   driver compiles a Vulkan pipeline. This is the remaining first-use stall.
2. Headstart replay (`LHS6`): pipeline states seen before are recorded and
   replayed during loading on the next launch.
3. SPIR-V disk cache and the driver's opaque `VkPipelineCache` (`GPC2` sidecar).

Each cache is keyed by a renderer fingerprint built from the actual compiled
renderer sources, toolchain, original game module hashes, the Vulkan device and
driver, and relevant environment variables. Any mismatch fails closed and
starts an empty cache. Pipeline states never seen before still compile on
first use. Details: [shader cache identity](shader-cache-identity.md).

## Packages

`nxpkg.c` reads NSP/XCI on demand (AES-XTS/CTR, BucketTree, LZ4). Preparation
copies only ExeFS and writes a `native.manifest`; RomFS is streamed from the
user's own package. Keys come from the user's own `prod.keys` and are never
written to disk in decrypted form. See [direct package loading](direct-nsp-native-loading.md).

## Title adapters

An adapter (`native/adapters/<title>/profile.c`) declares what is specific to
one game build: program ID, version, module build-ID prefixes, module layout,
startup order, NVN interface version, save namespace and renderer policy flags.
Adapters contain no game code and no copyrighted data. Shared behavior belongs
in the runtime, not in an adapter; a policy flag is acceptable only when a
title-specific difference is proven and documented.

## Android

- Shell app: Eden's Android app module, rebranded, with emulation features that
  Raitchi does not implement removed. It handles setup, keys, game folders,
  the library, per-game settings and GPU driver management. It talks to the
  engine only through `RaitchiShellBridge` and `libraitchiprepare.so`
  (metadata and preparation, no guest code).
- Game process: `:game` runs `LasActivity`/SDL3 and loads `libmain.so`. Every
  supported title loads the same `libmain.so` bytes.

## Canonical, pinned and generated sources

| Path | Status |
|---|---|
| `ports/shared-android/runtime/`, `native/`, `tools/`, `tests/` | canonical, edit here |
| `ports/shared-android/apk/` | canonical Android game-process module |
| shell repository at the commit in `shell.lock.json` | canonical shell app |
| `sources/` (fetched by `tools/verify_sources.py`) | pinned upstream references, read-only |
| `work/` | generated build overlays; never edit, regenerated by the build |
| `build/` | build outputs and receipts |
| `evidence/`, `private/` | local only, never committed |
