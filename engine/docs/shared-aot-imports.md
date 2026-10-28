# Complete legacy AOT import inventory

The legacy inventory remains discovery evidence. The subsequent [full Android
integration](shared-android-integration.md) supplies all56 measured runtime names
and verifies the actual new title imports against one Android runtime. The host
comparison below records the earlier foundation checkpoint; it is not the
current production-runtime boundary.

Offline ELF audit, 2026-10-25. `tools/audit_shared_imports.py` examines the **complete dynamic symbol tables and DT_NEEDED lists** of the two existing private title libraries, their baseline runtime exports and the dependencies' exports. No library is executed, rebuilt, regenerated or loaded on a device.

```sh
python3 tools/audit_shared_imports.py
```

The installed NDK LLVM readelf and API30 sysroot suffice; `--ndk` selects another local copy of pinned NDK27.2.12479018. Reports and raw readelf output go to ignored build evidence. Each run records exact library/runtime/dependency hashes, source of legacy ABI/code-ID declarations, symbol type/binding/version, dependency providers, missing dependencies, unclassified imports and optional shared-host comparison. No game identities, original input paths or generated code are included here.

## Measured result

| Existing title library | Runtime imports supplied by its baseline `libmain.so` | Dependency imports | Unclassified | Missing DT_NEEDED |
| --- | --- | --- | --- | --- |
| LAS Android |51 |7 |0 |0 |
| Let's Go |9 |7 |0 |0 |

The runtime-name union contains **56 distinct symbols**. No imported symbol is simultaneously exported by the baseline runtime and a dependency in these references; the tool retains that ambiguity category for future outputs. This is an offline provider inventory, not proof of Android namespace resolution or library semantics.

Both libraries directly need `libmain.so`. LAS also declares `libdl.so` and `libc.so`. Let's Go additionally declares `libandroid.so`, `liblog.so`, `libdl.so`, `libm.so`, `libc.so`. A DT_NEEDED declaration does not itself imply that this library supplies a particular imported symbol: the audit traverses the baseline runtime/dependency graph and checks versioned exports.

All seven dependency imports resolve to the API30 libc declarations: `__cxa_finalize`, `__cxa_atexit`, `__register_atfork`, `getauxval`, `__system_property_get`, `strncmp`, and weak `memfd_create` with its `LIBC_R` version. The others use `LIBC`. These include C-runtime/toolchain support and ordinary platform functions; they do not belong in the shared guest runtime API. The outlined ARM64 atomic helpers seen in compile-only objects are absent from the final libraries' dynamic undefined sets, consistent with their resolution during final linking. Do not add guest callbacks merely because intermediate objects imported those helpers.

## Complete measured runtime union and ownership

Common execution/hook/profiling names, **9 total**:

`rt_dispatch`, `rt_udf`, `rt_ticks`, `rt_svc`, `rt_hook`, `rt_fnhook`, `g_fnhook`, `fnprof_hit`, `g_fnprof`.

Generic HLE CPU-call wrappers, **9 total**:

`hle_bcmp`, `hle_memchr`, `hle_memcmp`, `hle_memcpy`, `hle_memmove`, `hle_memset`, `hle_strcmp`, `hle_strlen`, `hle_strncmp`.

These HLE names are the generated CPU-call boundary, not interchangeable with libc functions bearing similar names. They require the guest register/memory calling convention and core service implementations.

LAS title/patch-facing helpers, **37 total**:

