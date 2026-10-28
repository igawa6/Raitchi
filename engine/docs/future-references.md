# Future engineering references

## Echoes of Wisdom

Source-only research was authorized alongside shared-runtime implementation.
[Nycolazs/EoWRecomp](https://github.com/Nycolazs/EoWRecomp) is pinned separately at
`dd9c79d6c01d9eaddafc1fc5a07b6658bfc56505` in
[echoes-source.lock.json](../echoes-source.lock.json). Verify its local checkout
with `python3 tools/verify_sources.py --lock echoes-source.lock.json`.

The [source audit](echoes-source-audit.md) records different module order/bases,
main larger than64MiB, SDK startup/init policy, 128-bit atomic helpers and GPU
writes absent from our Android read-only fallback. Android gameplay is unverified.
The owner’s original executable modules have since been imported and audited on
the host; no generation, Android build or Thor game test has been performed. Use these constraints when
evolving the common contract; preserve LAS/Eevee as the current acceptance pair.

## BOTWRecompAndroid

Owner-supplied reference, recorded 2026-10-24:
[mamcjr/BOTWRecompAndroid](https://github.com/mamcjr/BOTWRecompAndroid).
Observed `main` revision: `b43e86a3047654d02b62145bc7857482d0f8e3b8`
(resolved with `git ls-remote`). This is a research reference, not a new
baseline dependency or a change to the LAS / Let's Go milestone.

The repository's [README at that revision](https://github.com/mamcjr/BOTWRecompAndroid/blob/b43e86a3047654d02b62145bc7857482d0f8e3b8/README.md)
describes an Android BOTW recomp derived from LAS Recomp and its Android port,
with references to SavageWindRecomp. Useful follow-up areas:

- Multiple executable versions: version detection and separate game-file folders.
- Android setup and local compilation: progress, recovery and artifact identity.
- Vulkan driver selection through adrenotools and compatibility paths for different GPUs.
- SDK 20 kernel fixes, setjmp handling and frame pacing credited to SavageWindRecomp.
- Save handling, lifecycle behavior, controllers and touch input.
- Which runtime changes are reusable services and which belong in a title adapter.

Treat its performance and playability statements as upstream reports. The README
reports testing on a POCO F7 Ultra / Adreno 830 with Turnip; this does not establish
AYN Thor / Adreno 740 compatibility. It also lists untested BOTW phone-side setup,
a simplified audio mixer and black save thumbnails as gaps.

Before reusing code, pin and inspect the actual implementation, dependencies and
licenses; compare against our audited LAS source and explicit module contract.
No code audit, build, install or device test of this repository has been performed
here. Consult it when addressing the shared runtime and setup gates; keep the
current LAS and Let's Go reference work focused.

## nxport

Owner-supplied comparison, inspected 2026-10-25 at
`3a5d23c67f23346a9bca0a03d4e03b01375090c4`:
[nxporting/nxport](https://github.com/nxporting/nxport/tree/3a5d23c67f23346a9bca0a03d4e03b01375090c4).
Its goal overlaps the shared compatibility layer and game-profile design here.
The README currently targets Apple Silicon and a builder producing macOS/iOS
apps; reported game compatibility is upstream evidence, not a local test.

The actual [CPU module](https://github.com/nxporting/nxport/blob/3a5d23c67f23346a9bca0a03d4e03b01375090c4/crates/engine/src/cpu/mod.rs)
runs original ARM64 code with TLS/platform-register rewrites and HLE trampolines.
The [AOT implementation](https://github.com/nxporting/nxport/blob/3a5d23c67f23346a9bca0a03d4e03b01375090c4/crates/engine/src/cpu/aot.rs)
packages patched machine-code images for iOS code-signing constraints; it does
not use our ARM64-to-C generator route. Engine startup currently installs its
Metal backend. Profiles identify games and versions and supply optional patches.

Audit its CPU rewriting, SDK interposition, profile defaults and shader
preparation before expanding our compiler architecture. An Android/Thor port
still needs Android lifecycle/input/audio and Vulkan integration; no local build,
Android support or automatic cross-project binary compatibility is established.
No nxport source was incorporated into this workspace.

Completed bounded source audits:

- [Native CPU execution and profiles](nxport-cpu-audit.md).
- [Graphics translation and Android feasibility](nxport-graphics-audit.md).

The recommended next proof is asset-free Android native execution with strict
TLS/x18 and callback preservation, while keeping the existing NVN/Vulkan core.
A separate translator experiment must replace hardcoded Apple capabilities
before any Thor pipeline execution.
