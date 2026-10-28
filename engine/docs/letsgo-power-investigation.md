# Eevee CPU and temperature investigation

2026-10-25. The owner reports the corrected game working but feeling hotter than
the emulator. Emulator resolution, frame cap, scene, fan/performance mode and
charging conditions were not established, so this is not a matched efficiency
comparison. The report is a positive manual gameplay observation, not separate
confirmation of capture, audio quality, save persistence or sustained performance.

## Measured waste and fix

Read-only thread sampling found one SDL thread near a full core during a stable
30fps scene. An8-second,200Hz user-cycle profile attributed58.74% of sampled
cycles to the clock helper itself. Available caller chains ran through `ui_run`,
SDL event waiting, joystick updates and tick reads. Frame-pointer unwinding was
incomplete, so caller percentages are not a complete allocation of all CPU time.
Steady native logs showed zero shader compilation/skips; compiler activity does
not explain the persistent hot UI thread.

The pinned LAS Android renderer already documents the same SDL Android issue
and avoids `SDL_WaitEventTimeout`: poll queued events, sleep4ms when initially
empty, then drain the current event batch. The Eevee port now uses that same
Android-only loop. Existing quit, input, menu, window-request and reboot handling
remain intact; desktop event waiting is unchanged. Startup logs the idle policy.
No mutex-spin, renderer, resolution, frame-cap or device performance policy was
changed for the comparison.

After explicit owner authorization, fresh processes were compared at the same
controller-selection scene with unchanged30fps/720p settings and warm shader
caches. Both intervals used12-second,200Hz user-cycle profiling. Process CPU time
includes user and system ticks read through the app UID;100% means one fully busy
core, so a multithreaded process can exceed100%.

| Build | Process CPU time | Measured wall time | Process CPU |
| --- | ---: | ---: | ---: |
| Original Android event wait |15.19s |12.334s |123.15% |
| Android polling with4ms idle sleep |3.32s |12.289s |27.02% |

This is about78% less process CPU time per wall second in this matched scene.
The frame rate remains approximately30fps. The change removes a measured source
of wasted CPU work; it does not establish78% less electrical power, a particular
temperature drop, or superiority over Eden. This is one before/after pair, not a
multi-scene benchmark. Thermal settling and normal gameplay need longer checks.

A subsequent20-second observation measured25.34% process CPU while the game
advanced to the title movie; it is not another matched-scene comparison. Readable
CPU-labelled sensor values trended down during that observation, but scene
changes and thermal inertia prevent assigning a temperature delta to this patch.
An additional eight-second graphics regression capture contains0/240 entirely
black samples. The app remains open with normal controls, no scripted input and
manual-session protection restored.

Native compilation, full APK link/signature verification and installation passed.
The existing private game library still validates against the unchanged generated
code identity; no game-code regeneration was needed.

## Remaining work

Source review found additional candidates, not measured causes of this report:

- Guest mutex contention defaults to a40us spin based on desktop wake latency;
  `LARECOMP_LOCK_SPIN_US=0` is a later single-variable comparison. It was **not**
  changed in the UI-loop test.
- The mobile read-only shader mirror repeatedly snapshots bound ranges and
  metadata per draw. Transient allocation totals are scene dependent and include
  constants, vertices, maps and other buffers, not just texture uploads.
- Debanding remains enabled by default even with AA/frame generation/SSAO/GI off.
  `LARECOMP_DEBAND=0` would test that extra post pass separately.
- Shader workers sleep when idle. Reducing compiler-worker count would address
  cold compilation bursts rather than this zero-compilation steady-state case.

Retain the working image-count and audio fixes. Regress physical controls and
dialogue, then compare the same gameplay route under matched conditions before
changing another default. Existing emulator installations/settings are untouched.

## Repeatable observation

```sh
python3 tools/sample_letsgo_load.py --serial <device> \
  --label ui-idle-observation --duration 12
# Optional sampling profile; this adds profiling overhead:
python3 tools/sample_letsgo_load.py --serial <device> \
  --label ui-idle-profile --duration 12 --perf
```

The utility requires one live reference process focused on display0 and never
launches, stops or sends input to it. It measures process CPU ticks against a
host monotonic interval, detects PID/start-time changes, and retains paired raw
thermal sensor names/values, GPU nodes, battery state and native logs. Slow sensor
reads are outside the CPU interval. Optional simpleperf records use a unique owned
app-internal file, copied locally before removing that exact file.

On this Thor, Android's thermal service reports its HAL not ready; readable sysfs
sensors provide additional observations. Sensor types/units must be interpreted
individually. The battery's36°C reading is not a CPU temperature. External power
was connected and battery current read0, so these readings cannot supply a useful
whole-device wattage measurement. All device IDs, traces, sensor snapshots and
exact private build receipts remain ignored under `evidence/letsgo-thermal/`.