`rando_word`, `rt_actor_create_fn`, `rt_add_item`, `rt_add_prize`, `rt_adv_exit_choice`, `rt_aspect_site`, `rt_bottle1_use`, `rt_bottle3_use`, `rt_buff_replace`, `rt_cull_aspect`, `rt_damage`, `rt_delegate_null`, `rt_dgnmap_end`, `rt_dgnmap_tick`, `rt_enemy_spawn`, `rt_fovy`, `rt_frame_steps`, `rt_game_dynres`, `rt_hero_mode`, `rt_hide_item_explanation`, `rt_hookshot_attr`, `rt_indoor_aspect`, `rt_interval_choice`, `rt_items_page_tick`, `rt_level_request`, `rt_log_resname`, `rt_map_page_end`, `rt_map_page_tick`, `rt_prompt_x`, `rt_skysea_margin`, `rt_snap_dir`, `rt_tilt_obj`, `rt_tunic_upgrades`, `rt_underwater_items`, `rt_underwater_skip_owned`, `rt_walk_speed`, `rt_warp_request_obj`.

Let's Go title/scene callback, **1 name**: `rt_scene_tick`.

The ownership split guides adapter extraction; it does not remove imports from already generated code. A feature can be disabled while its compiled call sites still require the helper symbol. Preserve correct adapter behavior or regenerate against a reviewed producer policy; silent no-op implementations are not compatibility evidence. In particular, the first-chunk probes missed the HLE wrappers, most LAS helpers, Let's Go's scene callback and the old profiling imports. The complete union is a necessary follow-up to those probes.

## Earlier foundation host comparison

The rebuilt `libshared_aot.so` is a **host x86-64** execution seam with nine libc HLE CPU-call wrappers; the broader Android OS/services and renderer remain outside it. The final audit rerun, after the profile-preflight correction, verified SHA-256 `b5fa8be0a880c40288525925289a7e4f5c462cdd0d7fd776ea09aa8b1828619d`. It supplies **15 of the56 union names**: all nine HLE wrappers listed above, plus these six execution/hook/profiling names:

`rt_dispatch`, `rt_hook`, `rt_fnhook`, `g_fnhook`, `fnprof_hit`, `g_fnprof`.

Its **41 expected missing names** are precisely:

- `rt_udf`, `rt_ticks`, `rt_svc`;
- all37 LAS title/patch helpers and Let's Go's `rt_scene_tick`.

This is an expected boundary of the intentionally small core. Integrating the common OS/HLE layer and title adapters must supply the relevant semantics and deliberate exports in one Android runtime artifact. A symbol-name intersection across x86-64 and ARM64 does not establish ABI, machine-code or calling-convention compatibility. The tool records the actual shared-host hash when present; rerun it after that artifact changes. If absent, the comparison is explicitly pending.

## Provenance and acceptance limits

LAS's earlier baseline evidence hashes the **packaged stripped title ELF**. Its larger unstripped link output has different bytes. The utility verifies the saved APK hash, extracts its existing title/runtime ELF bytes into private audit output, and compares the packaged title hash with that evidence. It separately records the unstripped library hash and observed legacy code-ID sidecar. Legacy ABI1 is a baseline source declaration; the getter is never invoked. There is no invented historical per-unit generation manifest.

Let's Go's complete build manifest binds its title-library hash and declares upstream ABI3,1,024-byte CPU and code identity. Its current baseline runtime may have a different hash from the runtime present at the original game-library link, while retaining the same code identity after host-only fixes. Both link-time and current runtime hashes are recorded; current bytes must match the current native-build manifest. Dependency symbols are taken from cached actual libraries or clearly labelled NDK API stub/toolchain ELFs, not claimed as freshly inspected device libraries.

Neither legacy title ELF is a producer of the new common CPU identity3. In particular, **Let's Go's upstream GAMELIB_ABI3 is not shared CPU identity3**. Both retain the old `lasgame_tables` function export; that is not the new validated immutable descriptor boundary. The new header's optional profiling policy can also remove some legacy profiling imports on recompilation, so this union is discovery input rather than an immutable production export specification.

Next acceptance step: rebuild actual title producers against the frozen common contract and repeat the full import/DT_NEEDED/metadata audit against the same Android runtime artifact. Keep platform dependencies distinct, require no unexplained strong imports, and validate file/descriptor/CPU/profile identities before guest entry. Then qualify behavior through both fresh-process gameplay routes. This audit alone establishes none of those execution results.
