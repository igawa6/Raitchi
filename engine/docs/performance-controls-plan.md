# Performance controls and next steps

These notes distinguish current source behavior from proposed work. No new
device performance result is implied by an Android build.

## Frame rate

The overlay counts presented pictures. It does not measure game simulation
steps. Positive guest NVN present intervals are paced; interval zero currently
has no additional software limit. Menus and loading can choose a different
interval from gameplay. A loading-screen report must be measured there rather
than inferred from a gameplay screenshot.

A general presentation-limit option should be separate from native game-mode
patches. Add a per-game Game/default, 30, 60 or display-refresh policy at the
shared presentation boundary, using monotonic deadlines and resetting pacing
after long loading gaps and pause/resume. Preserve slower positive guest
intervals. Tests must include zero intervals, 30/60/120 Hz, late frames, long gaps
and configuration switches. A cap can reduce unnecessary presentation work;
it cannot make a 30 fps simulation run correctly at 60.

LAS has verified title/build-specific native 30/60 hooks. Extending this to a
new title requires measured tick/frame-mode sites, executable identity and
expected instruction checks, then validation of physics, animation, audio,
timers and transitions. A global interval rewrite is not an equivalent patch.

## Operation mode and resolution

Current AM/APM/display replies report docked mode. Settings now display named
choice labels and native-relative scale factors. Internal scaling is currently
qualified for LAS; other adapters deliberately retain native guest render
targets. These factors do not promise universal 720p or 1080p output.

The intended controls are separate operation mode (Handheld, nominal 720p;
Docked, nominal 1080p) and resolution factor (1x, 1.25x, etc.). Operation mode must
consistently drive applet/performance/display queries and touch coordinates.
After a game chooses targets for that mode, apply the factor to coherent
eligible attachment groups. A game may use dynamic or lower internal sizes;
1x means its chosen target, not a forced nominal resolution. Do not apply an
extra 720/1080 ratio after the guest already selected its handheld targets.

General scaling requires consistent color/depth/MRT dimensions, viewport,
scissors, sampling, feedback copies and texture views. Enabling LAS heuristics
for every title risks correctness regressions. Test native 1x first, then each
fractional factor across mode changes, mixed-size passes, shadows and UI.
Wonder's corrected black sky was a linear render-target layout error, not an
operation-mode error.

## Stutter

The runtime already translates asynchronously and builds pipelines on worker
threads, with priority for foreground work. Required first-use draws and
compute dispatches still wait for readiness. Skipping them can remove objects
or leave GPU inputs unwritten. Known graphics and compute recipes replay
before gameplay on later launches; unseen variants may still stall.

Next steps, in order:

1. Measure exact-frame readiness waits, queue delays, upload/snapshot time and
   cache persistence, with identical diagnostic settings for cache seeding and
   replay. Diagnostic environment changes can invalidate replay identity.
2. If foreground graphics recipe persistence is material, move its immutable
   records to a bounded writer queue, preserving fail-closed validation,
   deduplication, ordered persistence and shutdown drain behavior.
3. Optimize the measured missing variants or data-transfer work. Warm frames
   with zero compiles need separate diagnosis; they are not automatically
   shader stalls.

Use the same executable, save route, driver, operation mode, resolution,
performance settings and command-lifetime mode when comparing runs. Report
cold visits and fresh-process warm replay separately, with frame percentiles
and worst frames rather than only the FPS overlay.
