# Contributor backlog

This records the proposals in [issue 1](https://github.com/igawa6/Raitchi/issues/1)
and the guide from [PR 3](https://github.com/igawa6/Raitchi/pull/3), reviewed on
2026-10-29. These milestone IDs are discussion labels, not issue numbers,
deadlines, reserved work or promises of support. Existing foundations do not
qualify an entire feature or game. Check current issues and receipts before
starting; the older reports linked below retain their original scope/date.

The public repository has `engine/` and `shell/`. Engine documentation paths
below are relative to `engine/`; a development engine checkout has these files
at its root. The shell is Eden-derived and its Android app is under
`shell/src/android/`. Follow the actual [source overlay](SOURCE-OVERLAY.md) and
pinned build inputs rather than creating a second engine implementation in the
shell.

| Proposal | Existing foundation / current evidence | Remaining bounded contribution and acceptance |
|---|---|---|
| 0a Native preparation publication | The no-replace publication fix and [synthetic publication regression](../tools/check_native_publish.py) are integrated; Android phone confirmation remains separate. | Compare any reported Android failure with current code first. Reproduce races/conflicts and preserve existing generations/saves on failure; device confirmation remains separate. |
| 0b Keyboard results | Flag-selected UTF-8/UTF-16 output, bounded guest spans and Unicode editing are covered by [keyboard tests](../tools/check_swkbd.py). Device name entry is a separate qualification. | Isolate encoding and length handling with ASCII/accented round-trip, cancel and repeated-entry tests; then verify on the reported device. |
| 0c Complete 16 KB compatibility | SDL and runtime link settings request 16 KB alignment; [ELF/APK checks](../tools/check_elf16k.py) cover the final packaged library set and ZIP layout. A 16 KB device is still required for device qualification. | Audit every packaged ELF, offset/address congruence and APK ZIP alignment, including dependencies; verify a 16 KB device. Existing flags alone do not establish complete compatibility. |
| 1 Reproducible ARM checks / CI | [Host runner](../tools/run_host_checks.py) emits commands, status and timings. Issue 1 reports historical ARM runs and two additional continuous-probe failures. | Reproduce at exact revisions and distinguish missing environment dependencies from runtime defects; run untrusted PR checks without secrets or private assets. Do not treat the historical run as current certification. |
| 2 Contribution documentation | [Contribution guide](../CONTRIBUTING.md), [assistant guide](AI-CONTRIBUTING.md) and this backlog exist. | Verify onboarding on a fresh checkout; add a small synthetic example or issue/PR templates where useful. Public guidance does not require distributing internal workspace instructions. |
| 3 Machine-readable evidence | Host JSON receipts and [compatibility schema](../compatibility/schema.json) already distinguish test results and scoped game observations. | Extend only missing fields: revision, command/exit status, timing, before/after and untested conditions; preserve validation and redaction. |
| 4 Isolated beginner environment | [BUILDING](BUILDING.md) documents host and Android dependencies; a small host-test path needs no Android build. | Validate one supported clean-machine setup with a newcomer and report skipped dependencies. No universal setup script is claimed. |
| 5 Task coordination | Issue comments and draft PRs provide manual coordination. | Optional duplicate/dependency tracking with clear stale-claim handling; no central credentials or automation prerequisite. Independent bounded tasks may proceed in parallel. |
| 6a Mods engine | Asset override primitives exist; [filesystem services](../ports/shared-android/runtime/services/fs.c) are an investigation starting point. RomFS overrides, executable patches and cheats require separate contracts. | Define deterministic ordered overlays, added/replaced files and path/archive safety across file and raw-storage reads; do not infer general mods from title-specific overrides. |
| 6b Mods UI | The Eden-derived shell supplies UI patterns; a visible toggle does not implement engine loading. | Depend on a real 6a capability contract; show priorities/conflicts and unsupported types, and keep original game files untouched. |
| 7 Cheats | Continuous NCE is an execution path, not a general cheat interpreter. | Investigate exact title/build matching, bounded writes and reversible activation; synthetic tests and save-safety validation before advertising a format. |
| 8 Optional RomM | No general integration is claimed. | Separate opt-in authentication, resume/integrity and import work; no bundled content/keys or assumed DLC/update support. |
| 9 Controllers and touch | Shared controller paths and Eden-derived configuration exist; [settings check](../tools/check_raitchi_settings.py) covers selected settings behavior. | Inventory existing mappings before adding missing dead zones or layout editing. Test physical input, touch and controller-selection regressions separately. |
| 10 Foldables / lifecycle | Android shell and fresh game processes already exist. Rotation/fold/resume correctness is device-specific. | Reproduce surface/input recovery and background behavior, then measure it. A second-screen layout is an optional separate feature. |
| 11 Performance profiles | Resolution/driver/per-title settings and shader policies exist. | Validate units/default restore and frame-time measurement; distinguish displayed FPS from game speed. Do not add quality reductions, stale data or timing changes as an unexplained performance fix. |
| 12 Save backup / recovery | Title save namespaces and preservation checks exist; that is not a general backup/restore feature qualification. | Inventory shell export/import first; verify format/version constraints, explicit overwrite behavior and an actual restore. No automatic upload or cross-title assumptions. |
| 13 Shader preparation / caches | [Preparation](shader-preparation.md), [cache identity](shader-cache-identity.md), discovery and profile checks already exist. | Improve progress, exact content/driver invalidation and first-use coverage without proprietary shader distribution. Observed recipes are not a complete whole-game shader set. |
| 14 Diagnostics / compatibility | [Compatibility guide](COMPATIBILITY.md), schema and validators already separate bounded states; diagnostics default off. | Export only minimal redacted metadata with tested content/version, driver, scene, timing and first failure. Boot/menu evidence never becomes whole-game support. |

Reliability and reproducibility come first. Documentation, synthetic tests and
independent service investigations can run in parallel when they do not rewrite
the same hot paths. Feature UI depends on the corresponding engine contract;
mods and cheat handling are distinct from installed update/DLC support.

Update/DLC installation, mods and cheats are separate capability contracts.
This public-baseline backlog does not assert that a content registry, BKTR
update mounting or a general DLC/mod provider is already available. A future
implementation must validate selected-content identity, executable/assets
consistency and a bounded device route for each claimed combination.
