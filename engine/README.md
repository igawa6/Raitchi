# Raitchi 0.0.1

Raitchi is an experimental Android launcher and shared runtime for original
Nintendo Switch ARM64 games. Continuous native execution (NCE) runs the original
CPU code; Horizon services and NVN-to-Vulkan graphics are implemented by a shared
runtime. A separate Android frontend is derived from Eden. No per-game generated
C is required for the default NCE path.

This is early compatibility work, not a claim that every game is playable. Games,
keys, firmware, saves and original shader code are not included. Supply your own
compatible game package and required keys. Import supports original package files
and prepared folders; updates, DLC and unsupported package layouts remain limited.

## Booted games

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
layouts and startup failures remain. See [release notes](docs/release-0.0.1.md).

## Build and contribute

- [Build instructions](BUILDING.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Compatibility reporting](docs/COMPATIBILITY.md)
- [Contributing](CONTRIBUTING.md)
- [Licenses and attribution](NOTICE.md)

## Credits

Raitchi builds on the work of upstream authors and contributors. Their copyright,
SPDX headers and license texts remain in adapted files and accompanying notices.
Pinned revisions are recorded in [sources.lock.json](sources.lock.json),
[echoes-source.lock.json](echoes-source.lock.json) and [shell.lock.json](shell.lock.json).

- [Eden](https://git.eden-emu.dev/eden-emu/eden) and its
  its yuzu lineage: Android frontend and
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

Nintendo and game trademarks belong to their owners. Raitchi is not affiliated
with or endorsed by Nintendo, Eden, or the upstream recomp projects.
