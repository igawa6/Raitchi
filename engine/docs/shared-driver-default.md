# Shared launcher graphics-driver selection

The shared test app uses one app-wide graphics-driver selection. Importing a
driver through **Install graphics driver** installs and enables it for subsequent
fresh game processes. Starting LAS or Eevee does not change that preference.
An explicit stock-driver choice remains disabled; LAS requires an enabled,
readable custom driver for its tested menu-text path. Eevee retains
`LARECOMP_VK_ACQUIRE_FENCE=0` independently of driver choice.

`tools/stage_shared_manual.py` also preserves this preference and `.probe` for
both titles. Its existing LAS-only seeding of an empty driver directory remains
available, but copying those files does not enable them. The staging receipt
records installation separately from the preserved selection.

The owner's requested default is MrPurple T30. The coordinator checked the
[upstream latest release](https://github.com/MrPurple666/purple-turnip/releases/tag/vturnip_mrpurple_T30-toasted.adpkg)
on 2026-10-25 and verified the downloaded asset against GitHub's SHA256 digest.
The release was published on 2026-08-17. This pins that check, rather than claiming
the release will remain the latest.

| Item | Identity |
| --- | --- |
| [Download](https://github.com/MrPurple666/purple-turnip/releases/download/vturnip_mrpurple_T30-toasted.adpkg/turnip_mrpurple_T30-toasted.adpkg.zip) | `turnip_mrpurple_T30-toasted.adpkg.zip` |
| Archive SHA256 | `f65b2d3353fd4aa7190bb5426b94468e99ffea7a58a830bc0c4651db89353227` |
| Library | `vulkan.purple.so` |
| Library SHA256 | `1d80dfa019659b008e4669311db5b1e4a02af59ff1d5458a98e2f5fe18ed013b` |
| Metadata driver version | `26.3.0-T30-1.4.359` |
| Minimum Android API | 30 |

The ZIP is a driver package, not an Android APK. Import enables this default
through the existing `GpuDrivers` preference; no driver is downloaded or bundled
automatically. It affects only `com.recomp.shared.test`, never Android's system
driver, another emulator, or another app's driver settings. The common native
runtime remains unchanged by this launcher policy.

Before replacing the shared app's existing driver, preserve its complete
`files/gpu_driver/current` directory and `shared_prefs/gpu_driver.xml` while its
game process is stopped. Roll back by restoring both together, or explicitly
select stock. Preserve other applications and the owner's profiles. These are
coordinator operations; do not interrupt an active manual session.

`GpuDrivers.config()` creates a `.probe` marker before driver startup. The native
renderer removes it after 60 successful presents. If a selected driver's marker
survives startup, the next fresh process disables that selection and tries stock.
Launching a title must not call `GpuDrivers.select()` automatically: that method
deletes the marker and would erase failure evidence. An explicit import or user
selection starts a new attempt. LAS can require the owner to enable a driver
again after a failed attempt; it does not silently reset the preference.

Installation and enabled preferences do not establish successful rendering.
Verify both the custom-library path in the startup log and the subsequent
Vulkan driver name/version before reporting a MrPurple run. Fresh-process LAS
and Eevee checks with this pinned release remain coordinator-owned.


Thor verification completed on 2026-10-25 with the actual T30 library mapped by
both games (`PurpleVK public driver`, 26.3.0-devel, git 62ac221a33, Vulkan 1.4.359).
The launcher started Eevee through controller confirmation/title/movie at 30 fps,
then LAS loaded cloned File2 into rendered Mabe Village at 30 fps. Both sessions
used C-AOT and the identical `dfcba7b7ac0c` native runtime; this was not direct
native performance. Selection stayed enabled across title switches and no
stock-driver fallback was observed. Eevee save gameplay was not qualified in
this driver check. Original save/settings directories were preserved, tests used
copies, then originals were restored with content hashes checked. The previous
custom driver remains in an app-private backup directory. Local evidence is
`evidence/mrpurple-current.json`; assets and device evidence remain ignored.
