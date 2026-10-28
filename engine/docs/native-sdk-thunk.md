# Original SDK wrapper on real guest threads

This extends the [native thread bridge](native-thread-bridge.md) with an optional,
privately verified original SDK wrapper. It tests a native nested call through
the shared runtime's actual SVC implementation and compares its result with the
verified title library's C-AOT entry on the same real guest thread.

## Measured Thor result

The fresh app-UID helper run on 2026-10-25 at 08:23:58 UTC passed with exit
status 0. It joined two genuine production KThreads, verified two RX mappings,
completed 50 returning native SVC transitions and 16 guest TLS reads, and
completed two original wrapper calls and returns. Both nested LR checks and both
actual descriptor comparisons passed. State, identity, opaque-state, nested-LR,
resume and AOT comparison failure counts were all zero. One thread returned
normally; the other entered actual ExitThread once without returning from it.

The helper SHA256 was
`fa874f5a47539eb33699f2b8acbd3a307a93b1e25637f78f595c07499e29428b`, using
diagnostic core
`d9f0bfdcd2dfcbeff15878c22dce299b32a1522d57b37bf5287ff5727ee75a9e`.
These DSOs ran separately from the installed launcher. The installed runtime
still hashed to baseline
`0e63f0d7bc800c31ff45f166f787060630188cc9bf536a7b6d1ced24acc170e8`.
Private output, status and exact artifact identities are recorded through
`evidence/native-sdk-sdk-device-current.json`.

A second fresh-process run with the same helper and SDK input omitted also
passed: 48 returning SVC transitions, 16 TLS reads, two joined threads, and zero
SDK/AOT calls or failures. Its evidence is recorded through
`evidence/native-sdk-synthetic-device-current.json`. This checks the helper's
default mode after extending its input ABI.

Optimized and ASan/UBSan host variants each passed 1,769 checks. Six targeted
receipt mutations were rejected. Independent review resolved a missing
game-library-to-producer link before device execution; the coordinator also
verified the actual published device receipt and the two relevant cached object
hashes. Native correctness checks are distinct from those static provenance
checks. The process wall time included hashing/bootstrap and is not a game
performance measurement.

## Candidate and execution boundary

`python3 tools/audit_native_sdk_thunk.py` resolves the exported internal
`ThreadManagerImplByHorizon::GetCurrentCoreNumber` symbol from the owned SDK.
The selected wrapper contains one unconditional branch to a local two-instruction
SVC/return thunk. The audit checks complete original-module SHA256 and build ID,
decompressed text identity, bounds, alignment, relocation exclusion, branch
target agreement between independent decoding and Capstone, and both generated
CPU3 function-table entries and definitions against their producer records.
SDK mode also requires the published library's build receipt. Its actual library
hash, complete semantic fingerprint, original SDK identity, generated table and
definition-unit hashes, and unique compiled-object records must agree with the
audited inputs. Overall host and on-device producer fingerprints may differ
because their compilers and paths differ; the relevant generated sources must
be byte-identical.

Original bytes, offsets, addresses and generated C excerpts stay in ignored
`evidence/native-sdk-thunk-candidate.json` and `build/native-sdk-thunk-candidate`.
REA rejected the original NSO as an unsupported format; the static evidence uses
the pinned NSO parser and Capstone. No REA instruction-analysis result or runtime
behavior is inferred from that failed open.

The existing bridge helper accepts an optional versioned SDK input. The generated
runner verifies the runtime, helper, game library, original SDK, and extracted
wrapper/thunk bundle hashes. SDK mode requires the explicit bundle argument;
without it, the helper runs the synthetic thread proof.

Each real production KThread first calls the exact SDK entry from the validated
game descriptor using its actual attached `Cpu`. The helper checks the returned
virtual core and opaque CPU state, then restores the original CPU image before
native canary seeding. It does not fabricate a thread or replace the production
SVC implementation.

Native code saves its outer LR, uses BL to call the copied original wrapper,
and checks the nested LR at the syscall boundary. Only the wrapper's relative
branch and the SVC interception are relocated. The original RET is retained.
The existing copied trap island branches to that SVC's exact copied PC+4; it
preserves the nested LR for the original return. Native caller code records
its resumed address and returned core, counts the completed return, and restores
the outer LR. Ordinary TLS/SVC canaries and the normal-return/ExitThread paths
remain part of the same run.

## Reproduction

```sh
python3 tools/audit_native_sdk_thunk.py
python3 tools/check_native_thread_bridge.py \
  --runtime-sha256 <diagnostic-runtime-sha256> \
  --game-sha256 <verified-game-library-sha256> \
  --sdk-candidate evidence/native-sdk-thunk-candidate.json \
  --game-receipt <published-game-build-receipt.json>
```

The build emits its paired helper, runner, config and optimized/sanitized host
results under ignored `build/native-thread-bridge`. In a fresh Android helper
process, use the library search path for that diagnostic runtime and its installed
dependencies, with an outer 30-second deadline:

```sh
python run_bridge.py config.json runtime.so helper.so \
  game_root game.so game.so.saotm bundle.bin
```

Omit `bundle.bin` to check default synthetic-mode compatibility. An older frozen
helper must retain its own paired runner/config because the experimental helper
call ABI now includes the optional input argument.

## Limits and next gate

This is one relocated SDK wrapper with one syscall and an unchanged return,
using the host-backed native stack and virtual guest X18/X19 contract. Its SVC
diagnostic PC is the copied instruction address; original architectural-PC
mapping is not generalized. The result establishes no SDK initialization,
blocking IPC, arbitrary native instructions, exclusive-monitor continuity,
native game boot, graphics, speed or power advantage.

The subsequent guarded-stack variant passed on Thor: native nested calls used
guest SP, every C/HLE transition used the host stack, normal return restored the
host ABI, and real ExitThread unwound correctly. See the optional stack mode in
[the thread bridge](native-thread-bridge.md). The later
[original game execution backend](native-game-backend.md) is a separate ongoing
integration effort with its own native, software and interpreter boundaries.
