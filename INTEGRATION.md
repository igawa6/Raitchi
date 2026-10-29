# Source integration after 0.0.1

This update incorporates reviewed contributions from PRs #2, #3, #4, #5, #7 and
#8, with additional fixes found during integration. Thanks to ptichalouf for the
contributions; existing upstream copyright and license notices remain intact.

- Publish prepared native manifests atomically without replacing existing files.
- Link SDL3 for 16 KiB pages and check all packaged native libraries and APK alignment.
- Honor host-test macros on ARM64 without changing the device execution path.
- Respect the software keyboard UTF-8/UTF-16 output flag, bound guest reads and
  writes, and preserve complete Unicode characters in input and deletion.
- Use bounded stack text conversion to avoid the observed Android conversion
  stall and memory growth during Eevee name entry.
- Use the activity lifecycle for driver downloads, avoiding the reported context cast crash.
- Document contribution workflow and track proposed features as a backlog.

## Validation

The shared host suite passed 104 checks. The isolated public source passed 71
host checks, Android compilation, debug/release APK builds, lint and unit checks.
After the bounded text conversion change, focused tests passed 85,028 HLE and
213 UI assertions with optimized and sanitizer builds, plus Android compilation.
All six packaged ARM64 libraries and both public APKs passed 16 KiB checks.
Cached public dependencies were reused; this is not a fresh-machine build claim.

On AYN Thor, development build 100 visibly returned an accented name in LAS
using UTF-8. Build 102 completed both Eevee UTF-16 name-entry calls, reached the
bedroom, and ran for seven minutes below 1.9 GiB RSS. The final LAS 102 repeat
reached title/movie screens but did not reach the keyboard through injected
controls; that repeat remains unqualified. A real driver download and install
also succeeded. These device builds are distinct from the public validation APK.

The public validation APK uses a separate test signing key and is not distributed
as an update. The tagged 0.0.1 release remains unchanged. No new general gameplay,
30 fps BOTW, or 16 KiB physical-device compatibility claim is made.

Issue #1's roadmap remains a backlog, not a declaration that its proposed features
are implemented. Issue #6's reported preparation and downloader code paths are
addressed; confirmation on the originally reported devices remains useful.
