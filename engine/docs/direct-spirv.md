# Direct Maxwell-to-SPIR-V compiler

Candidate53 adds an experimental native emitter to the common runtime. It does
not generate or parse GLSL and does not call shaderc for an admitted stage.
Direct and GLSL stages can share a program. Unsupported instructions retain the
existing compiler; no draw is omitted to disguise compilation.

## Activation and coverage

The default remains the existing compiler. Set `LARECOMP_VK_DIRECT_SPIRV=1` in the
existing app-private `env.txt` and start a fresh game process to opt in. Preserve
the owner's other environment/driver settings. `0`, an absent setting, or an
unknown value keeps direct compilation off. This host implementation did not
modify Thor's environment, install an APK or run a device session.

The admitted subset is straight-line vertex/fragment shaders using MOV, static
aligned LDC32/64, supported attribute I/O/interpolation, and scalar FADD, FMUL,
FFMA (including supported immediate forms). It preserves typed inputs/outputs,
varyings, vertex/instance IDs, point size, depth, coordinate remaps, std140
constant bindings and resolution-scale binding18. Vertex position remains
invariant and its arithmetic retains NoContraction.

Textures/images, dynamic constant addresses, control flow/predication, half and
integer arithmetic/conversions, compute and unsupported modifiers fall back.
Float target0 RGB fragment shaders also fall back because their dynamic
tint/recolour contract is not implemented in the direct emitter. Neutral current
settings are insufficient reason to omit that contract.

A survey of the existing private Fire Emblem seed journal admitted 53 of 335
stage occurrences. Deduplicated by original bytes, exact semantic keys,
partitions, stage and derived fragment varying mask, this is **11 of 158 unique
compilation inputs**: 11/61 vertex inputs, 0/82 fragment inputs and 0/15 compute
inputs. Those 53 occurrences produced 11 distinct direct modules, all accepted
by `spirv-val`. This is one bounded existing capture, not full-game coverage,
rendering validation or a speedup measurement. Only aggregate metadata is exported.

## Integration and recovery

`maxwell_compile_sized` chooses the compiler per stage. It requires bounded
immutable inputs for direct work, preserves debug/compiler overrides, validates
the output envelope and releases failed outputs before exact legacy fallback.
`ShaderInfo` owns source or SPIR-V. The module builder consumes direct words once
and keeps metadata for reflection and bindings. Proven first-use snapshots with
zero compiler constants now pass the exact zero extent; unproved original-memory
fallback retains its existing unknown-constant policy.

If `vkCreateShaderModule` rejects a direct module, the builder retries only that
stage through the legacy translator/shaderc using immutable inputs. It replaces
metadata/word ownership only after complete success and rejects a changed vertex
output interface. Reflection uses the final selected modules. Pipeline-creation
failures after program publication cannot safely mutate shared programs and do
not trigger this retry.

The wrapper exposes per-stage counters; runtime logs report direct emissions and
GLSL fallbacks. These count compiler selections, not rendered draws or driver
pipeline successes. A driver retry has a separate log entry. The legacy
translator lock remains around program translation; the new emitter itself has
per-call state and can be scheduled independently in future work.

Compiler/renderer fingerprints now include the direct emitter, selector, legacy
translator and their actual compilation dependencies/flags. The direct-mode
environment option participates in cache compatibility. Portable original-input
recipes remain game/version scoped and survive derived compiler invalidation.

## Numeric validation

The inspected Eden Maxwell decoder maps the supported flush mode to FTZ.
[NVIDIA's PTX reference](https://docs.nvidia.com/cuda/archive/12.1.1/parallel-thread-execution/index.html#floating-point-instructions-add)
documents flushing subnormal inputs and results to sign-preserving zero. The
emitter implements that with integer exponent/sign tests rather than assuming a
driver's floating-point default. Unsupported flush/rounding modes fall back.
This source/reference basis is not a Maxwell hardware qualification.

Software Vulkan tests uncovered FTZ multiply signed-zero folding, which is now
handled by restoring the operand XOR sign for a zero product before saturation.
The oracle compares direct output against an independent float32/fmaf plus
bit-level FTZ reference. Legacy GLSL ignores the decoded FTZ flag and sometimes
canonicalizes signed zero; resulting raw-bit differences are recorded explicitly
instead of being mislabeled as universal bit parity. Hardware accuracy and
performance remain unverified on Thor.

The host gate uses synthetic shaders only, validates modules with `spirv-val`,
and renders color/depth through an explicitly selected CPU Vulkan implementation.
It covers ALU operand forms, typed attributes/outputs, constants, varying modes,
coordinate transforms, fragment depth, empty fragments, saturation, NaN/infinity,
signed zero, subnormal inputs and underflow results. Policy/ownership and actual
render integration additionally run optimized and with ASan/UBSan, including
mixed compilers and driver-retry failure injection.

The final full host run passed 179 fixtures and 358 validated modules, 7593
front-end assertions per optimized/sanitized build, and 69056 software Vulkan
color/depth assertions. It records 43 known legacy differences (one signed-zero
case and 42 FTZ cases), with zero unexpected direct/reference failures. The
selector gate passed 1354 assertions per build; the actual render integration
gate passed 32 scenarios/876 assertions per build. These scopes are complementary
and must not be reported as a Thor game test.

Commands:

```sh
python3 tools/check_shader_compiler.py
python3 tools/check_direct_spirv_integration.py
python3 tools/check_direct_spirv.py --no-gpu
python3 tools/check_direct_spirv.py
```

The last command needs a software Vulkan ICD and Vulkan/shaderc development
libraries; the checker never installs a driver or downloads one automatically.
The default host-check suite runs the CPU/module validation subset. Explicit GPU
results and private-corpus admission results are archived separately. The optional
`tools/check_direct_spirv_coverage.py` requires an owner-provided validated journal
and emits metadata only; it is not a default/public-data test.

## Performance boundary

Removing source generation and shaderc can shorten the compiler front end for
admitted stages. It does not remove driver pipeline creation or asset streaming,
and explicit FTZ can add GPU instructions. Tiny synthetic front-end timing is
not an FPS result, a Thor speedup or a matched Eden comparison. Most fragment and
compute work still follows the existing path. Extending measured opcode/tint/
texture/control-flow coverage and measuring the driver stage are required before
calling this a general first-use hitch solution.

The pinned Eden compiler was investigated as an alternative. Its environment
specialization, descriptor allocation and global-memory lowering differ from
Raitchi's contracts, so it cannot simply replace the shaderc call. No Eden shader
compiler source is imported in this implementation.
