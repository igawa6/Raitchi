# Eevee whole-window blinking

2026-10-25, AYN Thor / stock Adreno740 driver. The owner described constant
whole-image flashing. The first screen recording reproduced fully black frames,
including the host FPS overlay, between otherwise correct images. This occurred
at controller selection and later introductory dialogue. Native logs showed zero
shader compilation and skipped draws during steady portions of the failure.

## Cause and correction

The renderer requested a minimum swapchain image count but treated eight as an
upper bound on what the driver could return. The actual Android mailbox
swapchain contained **ten** images. The original code silently clamped its count
to eight, ignored the truncated image-enumeration result, and provided only eight
image, view and completion-semaphore slots. Acquiring images8/9 could therefore
index past the initialized storage. Two missing images in a ten-image rotation
matches the observed approximately20% black-frame rate.

`runtime/nvn/vulkan/vk.h` and `device.c`, exported through the tracked renderer
patch, now use a shared32-image storage bound for every image-indexed array.
Swapchain enumeration results are checked; the returned count is preserved and
every image gets a view. Empty/oversized counts and invalid acquired/presented
indices fail explicitly. The requested minimum, render thread and normal mailbox
presentation remain unchanged. Supporting a driver returning more than32 images
would need another capacity change; such a count is now rejected rather than
silently truncated.

An optional `LARECOMP_VK_ACQUIRE_FENCE=1` diagnostic compares CPU-fence acquisition
with the original GPU-semaphore path. Fence acquisition alone did not fix this
bug. The final normal path retains GPU-semaphore acquisition. A startup log now
records the actual image count, acquisition method and selected presentation mode.
The CPU/generated game-code identity is unchanged.

## Device comparison

Eight-second recordings were decoded into complete192x108 RGB frames sampled at
30fps. An entirely black sample has maximum pixel channel0; the overlay is
included. These are recording samples, not independently measured display-refresh
events. The comparison used steady scenes where a complete black frame was not
intentional. Capture can affect timing; this evidence does not establish every
future scene or Android driver.

| Configuration | Entirely black samples |
| --- | ---: |
| Original mailbox, introductory dialogue |47/239 |
| Original storage, render thread disabled, controller page |48/240 |
| Original storage, FIFO, controller page |27/240 |
| Original storage, CPU-fence acquisition, controller page |48/240 |
| Corrected storage, original mailbox/semaphore path, controller page |0/240 |
| Corrected storage, original mailbox/semaphore path, title movie |0/240 |

The actual corrected startup reports ten images and GPU-semaphore acquisition.
The process remains alive and readable at approximately30 game frames/s.
The fixed count handling, rather than a presentation-mode or threading workaround,
removes the captured controller-page flashes.

The title-movie comparison used a finite diagnostic A-button script to pass
controller confirmation. Native Opus decoder-open events and advancing VP9 movie
frames were observed without a fatal. This does not establish audible output or
full gameplay correctness. The temporary script was removed and the app restarted
with normal controls before the owner handoff; no automatic cutoff was set.

## Reproduce the capture

```sh
python3 tools/capture_letsgo_blinking.py --serial <device> \
  --label swapchain-count-fixed --duration 8
```

The tool requires the separate reference app to be alive and focused on display0.
It never launches, stops or controls the app. A unique temporary screen recording
is pulled and only that owned temporary file is removed. Video, a16-image contact
sheet, per-frame metrics, native logs and receipt metadata stay ignored under
`evidence/letsgo-blinking/`. It needs only the cached host FFmpeg, without numpy
or downloads. All-black samples can be intentional in other scenes, so the metric
alone is not a universal graphics test.

Native Android compilation of the affected device, execution and ImGui glue
translation units passed; the complete APK linked, passed signature verification
and installed separately. The private title library still matches its generated
contract. Exact private runtime/APK hashes and capture receipts remain local.
No existing emulator/LAS installation, save or performance setting was changed.
