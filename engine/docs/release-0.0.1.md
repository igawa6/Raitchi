# Raitchi 0.0.1 release preparation

This document describes local release preparation. It is not a publication or
upload receipt. Android ARM64 is the initial target.

Included changes:

- Shared continuous NCE, package import/preparation, Horizon services, audio/input
  and NVN-to-Vulkan rendering behind an Eden-derived Android frontend.
- General settings and game library, original package/folder imports and retained
  per-title adapters for verified module identities.
- Bounded recorder prefix reuse/empty rebind, shader half/constant-buffer fixes,
  sparse local-memory GLSL lowering and audio routing fixes.
- Exact game/version shader metadata catalogs, original-source resolution,
  persistent discovery/cache replay and bounded preparation workers.
- LAS's 1,585 upstream preparation records merged with 221 observed records into
  1,803 exact metadata entries; three duplicates removed. Original shader bytes
  remain in the user's image. Echoes retains its separate 475-record catalog.

Compatibility evidence:

- Current library launch results are listed in the README and
  [full results](../compatibility/launch-results-0.0.1.md). Launch-only screens,
  automatic demos and menus do not qualify games as playable.
- Earlier bounded LAS, Eevee, Echoes and Wonder save/movement scenes have run;
  none is a complete-title compatibility certification.
- Fire Emblem's earlier class demo improved from roughly 5–6 to 30 fps after a
  general shader local-memory fix. Unsupported shader/compute work and broader
  rendering limits remain. This does not certify Fire Emblem as playable.
- Only five LAS rendering-state records resolve against Echoes; they add four
  pipeline variants but no new shader program specializations. A diagnostic
  480-record Echoes trial passed a bounded scene; the default catalog stays 475.
- Fire Emblem/Wonder BGM recovery has not been confirmed by audible testing.
  Residual LAS/Echoes speckles, rendering limits and unimplemented shaders and
  services remain. Graphics/audio are not claimed fully fixed.
- First-use compilation, source inventory, streaming and guest startup stalls
  remain. Direct SPIR-V and reusable Vulkan pipeline libraries are experimental,
  default-off paths; the selected test driver did not expose GPL support.

Release preparation status and remaining steps:

- The candidate56 launch sweep is complete: 58 catalog entries, 47 runtime
  attempts and 11 metadata blockers. Of the runtime attempts, 16 survived the
  bounded check (11 visible startup/menu/intro routes and five black screens),
  28 returned runtime errors and three exited. These counts do not certify
  gameplay. Candidate57 adds targeted checks, not another full-library sweep.
- The source staging retains upstream notices and the new root LICENSE, COPYING
  and NOTICE.md component map. Copyright and license headers remain authoritative
  for individual files; no permissive license is substituted for GPL components.
- The prepared source directories are filesystem snapshots without Git metadata
  or fetched references. The current shell builder requires Git identity and
  source verification requires pinned fetched repositories. Restore the shell
  with its pinned upstream history using the source-overlay manifest/helper;
  the local-cache metadata/hash validation passed without a full build. Native
  dependency bootstrap and a new-checkout APK build remain separate steps. See
  [building](../BUILDING.md) and [overlay setup](SOURCE-OVERLAY.md).
- A new public-checkout build is still needed after maintained fork integration
  commits are selected; the distribution URL is already defined. This cannot be established by a working-tree
  build. All eight candidate57 targeted checks completed: Fire Emblem reached
  its automatic class demo at 30 fps, Eevee controller selection at 30 fps, Wonder
  and Echoes title screens at 60 fps, and LAS intro at 30 fps. Three newer-contract
  callers advanced through early initialization but failed before presenting
  frames. These are bounded launch observations, not gameplay qualifications.
  All 49 recorded save files remained byte-identical and settings/driver restored.
- The authorized publication destination is [igawa6/Raitchi](https://github.com/igawa6/Raitchi).
  Preparation targets a draft release; final publication is a separate step.
  The release APK uses the retained local test certificate, not a newly claimed
  production signing identity.
- Integration checks passed: 68 host checks, 21 Android checks, debug/release lint
  and four unit tests. Candidate57 release APK: 16,117,101 bytes; SHA-256
  `ffc149aeda040261bd936308ae3cd3a3cfb48f90eab6a7fdc4b912713d82207b`.
  These checks do not establish a full clean-checkout build.
- The prepared source snapshot excludes games, keys, saves, private logs,
  device identifiers, signing secrets and generated game/shader payloads.

No maintainer identity, repository URL, signing secret or legal authorization is
invented by this preparation. Existing source notices remain authoritative for
their respective components.
