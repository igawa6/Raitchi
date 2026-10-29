# Raitchi 0.0.2 Android preview

Bug-fix release for the experimental Android ARM64 launcher and shared runtime.

- Fix native-manifest publication when Android denies hard links, preserving existing metadata.
- Fix driver-download crashes caused by casting a themed context to a lifecycle owner.
- Respect each game's UTF-8 or UTF-16 software-keyboard output mode. Bound guest
  reads/writes and use stack conversion to avoid Eevee's observed conversion stall
  and memory growth. Preserve whole Unicode characters when editing input.
- Link SDL3 for 16 KiB pages and verify every packaged native library and APK alignment.
- Fix ARM64 host-test build selection and improve regression checks.
- Add contribution guidance and a feature backlog.

Thanks to ptichalouf for the reviewed contributions in PRs #2, #3, #4, #5, #7 and
#8. Upstream and contributor license notices are retained.

## Install

The ARM64 APK uses the same package and signing certificate as 0.0.1, with
version code 2. Install it over 0.0.1; uninstalling is unnecessary. Games, keys, firmware and
saves are not included.

## Validation and limits

Release build, lint, unit checks, signature verification and 16 KiB ELF/APK
alignment checks passed. The isolated public-source suite passed 71 host checks;
focused keyboard tests passed 85,028 HLE and 213 UI assertions with optimized
and sanitizer builds, plus Android compilation. Cached public dependencies were used.

Live development tests verified LAS UTF-8 name entry, Eevee UTF-16 name entry
and continued execution, and a real driver download/install. These development
artifacts differ from this release. LAS's final development-build keyboard repeat
was not completed through injected controls. The release APK has not received a
fresh full-library gameplay sweep or a physical 16 KiB-device test.

Compatibility remains experimental. Existing boot observations and earlier
bounded gameplay evidence are in TESTING.md; a title screen is not a playable-title
qualification. No universal performance improvement or 30 fps BOTW claim is made.
