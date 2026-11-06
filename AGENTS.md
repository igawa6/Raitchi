# AGENTS.md - instructions for coding agents

AC-NativeNX is an experimental Android runtime that runs your own Nintendo Switch
games natively (Eden-derived frontend, shared NVN-to-Vulkan runtime). It is based on
[Raitchi](https://github.com/igawa6/Raitchi) by igawa6. This file is for coding
agents (Claude Code, Hermes Agent, Codex, Cursor, Copilot, Aider...) **and for the
people who drive them**. Most agents read `AGENTS.md` natively; if yours does not,
paste it into your prompt.

Humans: start with [`CONTRIBUTING.md`](CONTRIBUTING.md).

## Red lines (never, no exception)

Never put any of this into the repo, a PR, an issue, a comment, a log or a CI artifact:

- games or packages: `.nsp .xci .nsz .xcz .nca .tik`, extracted `romfs/` or `exefs/`
- keys: `prod.keys`, `title.keys`, any `*.keys`
- firmware, system fonts (`.bfttf`), saves, shaders dumped from a game
- anything generated from a game, or any file bigger than 50 MB

Users bring their own game and their own keys. If a task needs a game file, it cannot
be done in this repo: say so instead of working around it. `tools/check_no_game_content.py`
enforces this in CI (`.github/workflows/guard.yml`); do not weaken it.

Also:

- **No PR, issue or push to upstreams** (`igawa6/Raitchi`, Eden, yuzu). This is an autonomous fork.
- **GPL-3.0-or-later**: keep `COPYING`, `NOTICE.md` and every copyright line. Credit igawa6, yuzu
  and Eden. Do not paste code from incompatible licenses.
- **Do not merge your own PR.** Open it as a draft; a different identity reviews it.
- **Never claim "tested on device"** unless a human or a named device tester gave you a
  device report (device, SoC/GPU, driver, build SHA). Automated tests are not device tests.
- **No secrets** in commits, logs or PRs.

## AI disclosure (required)

AI work is disclosed, never disguised. Every commit and PR written by an agent says who:

```
Agent: <agent name> · Model: <model id>
```

Put that line in the PR description (the template has a field) and as the last line of
the commit message body. Example: `Agent: Hermes · Model: claude-sonnet-5-5`.

## Tools to install

Check what you have (installs nothing):

```sh
tools/setup.sh --check
```

It prints one line per tool: `OK <tool>`, `MISSING <tool> -- <how to install>` or
`WRONG-VERSION <tool>`. Exit 0 means the toolchain is complete. Without `--check` it
installs the Android SDK/NDK pieces through `sdkmanager` (needs
`--accept-android-licenses`); system tools are never installed silently.

| Tool | Version | Needed for |
|---|---|---|
| Python | 3.11+ | tests, tools, everything |
| git | recent | everything |
| JDK | 17 | Android build, Java host checks |
| Android SDK | platform `android-36`, build-tools `36.0.0`, CMake `3.31.6` | Android build |
| Android NDK | r27c (`27.2.12479018`) | native build |
| CMake + Ninja | recent | native build |
| clang, glslangValidator, spirv-val | any recent | optional host checks (skipped, not passed, if absent) |

Linux and macOS are supported, Windows through **WSL2** only (`tools/setup.ps1` explains).
You do **not** need the build toolchain to report an issue or to measure performance.

## Skills to install for your agent

The folder [`skills/`](skills/) holds task skills in the standard `SKILL.md` format
(frontmatter `name` and `description`):

| Skill | Use it to |
|---|---|
| `contribute-nativenx` | go from idea to a reviewable PR |
| `report-device-issue` | file a reproducible bug (no build tools needed) |
| `perf-report` | produce a comparable performance report (no build tools needed) |

Install them:

- **Claude Code**: copy them into the project (`mkdir -p .claude && cp -r skills .claude/skills`)
  or into your user folder (`cp -r skills/* ~/.claude/skills/`).
- **Hermes Agent**: nothing to copy if you work inside this repo. Hermes loads project skills from
  `.agents/skills/` or `.hermes/skills/`: `mkdir -p .agents && cp -r skills .agents/skills`, then run
  `hermes skills trust` once in the repo. To use them everywhere, add `<path-to-repo>/skills` to
  `skills.external_dirs` in `~/.hermes/config.yaml`.
- **Other agents** (Codex, Cursor...): point the agent at `skills/<name>/SKILL.md`.

## Map of the repository

| Path | What it is |
|---|---|
| `engine/` | Shared runtime and tooling (ex-Raitchi). `engine/native/` native execution (NCE, module loader), `engine/ports/shared-android/` Android runtime + APK sources, `engine/tools/` build and check scripts, `engine/tests/` C tests, `engine/docs/` design notes, `engine/compatibility/` per-game reports |
| `shell/` | Android frontend derived from Eden (`shell/src/android`, `shell/src/video_core`, `shell/src/core`...), built with CMake/Gradle |
| `sources.lock.json`, `shell.lock.json` | pinned upstream references |
| `TESTING.md` | bounded launch results per game: read it before claiming anything about a game |
| `skills/`, `docs/ai-kit/` | the AI kit (shared with the sibling project, see `docs/ai-kit/README.md`) |
| `tools/` | repo-level guards and setup (`setup.sh`, `check_no_game_content.py`) |
| `tests/` | repo-level tests (kit, branding) |
| `RELEASE.json`, `NOTICE.md`, `COPYING` | version / applicationId, credits, GPL text: keep them intact |

Renderer hot spots (`nvn/maxwell/translate.c`, `nvn/vulkan/exec.c`, `nvn/cmdbuf.c`, the
texture cache) change often. Comment on the tracking issue before a larger change there.

## Commands

Run from the repo root. Report the exact command and its real exit status in your PR.

```sh
# Kit tests (no toolchain needed)
python3 -m unittest discover -s tests -v
bash tests/test_setup_check.sh
python3 tools/check_no_game_content.py .

# Engine host checks (compile synthetic C tests; missing tools are "skipped", never "passed")
(cd engine && python3 tools/run_host_checks.py && python3 tools/check_compatibility.py)

# Source manifests must stay current: run after touching any shell overlay file
python3 tools/refresh_source_manifests.py          # --check to verify only

# Android build (exactly what CI replays, from engine/; see engine/docs/BUILDING.md)
cd engine
python3 tools/fetch_sources.py && python3 tools/verify_sources.py
python3 tools/build_las_baseline.py --deps --jobs 4
python3 tools/build_las_baseline.py --apk --jobs 4
python3 tools/build_letsgo_ffmpeg.py --build --jobs 4
python3 tools/prepare_shared_ondevice.py --fetch --build --jobs 2
python3 tools/build_shared_android.py --build --jobs 4
```

The engine scripts expect to run from `engine/`; `engine/docs/BUILDING.md` is authoritative. CI
(`.github/workflows/build.yml`) builds on Ubuntu 24.04 with JDK 17, SDK platform `android-36`,
build-tools `36.0.0`, CMake `3.31.6`, NDK r27c, and publishes `AC-NativeNX-<ver>-arm64-v8a.apk`,
`AC-NativeNX-<ver>-source.tar.gz` and `SHA256SUMS` (current version in `RELEASE.json`, applicationId
`com.accreation.nativenx`). `release.yml` only creates a **draft** release and refuses to run unless the
repo variable `NATIVENX_RELEASE_APPROVED=true`; agents never publish a release.

## Rules for a good PR

1. One bounded problem per PR, with a link to the issue. Split refactors from behavior changes.
2. Add a **synthetic** test whenever practical (small libraries, dummy files, never game data).
3. Paste real output. A missing tool is a *skip* or a *failure*, never a success. Write
   "not executed" for what you did not run.
4. State the limits: what you did not verify (other GPU, other game version, device).
5. Displayed FPS is not game speed. A change that lowers quality or adds a wait to improve a
   number is not a performance fix.
6. Conventional Commits in English: `feat(shell): ...`, `fix(engine): ...`, `docs: ...`.
7. LF line endings except `.bat` / `.ps1`.

## Supported hardware (set expectations)

Adreno GPUs (Snapdragon) are the target. Mali and other GPUs are **unsupported**: the app should say
so (missing Vulkan features, unsupported GPU, missing keys) instead of crashing. If you see a crash
on such a device, that is a bug worth reporting with `report-device-issue`.

The old Raitchi app (`dev.igawa6.raitchi`) can stay installed next to AC-NativeNX
(`com.accreation.nativenx`); saves are not migrated.
