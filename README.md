# AC-NativeNX

AC-NativeNX is an experimental Android ARM64 launcher and shared runtime for
original Nintendo Switch games that you own. It is **based on Raitchi by igawa6**
([igawa6/Raitchi](https://github.com/igawa6/Raitchi), v0.0.2), maintained by
AC-Création with help from disclosed AI agents. Repository:
[ptichalouf/AC-NativeNX](https://github.com/ptichalouf/AC-NativeNX). Its Android frontend is derived
from [Eden](https://git.eden-emu.dev/eden-emu/eden) and the yuzu lineage. All
upstream copyright lines, SPDX headers and license texts are kept; the project is
GPL-3.0-or-later (see [NOTICE.md](NOTICE.md), [LICENSE](LICENSE), [COPYING](COPYING)).

> Status: preview. AC-NativeNX builds have **not yet been tested on a real device
> by AC-Création**. Nothing is published as a release until a device report exists.

## What this is

- Continuous native execution (NCE): the game's original ARM64 CPU code runs on
  the phone; Horizon services and NVN-to-Vulkan graphics come from a shared
  runtime. No per-game generated C is needed for the default path.
- An Eden-derived Android shell (game list, settings, GPU driver manager) that
  launches games in a separate `:game` process.
- Early compatibility work, not a claim that every game is playable. The game
  observations below come from upstream Raitchi 0.0.2 on an AYN Thor; they have
  not been re-measured for AC-NativeNX.

## Bring your own game and keys

Nothing from any game is included or will ever be accepted in this repository,
its CI artifacts or its releases: no games, keys, firmware, saves or shader dumps.
You supply, from hardware you own:

- your `prod.keys` (and `title.keys` when needed), selected in the setup screen;
- your game as an original NSP/XCI package or a prepared folder.

Updates, DLC and unusual package layouts remain limited.

## Supported GPUs

| GPU family | Status |
|---|---|
| Qualcomm Adreno (Snapdragon), with the bundled/selected Turnip driver | Target platform; the only GPU family upstream tested |
| ARM Mali (e.g. Exynos/Tensor/Dimensity phones, Galaxy Fold models with Mali) | **Unsupported**; the app may start but games are not expected to run |
| Samsung Xclipse, PowerVR, others | Untested / unsupported |

Android 11 (API 30) or newer, ARM64 only.

## Installing next to Raitchi

AC-NativeNX uses its own application ID, `com.accreation.nativenx`. Upstream
Raitchi uses `dev.igawa6.raitchi`, so both apps install side by side on the same
phone and do not share data. **Saves, settings, shader caches and imported games
are not migrated** from a Raitchi install; copy your own saves manually if you
need them.

## Downloads

- Every pull request and push to `main` builds an arm64-v8a APK and the matching
  GPL source archive in GitHub Actions (workflow `build`; artifacts
  `AC-NativeNX-<version>-arm64-v8a.apk` and `AC-NativeNX-<version>-source.tar.gz`).
  CI APKs are signed with a throw-away per-run test key: they install, but they
  cannot be updated in place by a later build.
- Signed releases (`release` workflow, tag `v*`) also ship `SHA256SUMS` and the
  corresponding source archive. They are gated by a maintainer approval after a
  real-device test.

## Booted games (upstream Raitchi 0.0.2 observations)

The table records bounded launch observations on AYN Thor with the selected
Balemuni Turnip driver and the candidate56 runtime. The current sweep sends no
gameplay inputs. Later candidate57 checks are targeted regressions, not a fresh
58-game sweep. This table includes only games with a verified visible boot screen,
notice, menu, intro or gameplay scene. **Booting is not a gameplay pass.** Earlier manual/saved-game evidence is shown separately. “Playable” below
means only the previously tested initial scenes, not complete-title compatibility.

**11 games shown below.** See the [complete testing report](TESTING.md) for all 58 entries, including failures and blocked launches, and the regression checks.

| Game | Current launch check | Earlier gameplay evidence |
|---|---|---|
| The Legend of Zelda: Echoes of Wisdom | Main screen | Playable in earlier tested initial scenes; incomplete compatibility |
| The Legend of Zelda: Link's Awakening | Intro/demo | Playable in earlier tested initial scenes; incomplete compatibility |
| Super Mario Bros. Wonder | Main screen | Playable in earlier tested initial scenes; incomplete compatibility |
| Pokémon: Let's Go, Eevee! | Controller selection | Playable in earlier tested initial scenes; incomplete compatibility |
| 13 Sentinels: Aegis Rim | Main screen | No verified gameplay |
| Diablo III: Eternal Collection | Startup notice/logo | No verified gameplay |
| Fire Emblem: Three Houses | Intro/demo | Prior class-demo/battle evidence; not a playable-title qualification |
| Guacamelee! 2 | Startup notice/logo | No verified gameplay |
| Kirby and the Forgotten Land | Menu/setup | No verified gameplay |
| ONE PIECE: PIRATE WARRIORS 4 | Menu/setup | No verified gameplay |
| Ys IX: Monstrum Nox | Menu/setup | No verified gameplay |

## Shader preparation and performance

The runtime learns exact shader/pipeline recipes and prepares known combinations
before gameplay. Bundled metadata catalogs cover 1,803 Link's Awakening records
and 475 observed Echoes of Wisdom records. These are observed/imported lists,
not whole-game coverage. First visits to unknown combinations can still stall.
On a bounded Thor test, LAS preparation took about 71 seconds from an empty cache
and about 5 seconds when cached. Preparation is separate from game loading and
video playback. No comparison with another emulator is claimed.

A Fire Emblem shader local-memory optimization improved an earlier class demo
from about 5–6 to 30 fps. It is a shared compiler optimization with specific proof
conditions, not a universal speedup. Missing graphics, audio, services, package
layouts and startup failures remain. See [release notes](engine/docs/release-0.0.1.md).

## Build and contribute

- Build in CI: [.github/workflows/build.yml](.github/workflows/build.yml) runs the
  documented sequence from [engine/docs/BUILDING.md](engine/docs/BUILDING.md) on
  Ubuntu x86-64 with pinned JDK 17, Android SDK platform 36, build-tools 36.0.0,
  CMake 3.31.6 and NDK r27c (`27.2.12479018`).
- Local build: [engine/BUILDING.md](engine/BUILDING.md) (Linux x86-64).
- After editing anything under `engine/` or `shell/`, run
  `python3 tools/refresh_source_manifests.py` and `python3 -m unittest discover -s tests`.
- [Architecture](engine/docs/ARCHITECTURE.md) ·
  [Compatibility reporting](engine/docs/COMPATIBILITY.md) ·
  [Contributing](engine/CONTRIBUTING.md) · [Licenses and attribution](NOTICE.md)

## Credits

AC-NativeNX is based on Raitchi by igawa6 (GPL-3.0-or-later), which builds on the work of upstream authors and contributors. Their copyright,
SPDX headers and license texts remain in adapted files and accompanying notices.
Pinned revisions are recorded in [sources.lock.json](engine/sources.lock.json),
[echoes-source.lock.json](engine/echoes-source.lock.json) and [shell.lock.json](engine/shell.lock.json).

- [Raitchi](https://github.com/igawa6/Raitchi), by igawa6: the engine, the shared
  runtime and the Android shell integration this fork starts from.
- [Eden](https://git.eden-emu.dev/eden-emu/eden) and its
  yuzu lineage: Android frontend and
  attributed runtime helpers; GPL-3.0-or-later components retain their notices.
- [LASRecomp](https://github.com/peiikai/LASRecomp), by Peiikai and contributors:
  original recomp/runtime lineage.
- [LASRecomp-Android-Plus](https://github.com/Nycolazs/LASRecomp-Android-Plus):
  Android baseline and upstream LAS shader preparation records.
- [Let's Go Recomp](https://github.com/raulzrrs/letsgo-recomp): second-title runtime
  adaptations and references.
- [EoWRecomp](https://github.com/Nycolazs/EoWRecomp): Echoes startup, service and
  graphics references, with LAS lineage credited in the upstream project.
- [BOTWRecompAndroid](https://github.com/mamcjr/BOTWRecompAndroid): architecture
  reference reviewed during development; no claim that its game support or
  entire implementation is included here.
- [SDL](https://github.com/libsdl-org/SDL), [FFmpeg](https://ffmpeg.org/),
  [shaderc](https://github.com/google/shaderc), [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers),
  [volk](https://github.com/zeux/volk), [VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator),
  [AdrenoTools](https://github.com/bylaws/libadrenotools),
  [LinkerNSBypass](https://github.com/bylaws/liblinkernsbypass),
  [Capstone](https://github.com/capstone-engine/capstone), Dear ImGui and other
  dependencies identified in [NOTICE.md](NOTICE.md).
- Fredoka font authors and the attributed button-glyph generation work; notices
  accompany the font assets. Optional diagnostic dependencies have separate
  build options and licenses and are not implied to be default components.

Nintendo and game trademarks belong to their owners. AC-NativeNX is not affiliated
with or endorsed by Nintendo, igawa6/Raitchi, Eden, yuzu, or the upstream recomp projects.
