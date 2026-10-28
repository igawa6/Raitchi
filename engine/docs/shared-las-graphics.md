# LAS shared renderer investigation

Observed on the owner’s Thor with the separate shared test app, 2026-10-25.
Original reference applications and saves remain untouched. The owner explicitly
authorized interrupting the shared session for continued implementation.

The initial dark outdoor scene came from capability selection: Turnip reports
no shaderInt64, so the shared renderer selected a read-only snapshot path that
rejected actual per-frame compute stores. The writable 64 MiB mirror now also
supports uint32 low/high guest address arithmetic. Existing ordered GPU stores,
CPU merge and vertex readback are retained. Drivers with a smaller storage-buffer
limit retain the read-only path and reject unsupported stores explicitly.

The opening movie and water needed a separate startup correction. The pinned
LAS Android port disables Turnip LRZ with `TU_DEBUG=nolrz` before Vulkan startup.
Shared startup had omitted this default. Restoring it makes the opening movie
render on Thor; the owner also confirms water is correct. LAS retains its pinned
Turnip vertex-input defaults; Eevee keeps the verified stock-driver static-input
settings. Explicit environment-file overrides still take precedence.

BowWow’s body was missing while the chain and collar rendered on the old
Turnip 24.3 build. The owner already had Turnip 26.2 R5 on Thor. A separate
app-UID offscreen probe confirms the older driver reports shaderInt64=false,
while 26.2 R5 reports true. The common runtime selects its existing64-bit shader
path automatically for the newer driver; no unsupported feature override is
used. BowWow now renders completely at 30 fps with that driver, static vertex
input, and the normal serialized render thread. Original reference driver files
remain untouched; the old shared-app driver was preserved separately. Staging
now preserves valid selected drivers instead of overwriting them with the old
reference driver. Full route and lifecycle reliability remain unqualified.

A forced64-bit diagnostic on the old driver also made BowWow visible, but that
is diagnostic evidence, not a supported shipping mode. Basic old and rewritten
uint32 address predicates both pass512 synthetic addresses per mode on the old
GPU driver; the newer driver additionally passes the advertised64-bit mode.
Therefore those small tests do not establish the cause of the complex shader
failure. No claim is made that the old driver's BowWow rendering is fixed.

Some diagnostic captures became invalid when Thor slept; the harness now wakes
it and refreshes user activity, and the game window keeps the display awake
without changing system settings. Tests that disabled the normal render thread
also produced startup/driver failures. Restoring the normal thread reached the
scene without validation overhead. A separate validation run reached BowWow
without a Vulkan error, at a lower frame rate due to instrumentation. The
optional cached-layer packager flag is disabled in the final normal APK. Mixed
vertex-mode diagnostic headstart data was set aside before the final profile.

The source audit also found headstart-cache invalidation gaps when changing
dynamic vertex mode, and a SPIR-V optimization-flag key omission. Those remain
follow-up items; complete driver/mode switching is not yet qualified.

Host checks in `tools/check_shared_writable_shaders.py` compile/validate 66 GLSL
and SPIR-V variants, execute 1,405 generated address-arithmetic comparisons and
1,872 emitted integer-addition comparisons, and
check map bounds and actual production guest-range validation. These checks do
not establish GPU ordering or complete graphical accuracy. Captured game shaders,
images, logs and artifact hashes stay in ignored private evidence.
