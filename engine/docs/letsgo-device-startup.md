# Let's Go Android device startup

2026-10-25 local time, AYN Thor / Android13 / Adreno740. The owner explicitly
finished manual LAS play before these separate-app actions. No existing emulator
installation, LAS save or device performance setting was changed.

## Verified startup

The asset-free `com.recomp.letsgo.test` APK installs separately, loads its native
runtime and renders the upstream frontend with readable text on the stock
Adreno740 Vulkan1.3.128 driver. This is frontend evidence, not guest graphics.

The native constructor successfully reserved the entire unshifted guest window
`[0x7000000000,0x7320000000)`, 13,421,772,800 bytes. App-UID `/proc/<pid>/maps`
inspection independently confirmed a single read/write mapping covering the
whole window after Vulkan initialization. A second fresh process after the
orientation update also logged successful full-window reservation. This supports
using the original address profile for this reference; it does not establish
all devices, reboots or future process layouts.

The first screenshot was black because the device was already asleep: the Java
activity existed, while SDL's native main had not started. Waking it allowed
native startup. The launcher helper now wakes before launch and supports
read-only `--observe-only` capture. Native logs are mode0600; it reads them via
the application's debug UID. An unreadable/missing log now records unknown
reservation status instead of reporting a failed mapping.

## Orientation correction

The first active frontend was rotated90 degrees inside its landscape window.
Android's Vulkan surface reported currentTransform ROTATE90, while the original
renderer selected that transform without pre-rotating its output. The Android
port now selects supported IDENTITY, letting the compositor rotate normally,
and handles its expected SUBOPTIMAL result without perpetual swapchain rebuilds.
The updated screenshot is upright and readable. Desktop behavior is unchanged.
The thin APK uses an Android theme without an action bar; upstream fullscreen
settings still control the final native window's system bars.

## Private inputs

`tools/push_letsgo_inputs.py --serial <device>` streams the existing private
exefs/romfs through tar directly into a new staging directory under this app.
It refuses an existing destination, transfers no keys and creates no large
intermediate archive. 27,824 files / about4.46GB transferred. All four NSO SHA256
values and total file count matched when checked as the app UID.

Let's Go requires `extracted/romfs.bin`; a loose directory alone does not satisfy
its setup/runtime contract. `tools/build_letsgo_romfs.py --serial <device>` builds
a small Android CLI around the unmodified upstream `runtime/romfs_build.c` and
packs the transferred directory on-device. The complete file table contains
27,818 files and 4,428,561,283 content bytes, matching the host directory. The
image's app-UID access and SHA256 were verified before publishing its final name.
The original loose inputs are retained. No game execution is established by
packing or hashing the image.

Exact APK/hash observations, screenshots, native logs, memory maps and transfer
receipts remain ignored under `evidence/letsgo-launch/`,
`evidence/letsgo-input-transfer/`, and `evidence/letsgo-romfs/`.

## Subsequent guest gate

The next gate was to finish all973 objects (972 generated units plus matching ABI3 entry), link and
verify the private title library, stream it directly into app-internal storage,
and boot a fresh process against the matching installed runtime. The boot helper
uses a separate baseline profile: 30fps,720p, bilinear, no frame generation or
optional AA/effects/interface texture pack, default handheld input mode. It sends
no game inputs and has no timed close. It refuses an active owner session.

The subsequent [guest-boot report](letsgo-guest-boot.md) records completion of
the full library, four-module initialization and readable controller selection
with actual NVN rendering on both stock and Turnip drivers. Intro, audio,
gameplay and save/reload remain pending. The shared-runtime identical-artifact
gate remains open.
