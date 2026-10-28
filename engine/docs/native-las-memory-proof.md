# Original SDK memory and branch comparison

This bounded follow-up compares one reviewed original LAS SDK `strlen` against its actual compiled CPU3 descriptor function. The [synthetic bridge](native-arm64-proof.md) passed twice on Thor and the [original `fabsf` comparison](native-las-leaf-proof.md) passed all twelve cases before this experiment. It is not a native game backend, SDK-startup test, game-boot test or performance comparison.

## Measured Thor result

On 2026-10-25, the coordinator ran the frozen helper under the app UID in a dedicated fresh Python process on the owner's AYN Thor. It returned status 0 with empty stderr, `state=passed`, `error=none`, **180 cases and zero failures**. All eight byte alignments were exercised (`alignment_mask=255`), including 52 cases near the trailing guard.

The report verified RX code mapping, inaccessible guards, read-only input pages, unchanged complete input buffers, actual low-guest buffer placement, restored caller FPCR/FPSR and preserved host TLS/x18. Native and actual CPU3 results both matched the known string length for every case. No original guest initializer ran and no full guest window was reserved. The helper identity matched the frozen build; complete results, process status and library/helper identities remain in ignored evidence through `evidence/native-las-memory-current.json`.

This hardware pass establishes only the reviewed memory/branch function and tested buffer arrangements. It does not establish arbitrary instruction rewriting, byte-granular absence of reads beyond a string's zero byte, full SDK/thread operation, game boot, GPU correctness or a speed/power advantage.

The candidate is 116 bytes containing 29 scalar instructions. It reads bytes until aligned, then reads aligned 64-bit words to locate a zero byte. Every conditional branch stays inside the copied function; there is no relocation target, external call, TLS/x18 access, system call, privileged instruction, literal/ADRP address, stack frame, input store or unwind dependency. Original bytes, addresses, NSO/text identities, disassembly and the verified AOT source excerpt remain in ignored `evidence/native-memory-candidate.json`.

Prepare with coordinator-supplied full hashes of the completed on-device game library and the exact installed runtime to be used for this run:

```sh
python3 tools/check_native_las_memory.py \
  --game-sha256 <completed-device-library-sha256> \
  --runtime-sha256 <matching-installed-runtime-sha256>
```

The host tool performs no ADB action or download. It requires the previous device gates and their exact helper identities, independently verifies original NSOs/text and candidate instructions/branches, and checks the generated CPU3 table/function-unit hashes. The C helper includes production `Cpu`, descriptor, sidecar and validator definitions; it does not recreate those layouts in Python. NDK r27c/API30 cross-linking and the sole-export check are recorded in ignored `build/native-las-memory-proof/verification.json`. Actual candidate acceptance and 352 unsafe-instruction rejection assertions pass in both optimized and ASan/UBSan host builds; these checks do not execute ARM64 instructions on the host.

The coordinator may stage `libnative_las_memory_probe.so`, `private-config.json` and `run_memory.py` from that ignored build directory, then run them under the app UID in a dedicated fresh Python helper process with a 30-second outer deadline:

```sh
LD_LIBRARY_PATH=<runtime-dependency-directory> \
  python run_memory.py private-config.json \
  <absolute-runtime-library> <absolute-completed-game-library> \
  <absolute-helper-library>
```

The runner hashes all three libraries before loading. The helper validates the complete actual descriptor against independent original-module/producer expectations, then selects only the reviewed SDK entry. Its only exported API is `native_las_memory_probe`, version `MEMORY_PROBE_1`. Normal host ELF constructors may run during library loading, but no original NSO initializer, runtime main, guest dispatcher, SDK startup or GPU setup is called.

Input storage occupies two accessible pages between two inaccessible guard pages, inside the descriptor's actual low guest window. Placement uses `MAP_FIXED_NOREPLACE` and verifies the returned address; collision candidates are bounded and no existing mapping is overwritten. This reserves only four pages, not a full guest session. The native function receives the actual buffer pointer. CPU3 receives that same pointer as `canonical_pointer - selected_guest_shift`; dynamic pointers are already actual addresses, so the shift is applied once. Code has a separate RW-to-RX mapping and instruction-cache flush.

There are 180 cases: 96 combinations of twelve lengths and all eight byte alignments; 48 short strings near the trailing guard; 32 fixed-seed random cases; and four strings extending almost across both accessible pages. The latter cases end at the final accessible byte. Filler bytes are nonzero and include high-bit values; each string has one deliberately placed zero. Aligned word reads may include readable padding after that zero within its word. This test does not claim byte-granular absence of overreads inside an accessible word.

For each case, the helper snapshots the entire two-page input, changes it to read-only, and checks native length, CPU3 length and the known expected length. It compares the full buffer after each implementation. Guard/permission checks, all-eight-alignment coverage, caller FPCR/FPSR restoration and host TLS/x18 preservation are required. A write or guard violation may terminate the dedicated process; that is failure evidence, not a pass or a reason to substitute a simpler implementation.

A passing report must show `state=passed`, `cases=180`, `failures=0`, `alignment_mask=255`, `near_guard_cases=52`, and true RX, guards, read-only inputs, unchanged buffers, actual-low-guest buffers, restored FP state and preserved host TLS/x18 fields. Preserve stdout, stderr, process status and exact helper/library identities. A pass would establish this one memory/branch function on these guarded inputs. It would not establish arbitrary memory instructions, arbitrary native rewriting, complete guest services, real SDK/thread operation, game boot, GPU correctness or a power/speed advantage.
