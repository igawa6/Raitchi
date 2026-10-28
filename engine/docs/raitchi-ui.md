# Raitchi Android frontend

Raitchi keeps the existing `com.recomp.shared.test` package and each game's
private game, compilation and save directories. This is a launcher change;
recompilation, the common runtime and game compatibility remain separate.

The library provides grid and list views, remembers the chosen layout, and shows
the supported titles: Link's Awakening, Let's Go Eevee and Echoes of Wisdom. Each card opens game
details with current readiness, preparation progress, Play/Resume game,
import and per-game settings. The older LAS/Eevee compiled-code path also offers
Prepare/Resume through the foreground `SharedBuildService`; cancellation keeps
completed compiler work. Direct package preparation does not compile game C.
The library process remains separate from the game and compiler processes.
A running session can be resumed or explicitly ended after a save reminder.
Ending writes a request bound to that game PID and resumes a paused SDL window
so it can drain and join its threads. If it does not finish, the launcher offers
an explicit Force close confirmation after ten seconds.

The frontend follows Android's system light/dark setting, including Thor's
selected system theme. Pink/red accents, rounded cards, native focusable buttons
and the owner's Raitchi logo form the shared presentation. Graphics-driver
selection stays app-wide and belongs to Settings; switching library cards does
not change drivers. Global and per-game settings use the companion
`SettingsActivity` and validated `SettingsStore` implementation.

Global defaults include CPU execution, language, resolution, upscaling,
anti-aliasing, audio and controller preferences. LAS-only frame-rate/aspect/blur
options are labelled accordingly. A per-game value can be reset to inheritance;
unknown existing INI entries and controller mappings are retained. Continuous NCE
is the requested default and uses its separate save/settings namespace. AOT
remains selectable for LAS/Eevee; Echoes uses NCE. Native compatibility testing
continues. Supported package files use the native preparation path below.

Navigation was reviewed against the local Eden Android frontend at commit
`c6541721fa6307e99b3fdd9517dd4ed1b73927ed`: library layout selection, game properties
and settings entry points. Raitchi uses independently written Android platform
widgets; it does not copy Eden's Kotlin/Material implementation or include an
Eden emulator. Cover placeholders are title initials rather than bundled game
artwork. The root coordinator supplies the logo/icon assets.

## Import paths

- **Game file:** choose **Add game → title → Game file**, select an uncompressed
  base **NSP or XCI**, then the owner's `prod.keys`. Native preparation validates
  the original modules and publishes a separate app-private ExeFS/manifest
  generation. The runtime reads RomFS from the retained package; no full resource
  tree or generated game C is written. Keys are retained privately for later
  package reads. Keep Raitchi open until ready, then open the card and press
  **Play**. See [direct package loading](direct-nsp-native-loading.md) for the
  supported build, ticket, document-access and validation limits.
- **Extracted folder (LAS/Eevee):** the existing document-tree import copies only top-level
  `exefs`, `romfs` and `romfs.bin`, retaining hash comparisons and refusing
  conflicting existing files. Saves, keys and build folders are not copied.

The retained `RaitchiUiPackage` Java full-extraction helper and its empty-destination
rule belong to the legacy implementation; the current **Game file** action uses
native package preparation. Native generations preserve existing extracted
games, compiled libraries and saves. NSZ/XCZ is unsupported. Import workers are
not foreground services; keep the app open until preparation finishes.

## Validation

All actual overlay Java classes compiled together with Java 17 against the cached
Android 36 API and existing launcher dependencies, including the real settings
classes. Historical legacy-extractor validation used a host harness against
synthetic AES-XTS-encrypted NSP headers: **17 checks passed** for both title
selections, wrong-title rejection, update-only rejection, compressed-package
rejection and key parsing. These fixtures contain no game data and do not prove
complete extraction of an actual game. Private check sources, logs and hashes
are recorded under `build/raitchi-ui-check`.

Current native-package tests and device evidence are recorded in
[direct package loading](direct-nsp-native-loading.md). Java compilation and
synthetic parser tests alone do not qualify Android lifecycle behavior, rendering
or gameplay.
