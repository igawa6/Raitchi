# Asset-free Android native ARM64 bridge proof

This isolated experiment tests an Android native-execution boundary before any game or nxport integration. It contains original synthetic instructions and assembly only. It does not read game assets, import nxport code, change the shared runtime or build an APK. The host tool never operates Thor; device execution belongs to the coordinator.

## Measured Thor results

On 2026-10-25, the coordinator ran the frozen DSO twice under the app UID in separate fresh Python helper processes on the owner's AYN Thor. Both returned status 0 with `state=passed`, `error=none` and zero failures. Both measured 4096-byte host pages, verified RW-to-RX publication, two threads, 256 nested callbacks, two signals with two valid interrupted contexts, and preserved host TLS/x18.

| Run | Scalar increments | Pair low / high | Scalar exclusive retries | Pair exclusive retries |
| --- | ---: | ---: | ---: | ---: |
| 1 | 40,000 | 40,000 / 120,000 | 13,847 | 5,357 |
| 2 | 40,000 | 40,000 / 120,000 | 41,426 | 23,529 |

The nonzero retry counts show that the exclusive retry paths executed during the concurrent workload. They are not timing measurements or proof of arbitrary exclusive-sequence rewriting. Complete reports and the exact artifact identity are retained through ignored `evidence/native-arm64-device-current.json`. The separately reviewed [original SDK leaf comparison](native-las-leaf-proof.md) subsequently passed all twelve cases.

Build and host validation:

```sh
python3 tools/check_native_arm64_proof.py
```

The tool uses installed Clang and cached Android NDK r27c `27.2.12479018`, targeting API 30. It downloads nothing. It runs the actual rewrite implementation under optimized host compilation and ASan/UBSan, then cross-links an Android ARM64 DSO and PIE executable. Both host variants passed 190 assertions. `build/native-arm64-proof/verification.json` records source and artifact SHA-256 values; its build-only execution field remains pending until paired with the separate device evidence for that exact artifact.

## Coordinator execution

After staging only the three generated proof artifacts into a coordinator-owned private directory, invoke an already installed Android Python in a **fresh process** under the app UID:

```sh
python /absolute/private/path/run_probe.py /absolute/private/path/libnative_arm64_proof.so
```

The equivalent standalone artifact is `build/native-arm64-proof/native_arm64_proof`; use it only where the app UID can execute it. The host tool performs no staging or execution. The coordinator should enforce a 30-second outer process deadline, record stdout and process status, and leave the game app untouched. The proof temporarily handles SIGUSR2 and restores its prior disposition; use a dedicated proof process, not the game process.

The DSO exposes exactly one API, versioned `NAP_PROOF_1`:

```c
int native_arm64_proof_run(char *json_output, size_t capacity);
```

Provide at least 1024 bytes; the supplied launcher provides 2048. Return `0` means all execution checks passed; `1` means failure; `-1` means invalid buffer or concurrent invocation. The API is serialized and does not abort the process on an ordinary assertion failure. A hardware fault still terminates the dedicated process and must be recorded as failure. No persistent device state is changed by the proof itself.

## What is exercised

The actual rewrite function accepts only these operations: `MRS X0..X17,TPIDR_EL0`, register MOVs from/to virtual x18 using X0..X17, one exact `ADD X0,X9,X10`, and final `RET`. X19 points to the explicit synthetic context. TLS/x18 reads become context loads; x18 writes become context stores. Unrecognized instructions, writes to the dedicated context register, physical x18 targets and unsupported TLS registers are rejected transactionally. Raw hazardous input words are never executed. This is a whitelist for two tiny templates, not a general ARM64 decoder.

The executable snippets occupy anonymous RW memory during rewriting, receive an instruction-cache flush, then become RX. Actual `/proc/self/maps` permissions must confirm read/execute without write. There is no RWX mapping. Host page size is queried at runtime; the DSO and PIE use 16 KiB ELF segment alignment.

Two pthreads use distinct guest TLS/x18 values with the same RX snippets. Each performs 128 native-to-host callbacks, and each callback reenters both rewritten snippets. A native gate saves caller registers, all vector registers and NZCV/FPCR/FPSR around the C callback. Live x9, all 128 bits of v0/v31, NZCV and FPCR/FPSR are checked after deliberate host clobbering. Host Bionic `TPIDR_EL0` and actual x18 are read for comparison and never written by the proof's assembly or generated code; the build tool checks the linked instruction stream for such writes.

After synchronization, both workers increment a shared scalar through `LDAXR/STLXR` and a 16-byte-aligned pair through `LDAXP/STLXP`, 20,000 times each. Pair updates add one to the first word and three to the second. Retry counts are reported and each increment has a retry bound. These kernels are original assembly, **not rewritten exclusive sequences**; the result does not validate arbitrary x18 rewriting inside exclusive windows. A zero retry count does not demonstrate actual contention despite the concurrent workload.

Each worker then waits in a marked native loop. A targeted SIGUSR2 handler verifies that the interrupted PC lies in that loop, the kernel-saved x18 equals the thread's original value, and the handler sees the same host x18/TLS. The handler releases the loop without calling libc, allocating, locking or doing TLS lookup. The caller's original handler is restored after joins. Ready/signal waits have five-second bounds; the coordinator's outer deadline also covers unexpected kernel/library faults or stuck threads.

## Acceptance and limits

A passing device result must report `state=passed`, `failures=0`, two threads, 256 callbacks, two signals with two valid contexts, verified RX mapping, scalar/pair counts `40000/40000/120000`, and preserved host TLS/x18. Preserve the complete JSON and the exact library hash beside the host verification record. Repeat once in another fresh process to check process-independent setup; that is a coordinator action, not performed by the host tool.

Host tests validate encoding, register restrictions, invalid templates and unchanged output after rejection. Cross-linking and inspection establish that the artifact is buildable with the intended API/export policy. The two hardware runs additionally verified the specific synthetic state-preservation, context-isolation, signal and atomic checks described above on this Thor. They do not cover arbitrary ARM64 rewriting, real SDK ABI marshaling, guest stack switching, system-call interception, real guest TLS layouts, faults during exclusive sequences, cancellation/unwinding, GPU correctness, shader compilation or a performance/power advantage. Neither these runs nor the subsequent single-leaf comparison establish a full native game backend or justify replacing the working C-AOT backend.
