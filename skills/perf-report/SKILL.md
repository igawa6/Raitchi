---
name: perf-report
description: Use when measuring or comparing AC-NativeNX performance (FPS, frame pacing, load time, thermals, memory) on one real Android device and filing a comparable report. No build tools required. Read-only measurement, no phone tuning.
---

# Performance report

A report is useful only if someone else can compare it. One named device, fixed scene, fixed protocol,
real measurements.

## Ground rules

- **One named device per report**, by model (for example "Galaxy Z Fold5"). Never generalize to another SoC:
  a gain on one can be a loss on another.
- **Read-only.** Do not "optimize" the phone: no debloat scripts, no downloaded tuning recipes, no system
  settings flipped, no driver swap, no uninstall or `pm clear` (it erases the owner's imported data). A setting
  or driver A/B is a separate, explicit decision of the device owner and goes in its own report.
- **Privacy.** Never read SMS, contacts, photos or other apps' data. Only the AC-NativeNX process and
  system performance counters.
- **Measurements vs estimates.** Every number comes from a run, with its source (log line, tool output).
  Anything else is labelled "estimate". Never write a figure from memory. A figure you announced then retract
  costs trust in all the others.
- **No game content** in logs or screenshots (no keys, no dump paths).

## 1. Probe first

Run the read-only probe from the `report-device-issue` skill (SoC, GPU, driver, Vulkan features, display modes,
RAM, build). A measured capability can make a whole idea pointless on this phone; say so.

Then read the **ceilings before the frame rate**:

- Battery saver: `settings get global low_power` and `dumpsys power | grep 'Battery Saver is currently'`.
  When ON it caps CPU frequencies; report it, do not switch it off yourself.
- CPU caps: `scaling_max_freq` for the small and big cores (`/sys/devices/system/cpu/cpu*/cpufreq/`).
- GPU on Adreno: `max_gpuclk` under `/sys/class/kgsl/kgsl-3d0/` is a *ceiling*; the real clock is
  `devfreq/cur_freq`. Report both with the busy percentage if readable.
- Thermals: `dumpsys thermalservice | grep -A3 "Current temperatures"` before and after the run.

## 2. Protocol

1. Same build (version or full commit SHA) and same settings, all listed (resolution scale, GPU driver,
   CPU backend, every non-default option). Not charging, battery above 50%, brightness fixed, device cool
   (wait 5 minutes after a previous run). State refresh rate (60 or 120 Hz).
2. A scene anyone can reach, described in one sentence (location or menu; same save if the user has one).
3. Run at least 3 minutes, and for a thermal verdict 10 to 15 minutes: thermal stability beats first-minute FPS.
4. Record average FPS, 1% low FPS and the worst gap between frames; time to first frame; and whether game speed
   was right (too fast or too slow). Repeat once; if the runs differ by more than 10%, say so.
5. If the phone is shared, note the app PID before and after and discard windows where someone else touched the phone.

## 3. Read the numbers honestly

- **Displayed FPS is not game speed, and a cadence is not a cost.** A line like "present pacing: 30 fps" is the
  frame rate the app asks for. Judge a 60 fps mode by frame regularity (worst gap), not the average; in modes
  that advance the world per frame, missing 60 slows the game instead of just dropping frames.
- The app's own frame counter is not a measurement until you know over what window it counts. Cross-check
  frames drawn vs frames shown.
- **Rule out non-code causes before blaming code**: effective render resolution per scene (check the profile
  line in the log, and how late it switches after a scene change); rendering continuing in the background after
  the app is paused (CPU burn, `am_kill ... excessive cpu`); diagnostic probes left on (look for log or dump
  files growing in the app's storage, not only for text in the log); battery saver and thermal caps.
- A thread at 100% of one core is not necessarily the critical path (it can be a busy-wait). Say which core
  type it ran on only if you measured it.
- Memory: `dumpsys meminfo com.accreation.nativenx` (TOTAL PSS, Private Dirty, Graphics, Native Heap) next to the
  system total. A reserved virtual address range in a log is not memory use.
- Driver A/B: first confirm from the app log which Vulkan library each run actually loaded; otherwise you may
  be comparing the same driver twice.
- A change that lowers quality or adds a wait to raise a number is not a performance fix; compare before and
  after on identical protocol and settings.

## 4. Submit

Open the **Performance report** form (`.github/ISSUE_TEMPLATE/perf_report.yml`). Include the probe facts, the
ceilings, both runs, and a list of what you did NOT measure. Add `Agent: <name> · Model: <id>` if an agent
wrote it and say who took the measurements; "not device-measured" if nobody did.
