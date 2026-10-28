# Native entry on production guest threads

This opt-in diagnostic connects native AArch64 instructions to the production shared runtime's actual KThread lifecycle. It stays outside the launcher, APK and normal game startup. Its default fixture contains only reviewed synthetic instructions. An optional [original SDK wrapper mode](native-sdk-thunk.md) now passes a separate Thor gate; neither mode runs SDK startup or a game entry.

`tools/check_native_thread_bridge.py --runtime-sha256 <full SHA256> --game-sha256 <full SHA256>` verifies the runtime build receipt and relevant frozen source hashes, runs optimized and ASan/UBSan host tests, emits assembly offsets from the actual `Cpu`/context declarations, confirms the offsets with the Android compiler, and links a helper exposing only `native_thread_bridge_probe`. `--runtime-receipt` selects an archived receipt if needed. Build artifacts and the hash-pinned runner go into ignored `build/native-thread-bridge`.

Run `run_bridge.py config.json runtime.so helper.so game_root game.so game.so.saotm` in a fresh Android process under an **outer 30 second process timeout**. The coordinator must provide the diagnostic core as `libmain.so` through the process library search path so the verified title library's dependency resolves to that same core. The runner checks full runtime/helper/title-library SHA256 values. The production session also verifies its sidecar and original NSOs. This tool does not use ADB, install an APK, or interrupt a play session.

The helper calls real `shared_session_prepare` and `shared_guest_prepare`, registering one diagnostic HLE entry after the normal loader/kernel bootstrap. It creates two actual production KThreads with SVC 08, assigns different virtual processor numbers, and starts them with SVC 09. Each callback verifies its genuine current KThread/Cpu identity through the production API and writes a distinct sentinel into its allocated 0x200-byte guest TLS slot. An atomic barrier establishes that both slots are live simultaneously.

Each native fixture performs eight unrolled rounds of guest TPIDRRO reads, TLS sentinel loads, GetCurrentProcessorNumber (10), GetThreadId (25, current-thread pseudo handle), and SystemTick (1e). Each source trap branches to its own copied island, which captures all GPRs, all Q registers, flags and FP state before calling C. The island restores state and branches directly to that source instruction's PC+4; SVC continuation does not depend on guest LR. Each call verifies current thread/core/ID/TLS, architectural canaries and every non-output byte of the real `Cpu`, including opaque thread, exclusive-state and dispatch-cache fields.

The first thread returns normally through the native adapter and the existing production thread entry. The second calls production ExitThread (0a), which longjmps to the existing `thread_main` setjmp. All contexts, code mappings and snapshots are owned by the parent coordinator; no callback holds a host lock or owns a resource requiring stack cleanup across this exit. The coordinator polls the production snapshot API with a ten second internal deadline, joins only after FINISHED, requires SIGNALED and JOINED, and shuts down the prepared guest. A failed attempt with live threads intentionally retains all callback allocations until the outer helper process exits. Reports read callback counters only after both actual joins; earlier failures report counters_known=false and zero placeholders without accessing callback-owned mutable state.

A successful device report requires two joined threads, two RX mappings, 48 returning SVC calls, 16 guest TLS reads, one nonreturning ExitThread, distinct IDs/TLS slots and zero identity, state or opaque-state failures. Host tests exercise the actual marshal/restore, branch policy, range rejection and mutation detection with explicitly synthetic callbacks; they do not prove Android thread execution.

## Measured Thor result

The AYN Thor run at `2026-10-25T08:02:55Z` passed with exit status 0. Its local evidence is `evidence/native-thread-device-20261025T080255Z/result.json`. Both real threads joined and signaled, both fixture mappings were RX, 48 returning SVC transitions and 16 guest TLS reads completed, and ExitThread was entered once without returning. State, current-thread identity, identity-reason and opaque-state failure counts were all zero. The helper process took 4.767 seconds including hashing and session bootstrap; this is diagnostic wall time, not a game-performance measurement.

The measured helper SHA256 was `ceb96f11ff407c29a27b6350ef099f6430ff4dfceaf9bad108d045278b7f5df8`, paired with diagnostic core `d9f0bfdcd2dfcbeff15878c22dce299b32a1522d57b37bf5287ff5727ee75a9e`. The coordinator loaded these separate DSOs from an isolated probe directory. The installed launcher runtime remained byte-identical to baseline `0e63f0d7bc800c31ff45f166f787060630188cc9bf536a7b6d1ced24acc170e8`; no APK update was needed.

The first attempt failed all 65 identity checks because the helper incorrectly compared the snapshot's writable `TPIDR_EL0` value with the allocation assigned to read-only `TPIDRRO_EL0`. The production kernel initializes those registers independently. The correction compares writable TLS against the actual thread's frozen `Cpu.tpidr`, and read-only TLS against its real allocated TLS base. Host regressions deliberately give the registers unequal values and reject each independently corrupted identity field. Optimized and ASan/UBSan runs each passed 1,739 assertions; target offset checks, cross-linking, the sole-export check and the physical X18/TLS-write scan also passed.

The reusable production lifecycle and identity contract is [guest_session.h](../ports/shared-android/runtime/guest_session.h). Its separate host API checks are [check_shared_guest_session.py](../tools/check_shared_guest_session.py), and actual production thread-start failure/cleanup checks are [check_kernel_thread_start.py](../tools/check_kernel_thread_start.py). The native bridge's own checks are [check_native_thread_bridge.py](../tools/check_native_thread_bridge.py). These tests support the measured result without substituting synthetic host callbacks for the successful real-thread device run.

## Limits and next gate

An optional trailing `--guest-stack` argument now selects a guarded stack in
the fresh session's reserved stack region. On Thor, the SDK variant passed with
four verified guard pages, 68 observations proving C/HLE ran on the actual host
pthread stack, and 67 guest stack-canary observations. Normal return restored the
host ABI; actual ExitThread unwound from the host stack and both KThreads joined.
The helper spills/reloads native registers on guest SP and rejects nested reuse
of the same bridge context. Its host checks pass 1,791 assertions per optimized
and sanitized variant, including isolated guard-fault and negative stack tests.
This optional mode is distinct from the historical host-backed result below.

Native SP remains backed by the real host pthread stack. The guest stack allocated to KThread is recorded and restored after normal return, but no guest stack switch is attempted. Physical X18 remains Android's; guest X18 and X19 stay virtual, with physical X19 reserved for this bridge context. The helper checks host TLS/X18 on every transition and checks host FP restoration after normal return. ExitThread abandons the native callback, so there is no claim of callback FP restoration after that nonlocal exit. Hardware exclusive-monitor continuity, arbitrary native instructions, SDK initialization, shaders, game boot, performance and power are untested.

The subsequent [original SDK SVC wrapper gate](native-sdk-thunk.md) passed on these genuine threads, preserving its nested-call LR and checking the exact native return marker. Its gateway targets copied SVC PC+4, retaining the original RET. Guest-stack switching and a complete native entry ABI remain separate work. The existing C-AOT launcher stays the working baseline.
