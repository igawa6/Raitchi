# Licenses and attribution

Raitchi combines a shared native runtime with an Eden-derived Android shell.
The default engine includes GPL-3.0-or-later components; it also retains MIT and
other third-party components under their existing notices. [LICENSE](LICENSE)
maps these notices and [COPYING](COPYING) contains the unmodified GPLv3 text.
Original copyright and SPDX headers remain in the adapted source files.

## Source provenance

| Component | Source revision | Scope and retained notice |
| --- | --- | --- |
| LAS Recomp Android / LAS lineage | `cca791a3e7218e1dac95e7d4a11b8f97dba3ea51`; upstream reference `59b61fd3c65dcabd0feb62d5d5706240cb3f5bb9` | Shared runtime lineage; MIT, Peiikai. [Notice](native/runtime/LICENSE-LASRecomp), [source lock](sources.lock.json). |
| Let's Go Recomp | `96894fd8e9ea20c1b38cdea53105ec2a335df620` | Runtime adaptations; MIT, raulzito12345rrs and Peiikai portions. [Notice](native/runtime/LICENSE-LetsGo), [source lock](sources.lock.json). |
| EoWRecomp | `dd9c79d6c01d9eaddafc1fc5a07b6658bfc56505` | Echoes startup/services/graphics references and adapters; MIT, Peiikai and EoWRecomp contributors. [Echoes notice](native/runtime/LICENSE-Echoes), [EoW notice](native/runtime/LICENSE-EoWRecomp), [adapter notice](native/adapters/echoes/LICENSE), [source lock](echoes-source.lock.json). |
| Eden/yuzu runtime helpers | Eden `c6541721fa6307e99b3fdd9517dd4ed1b73927ed` | Logical guest affinity and audio-renderer work-size accounting; GPL-3.0-or-later adaptations. Source headers retain Eden/yuzu copyrights and source paths. [Affinity license](ports/shared-android/runtime/LICENSE-kernel-thread-affinity.txt), [audio license](ports/shared-android/runtime/services/LICENSE-audren-work-size.txt). |
| Eden Android shell | Upstream `989768aa46720e3fcab9e2111e3ddd70819c382b` | Separate GPL-3.0-or-later frontend; retains its own `LICENSE.txt`, per-file notices and upstream history. [Shell lock](shell.lock.json) records the actual upstream base and hashed uncommitted source overlay. The distribution URL is igawa6/Raitchi; no published integration commit is asserted. |
| AdrenoTools and LinkerNSBypass | Dependency inventory produced by the Android build | BSD 2-Clause, Billy Laws. [AdrenoTools notice](ports/letsgo-android/LICENSE-AdrenoTools), [LinkerNSBypass notice](ports/letsgo-android/LICENSE-LinkerNSBypass). |
| Fredoka / attributed button glyphs | Bundled font assets | [Fredoka SIL OFL 1.1](ports/shared-android/assets/fonts/OFL-Fredoka.txt); [button-glyph MIT notice](ports/shared-android/assets/fonts/LICENSE-ButtonGlyphs.txt) and [font provenance](ports/shared-android/assets/fonts/README.txt). |

## Community fixes and compatibility references

[ptichalouf](https://github.com/ptichalouf) contributed the native-manifest
publication, 16 KiB SDL linkage, ARM host-probe guard and keyboard fixes in
[PRs #2–#8](https://github.com/igawa6/Raitchi/pulls), plus the contributor-guide
proposal. Integrated changes include additional bounds, packaging and Unicode
editing checks.

[nx2android](https://github.com/shirai91/nx2android), reviewed at
`9d67edec2175ae5f4beadbd65f37eaa88105d132`, informed the SDK keyboard encoding-mode
compatibility review. This is an implementation reference, not a claim that its
runtime or game profiles are incorporated wholesale.

## Packaged dependencies and notices

The Android engine build uses SDL3, FFmpeg, shaderc and its dependencies, Vulkan
headers, volk, VulkanMemoryAllocator, Capstone, AdrenoTools and LinkerNSBypass.
The runtime UI also retains its upstream Dear ImGui integration. Their source
notices and dependency build records apply; the engine's GPL text does not
replace those licenses. Exact dependency versions, archive identities and build
options come from the build manifests used for the artifact.

[The asset packager](tools/package_shared_android.py) copies LAS, Let's Go,
Echoes, Fredoka and button-glyph notices, plus the Baloo2 and DejaVu notices
alongside those fonts. [The shell builder](tools/build_eden_shell.py) verifies
that the shell's `assets/GPL-3.0-or-later.txt` is present. Preserve these assets
and the shell's license viewer when repackaging. Additional linked dependencies
must retain their own notices; this list does not assert that an uninspected APK
contains every required notice or source file.

## Optional configurations

| Configuration | Default release | Notice scope |
| --- | --- | --- |
| Continuous NCE runtime | Enabled | Default shared engine and shell notices above. |
| Unicorn native-blocks diagnostic backend | Disabled (`RAITCHI_NATIVE_BLOCKS=OFF`; no `--with-native-blocks`) | Optional Unicorn dependency uses its own GPLv2 `COPYING`; disabling it does not remove the default engine's GPL3 helpers. |
| Legacy on-device compiler toolkit | Omitted (no `--with-ondevice`) | When included, preserve the toolkit's `licenses/` directory and `components.json`, including Unicorn, Python, Capstone, LLVM, Zig, musl, lz4 and Android toolchain notices as applicable. |

These options have separate dependency inventories. No compatibility declaration
for all optional combinations is implied by the default configuration.

Game files, game assets, keys and generated game code are not licensed by this
repository. Nintendo and game trademarks belong to their respective owners.
Raitchi is not affiliated with or endorsed by Nintendo, Eden or the upstream
recomp projects.
