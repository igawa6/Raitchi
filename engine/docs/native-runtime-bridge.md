# Native caller to production runtime bridge

This isolated experiment connects a synthetic native ARM64 caller to the **existing installed runtime** through its exported `void rt_svc(Cpu *, uint32_t, uint64_t)` API. It invokes only SystemTick, SVC `0x1e`, whose production implementation changes `Cpu.x[0]` to the real 19.2 MHz runtime tick. The implementation and Android monotonic clock source are pinned to the supplied runtime build receipt. It is not a host-libc stand-in, original SDK wrapper or complete native game backend.

The earlier [synthetic bridge](native-arm64-proof.md), [original scalar leaf](native-las-leaf-proof.md) and [original memory/branch function](native-las-memory-proof.md) passed their separate Thor gates. This experiment joins native state to a production `Cpu *` entry; it does not retroactively combine those tests into a game boot.

## Measured Thor result

On 2026-10-25, the coordinator executed the frozen helper in a dedicated fresh app-UID Python process on the owner's AYN Thor. It returned status 0 with empty stderr, `state=passed`, `error=none` and zero failures. All **65 native-to-production-to-native transitions** passed their independent clock brackets, and the clock advanced after the bounded sleep.

The actual reported production entry was `rt_svc:SystemTick`. RX mapping verification passed; materialization failures, changes outside X0 and native-resume failures were all zero. Resumed native code consumed the production X0 result, the declared GP/vector/flag state checks passed, and host TLS/x18/floating state were preserved. No kernel or guest initializer ran. The helper identity matched the frozen artifact; complete output, process status and runtime/helper identities remain in ignored evidence through `evidence/native-runtime-bridge-current.json`.

This establishes the tested production boundary with virtual guest x18/x19, a host-backed aligned stack and the preseeded continuation described below. It does not establish arbitrary guest register/stack handling, hardware exclusive-monitor preservation, real SDK initialization, general SVC/IPC/thread behavior or full native game execution. No performance or power conclusion was measured.

## Build and coordinator execution

```sh
python3 tools/check_native_runtime_bridge.py \
  --runtime-sha256 <full-installed-runtime-sha256>
```

The host tool makes no device call or download and changes no production source, game artifact or APK. It requires the supplied hash to match the authoritative native build receipt and verifies the production CPU header, kernel API and platform clock source. `Cpu` comes directly from `native/runtime/aot_cpu.h`. Assembly offsets are generated using `offsetof` from that header and this experiment's frame definitions, then statically checked by the Android target compiler. There is no duplicated numeric CPU layout or Python recreation of it.

Both optimized and ASan/UBSan host builds pass 595 assertions against the actual branch-policy and state-marshaling functions. Host unit tests explicitly use a synthetic SVC to test changed-X0 propagation and detection of forbidden upper-vector/cache-byte writes; this is not reported as production execution. NDK r27c/API30 cross-links the device helper with only `native_runtime_bridge_probe`, version `NATIVE_RUNTIME_BRIDGE_1`, exported. Linked-instruction inspection checks that the helper never writes Android's actual x18 or TLS registers.

The ignored `build/native-runtime-bridge/` directory contains the helper DSO, `config.json`, `run_bridge.py`, source/artifact hashes and host verification logs. The coordinator can stage those three runnable inputs and invoke a dedicated fresh app-UID Python process with a 30-second outer deadline:

```sh
LD_LIBRARY_PATH=<runtime-dependency-directory> \
  python run_bridge.py config.json \
  <absolute-installed-runtime-library> <absolute-helper-library>
```

The runner hashes both libraries before loading. The helper uses `dlsym` on that runtime's actual `rt_svc`; it supplies no fake `KThread` and calls no `kernel_init`, session binding, original NSO initializer, SDK startup, main entry or GPU API. Host ELF constructors can run during `dlopen`. `LARECOMP_TICK_SCALE=1` is set and SVC trace/profiling variables are unset before loading/first use. One serial direct call primes production lazy caches; 65 subsequent calls cross the native gateway. This fresh single-process protocol does not establish concurrent production-SVC initialization safety.

## Native state contract

The sole rewrite accepts a synthetic `SVC #0x1e` word and replaces it with a direct branch to the gateway. A nearby anonymous page is acquired using bounded `MAP_FIXED_NOREPLACE` searches, never overwriting another mapping, and published RW-to-RX with cache flush and actual permission verification. Branch range/alignment and unsupported-source rejection are tested; there is no arbitrary ARM64 decoder and no original game instruction in this snippet.

Physical x19 is the bridge context and physical x18 stays Android-reserved continuously. Guest x18/x19 are distinct virtual `Cpu` slots. The test therefore **does not** claim arbitrary native guest use of those two registers. Physical x16 is seeded with the RX entry pointer and x30 with the valid native continuation; the other nonreserved GP registers receive distinct canaries. Native SP stays on a valid 16-byte-aligned host-backed test stack; this is not a switch from an arbitrary guest SP. `Cpu.pc` and the SVC diagnostic PC argument use an explicit nonzero synthetic cookie, not an original game address or an architectural-PC relocation scheme.

The synthetic branch returns through that preseeded LR; it does not implement a generic SVC-site `PC+4` continuation. Guest FPCR is zero, FPSR has the invalid-operation sticky bit seeded, and NZCV has N/C set. Other floating modes and flag combinations are not covered by this fixture.

Before C bookkeeping, the gateway captures all nonreserved live GP registers, SP/LR, NZCV, FPCR/FPSR and every full 128-bit Q register, including the upper halves of Q8–Q15. It materializes the production `Cpu` representation and checks it against the expected state. Virtual TLS/x18/x19 and the remaining opaque CPU fields retain their seeded images. After actual `rt_svc`, exactly the first eight bytes, X0, may differ: all other 66,552 bytes must match. This includes PC, virtual registers/TLS, thread pointer, padding, exclusive metadata and dispatch-cache bytes.

The returned `Cpu` state is restored into native registers, then the resumed native caller captures it again and consumes **returned X0 XOR 1**. This checks that pre-call X0 was not accidentally restored over the production result. All GP/vector/flag state represented by the declared contract is compared again. Unexpected changes outside X0 fail the proof; the saved continuation is used only to permit safe cleanup, not to turn a failure into a pass. Host caller floating state and Android TLS/x18 are checked/restored independently.

## Clock checks and limits

Each entire native-to-production-to-native transition is bracketed by independent `CLOCK_MONOTONIC` reads. Returned X0 must lie between the corresponding integer conversions `floor(ns × 192 / 10000)`. Results must be nondecreasing; an additional call after a bounded two-millisecond sleep must advance. The hardware counter frequency is not assumed to be 19.2 MHz, and the time brackets are correctness checks, not speed/power measurements.

A passing device report must show `state=passed`, zero failures, 65 native transitions and 65 valid clock brackets, progress after sleep, verified RX mapping, zero materialization/other-byte-mutation/resume failures and preserved host TLS/x18/FP. Save stdout, stderr, status and exact artifact identities. A signal/fault or inability to map near the gateway is failed/blocked evidence, not permission to skip the gateway or replace the production API.

No nested production callback is exercised by SystemTick. Hardware exclusive-monitor preservation is explicitly unsupported: preserving `Cpu` exclusive metadata does not keep a native load/store-exclusive window alive across a host call. The proof does not cover arbitrary register rewriting, real guest TLS layouts or stack switching, SDK/IPC/thread initialization, general SVC argument/output rules, exceptions/unwinding, full native game execution, GPU correctness or any performance/power benefit. Passing it would establish this one production boundary under this explicit synthetic caller contract.
