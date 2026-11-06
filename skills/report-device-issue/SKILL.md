---
name: report-device-issue
description: Use when a user sees a crash, black screen, wrong audio/video or startup failure of AC-NativeNX on an Android device and wants a reproducible bug report. No build tools required. Probes the device read-only (SoC, GPU, driver, Vulkan features) and collects logs without any game content or personal data.
---

# Report a device issue

You do not need to build anything. Produce an issue someone else can reproduce.

## Ground rules

- **One named device per report.** Name it by model (for example "Galaxy Z Fold5, SM-F946B"). A result on one
  SoC says nothing about another. Same bug on a second device = a second report (or a comment on the first).
- **Read-only.** Observe, never "fix" the phone. No debloat scripts, no downloaded tuning recipes, no system
  setting changes (battery saver, refresh rate, developer options), no driver swaps, no uninstall, no `pm clear`
  (it erases the user's imported data). If a setting looks relevant, report it and let the owner decide.
- **Privacy.** Never read or paste SMS, contacts, photos, accounts or other apps' data. Stay on the AC-NativeNX
  package, system properties and graphics information.
- **Measurements vs estimates.** Every fact comes with its source (the command or screen). Anything not
  observed is labelled "estimate" or left out. Never write "works" or "tested on device" for something that was not run.
- **No game content.** No games, keys (`prod.keys`, `title.keys`), firmware, saves, dumped shaders, or paths that reveal
  where they are kept. The game *name and version number* are fine; hashes of the user's dump are not.

## 1. Probe the device first (read-only, `adb shell` or the device's own screens)

| Fact | Command / source |
|---|---|
| Model, Android version, ABI | `getprop ro.product.model`, `getprop ro.build.version.release`, `getprop ro.product.cpu.abi` |
| SoC | `getprop ro.soc.model` (and `ro.soc.manufacturer`) |
| RAM | `grep MemTotal /proc/meminfo` |
| GPU and driver | `cmd gpu vkjson` -> read `properties.deviceName`, `driverProperties.driverName` / `driverInfo`; stock vs replacement driver |
| Vulkan features that matter | in the same JSON: `textureCompressionBC`, `textureCompressionASTC_LDR`, plus the Vulkan API version |
| Display | `dumpsys display`: supported refresh rates and active mode |
| Page size | `getconf PAGESIZE` (16 KiB devices need 16 KiB-aligned libraries) |
| Installed build | `pm path com.accreation.nativenx` then `dumpsys package com.accreation.nativenx` (versionName, versionCode) |

Mali and other non-Adreno GPUs are unsupported: the app should say so (unsupported GPU, missing Vulkan
features, missing keys) instead of crashing. A crash there is still a valid report, mark it "unsupported GPU".

Which GPU driver *actually loaded* comes from the app log (the line naming the Vulkan library), not from a
driver selector in the UI: a selector proves nothing. If the log does not name it, say "driver load not confirmed".

## 2. Describe the failure

Fresh-launch steps, expected vs actual, frequency (always / sometimes / once), and whether it worked in an
earlier build (give the version). Say whether keys/firmware were configured in the app. If a first-run
message appeared (unsupported GPU, missing keys), copy its exact wording.

## 3. Logs (check them before pasting)

`adb logcat -d -b crash`, then the last ~300 lines of `adb logcat -d` filtered on the package name and
`vulkan|fatal`. Also the app's own log file if it has one. Remove game text, key material and personal paths.
If the app seems to run while in the background, or an `am_kill ... excessive cpu` line appears
(`adb logcat -d -b events | grep am_kill`), say so: it is a separate bug.

## 4. Submit

Open the **Bug report** form (`.github/ISSUE_TEMPLATE/bug_report.yml`) and fill every required field.
If an agent wrote it, add `Agent: <name> · Model: <id>` and state which observations a human made on the real
device and which the agent only relayed.
