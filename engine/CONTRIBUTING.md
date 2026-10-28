# Contributing to Raitchi

Raitchi is one shared runtime with small per-title adapters. Contributions
should improve that shared runtime. Please do not fork the runtime per game.

## Kinds of work

| Work | What you deliver | Where |
|---|---|---|
| Game investigation | A **Game investigation** issue: game version, first failure, diagnostics, references compared | Issue |
| Shared runtime fix | A focused PR to services, kernel, NVN, shader translation or packages, with a synthetic test | PR |
| Title adapter | Adapter table plus adapter test; only identifiers you measured from your own copy | PR |
| Device QA | A **Compatibility report** issue or a `compatibility/reports/*.json` PR for a specific build | Issue or PR |
| Android app | Shell UI changes go to the shell repository (pinned in `shell.lock.json`) | Shell PR |

Keep investigation, implementation and device QA separate. A game
investigation does not need to come with a fix, and a QA report does not need
to explain the cause.

## Before you start

1. Search the issues. One open issue per game version; claim it by commenting.
2. Shared renderer areas (`nvn/maxwell/translate.c`, `nvn/vulkan/exec.c`,
   `nvn/cmdbuf.c`, texture cache) change often. Comment on the tracking issue
   before a larger change there, so two people do not rewrite the same code.
3. One bounded problem per PR. Split refactors from behavior changes.

## Pull request requirements

- **Trigger**: game, version, scene and steps that reproduce the problem, or the
  API contract the change implements.
- **Shared or title-specific**: say which. Title-specific behavior needs an
  adapter policy flag and the evidence that it really differs per title.
- **Provenance**: name any reference used (project, commit, file) and its
  license. Only license-compatible sources may be adapted, with attribution kept.
- **Tests**: a synthetic host test that fails before and passes after, where
  practical. Tests and CI never use game files, keys or firmware.
- **Results**: before/after observations, regressions checked, and an explicit
  list of what you could not test (for example: no device, one game only).
- Run the host checks: see [building](docs/BUILDING.md#host-checks).

Maintainers can only review evidence they can see. Describe private results
(logs, captures) in words or with redacted excerpts. Never attach game data,
keys, saves, extracted assets or shaders, or logs containing private paths or
device serial numbers.

## Code style

Match the surrounding C: small functions, explicit bounds checks, no silent
fallbacks. An unimplemented call should fail with a diagnostic instead of
returning fake success. Diagnostics stay off by default and print metadata,
never game content. User-visible text says "Raitchi"; upstream project names
belong only on the licenses screen (`tools/check_frontend_branding.py`).

## Licensing

By contributing you confirm that you have the right to submit the change under
the repository's license (see `LICENSE` and `NOTICE.md`), and that it contains no
code copied from incompatibly licensed work, games or SDKs.
