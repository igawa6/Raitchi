# Diagnostics

The game process reads `LARECOMP_*` environment entries from `env.txt` in the
app's private data directory. The launcher rewrites its own entries (paths,
title, CPU backend, scale) before every launch and keeps other entries, except
a fixed list of research flags it removes for ordinary sessions
(`SharedFiles.withoutResearchEnvironment`).

On a **debug** APK you can edit the file with `adb shell run-as <application id>`.
Release APKs are not debuggable.

## Metadata-only traces

These flags only add bounded log output. They do not change translation,
rendering or assets, and they are excluded from the shader-cache identity, so
turning them on does not invalidate caches.

| Flag | Output |
|---|---|
| `LARECOMP_SAMPLING_TRACE=1` | First 64 unique texture/sampler bindings: shader hash, stage, descriptor and sampler state, format fallback |
| `LARECOMP_LOG_COMMAND_POOLS=1` | NVN command pool generations, adds, finalize requests and destruction |
| `LARECOMP_FONT_TRACE=1` | Shared font service requests |

## Other flags

Every other `LARECOMP_*`, `TU_*`, `MESA_*` and `VK_*` entry is part of the
shader-cache identity: changing it starts a new cache. Many are experiments for
one investigation. Search the source with
`grep -rn 'getenv("LARECOMP_' ports/shared-android/runtime` before relying on one.

## Logs

`adb logcat -s LASRecomp` (the log tag is a historical internal name). The
`[vk]` frame line reports fps, GPU time, draws and pipeline compiles. Before
sharing a log, remove device serials, account names and private paths.

## What not to share

Diagnostics print metadata. Do not post shader binaries, texture dumps, memory
dumps or screenshots that reveal game content beyond what a bug report needs.
