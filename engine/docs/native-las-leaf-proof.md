# Native LAS leaf comparison

This optional experiment compares one reviewed original SDK `fabsf` leaf against its actual compiled CPU3 descriptor function. It follows the twice-passed asset-free ARM64 bridge proof. It does not add a game backend or attempt SDK startup, game initialization, graphics, controls or performance benchmarking.

## Measured Thor result

On 2026-10-25, the coordinator executed the comparison in a fresh app-UID Python helper process on the owner's AYN Thor, using the completed on-device LAS library and its matching installed runtime. The process returned status 0 with empty stderr. The report recorded `state=passed`, `error=none`, twelve cases and zero failures; every native result, actual CPU3 function result and expected float32 bit pattern agreed.

The twelve cases cover both signs of zero, 1, 1.5, the smallest normal value, the largest finite value and infinity. RX mapping verification, caller floating-state restoration and host TLS/x18 preservation all passed. No original guest initializer was called and no guest window was mapped. The complete result, library/helper identities and process status are retained through ignored `evidence/native-las-leaf-current.json`.

This measures one relocation-free, two-instruction SDK leaf under those inputs. It establishes neither real SDK initialization nor arbitrary guest execution, game boot, graphics correctness or a performance/power advantage. The [synthetic bridge's two earlier hardware runs](native-arm64-proof.md) remain a separate result with their own scope.

The original bytes, addresses, full NSO identities and generated-source details remain in ignored `evidence/native-leaf-candidate.json`. Tracked helper code contains an independently constructed ISA whitelist, not an extracted asset array. Only scalar `FABS S0,S0; RET X30` is accepted, with exactly eight supplied bytes. Every single-bit mutation of either instruction is rejected by host tests.

Prepare with the coordinator's full measured hashes from the completed on-device library receipt:

```sh
python3 tools/check_native_las_leaf.py \
  --game-sha256 <full-completed-device-library-sha256> \
  --runtime-sha256 <full-matching-installed-runtime-sha256>
```

No ADB command runs. The tool requires two successful hardware bridge reports matching the frozen bridge artifact. It independently rehashes the original modules and SDK text, rechecks the exported symbol and no-relocation instruction whitelist, and validates the matching generated table slot and source-unit hashes. It compiles actual `Cpu`, `ModuleAbiDescriptor`, module validator and title-manifest definitions from the public headers/sources with cached NDK r27c/API30. Host whitelist tests run optimized and under ASan/UBSan. The helper DSO exports only `native_las_leaf_probe`.

Ignored output is under `build/native-las-leaf-proof/`: the helper DSO, `run_leaf.py`, `private-config.json`, and `verification.json`. The private configuration carries the eight original bytes, twelve reviewed cases and a probe-only expected sidecar. Its module metadata is checked against independently hashed producer inputs; only the library hash field changes to the coordinator's completed device ELF hash. The production sidecar, runtime, game library and existing application are untouched.

After the coordinator stages those three runnable inputs into its own private directory, run an already installed Android Python in a dedicated fresh helper process, with a 30-second outer deadline:

```sh
LD_LIBRARY_PATH=<directory-containing-runtime-dependencies> \
  python run_leaf.py private-config.json \
  <absolute-runtime-library> <absolute-completed-game-library> \
  <absolute-helper-library>
```

The runner verifies all three library hashes before loading the helper. The helper loads the runtime and game ELF, validates the complete actual descriptor against the independent profile, and selects the SDK function by its exact table address. It calls no original NSO initializer, dispatcher, `nnMain`, runtime main entry or GPU setup and reserves no guest window. Normal host ELF constructors/destructors can run during `dlopen`/`dlclose`; these are distinct from original guest initialization.

The original leaf is copied into anonymous RW memory, instruction-cache flushed, changed to RX and checked against `/proc/self/maps`. Both implementations run with matching FPCR/FPSR values of zero; twelve positive/negative zero, normal and infinity inputs are compared bitwise in the low float32 result lane. NaNs and subnormals are excluded. FPSR/FPCR behavior and host TLS/x18 preservation are checked; the caller's original FPCR/FPSR are restored on normal/error returns. Other guest vector lanes and full Cpu state are outside this comparison.

A passing device JSON must report `state=passed`, `cases=12`, `failures=0`, all case rows passing, RX verification, restored FP state and preserved host TLS/x18. Save its stdout, return status and verification artifact beside the earlier device evidence. A hardware fault or failed library load is a failed experiment, not a reason to silently substitute an implementation. Host compilation remains distinct from device validation; the measured pass above establishes only this one leaf under these inputs.
