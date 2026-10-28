# 0.0.1 maintenance pass

The maintenance pass keeps existing runtime behavior and separates independently
testable responsibilities. It does not claim a general FPS improvement.

- Shader job storage and foreground/background scheduling now live in
  `ports/shared-android/runtime/nvn/shader_job_queue.{c,h}`. The renderer retains
  policy, compiler startup and Vulkan work. Foreground priority, loading capacity
  and gameplay background limits remain unchanged. Allocation failure reports a
  terminal runtime error before queue mutation. A failed translator thread start
  also reports an error rather than leaving queued shaders waiting indefinitely;
  the successful translator is detached, matching process-lifetime workers.
- The kernel's GetInfo switch is isolated in `process_info`. Selector 28 now
  validates its arguments and reports zero optional alias-region extra space,
  matching this runtime's configured layout. This is separate from allocating a
  larger guest address space; other missing memory contracts remain.
- The Android shell stores directory lists through `ShellDirectoryPreferences`.
  Invalid persisted JSON/entries no longer abort library loading. UI integer
  preferences write directly, removing recursive calls between string/integer
  setters. Boolean input retains `0/1` and `true/false`; invalid strings preserve
  the prior setting. Reset batches its preference writes. Write-only configuration
  state was removed.
- Host checks use the canonical IPC header for the audio arithmetic fixture and
  recognize the current preparation gate and pipeline creation wrapper. These
  changes repair stale checks without weakening their ordering requirements.

Verification includes optimized and ASan/UBSan queue scheduling, failure-path,
GetInfo ABI and audio fixtures, shader replay integration, the native Android
build, Android renderer checks, shell preference regressions, lint and Android
unit tests. Device launch stages are recorded separately in the compatibility
report. Synthetic failure injection is not evidence of an observed device OOM,
and startup improvements do not establish whole-game compatibility.

Reachable diagnostic paths were retained. A symbol occurring infrequently is not
sufficient evidence that it is dead code. Larger renderer decomposition remains
future work requiring separate ownership and graphics regression checks.
