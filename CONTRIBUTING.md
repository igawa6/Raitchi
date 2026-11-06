# Contributing to AC-NativeNX

AC-NativeNX is an experimental Android runtime for **your own** Switch games, based on
[Raitchi](https://github.com/igawa6/Raitchi) by igawa6. A lot of the work here is done by AI
agents, openly. You are welcome with or without an agent of your own.

You do not need to be a developer to help. Pick a path.

## The hard rule: no game content

Never add, attach, paste or link: games or packages (`.nsp .xci .nsz .xcz .nca .tik`), extracted
`romfs/` / `exefs/`, keys (`prod.keys`, `title.keys`, `*.keys`), firmware, system fonts, saves, shader
dumps, or anything generated from a game. That covers commits, issues, comments, logs and screenshots.
CI blocks such files (`tools/check_no_game_content.py`). Do not ask where to get them; we will not help.

## Set up your agent

Give your agent this repo and tell it to read [`AGENTS.md`](AGENTS.md). Then:

1. **Tools to install**: run `tools/setup.sh --check`. It lists what is missing with the command to
   fix it (Linux, macOS; Windows through WSL2). Reporting and perf paths need no build tools.
2. **Skills to install for your agent** (the folder [`skills/`](skills/)):
   - Claude Code: `mkdir -p .claude && cp -r skills .claude/skills` (or `cp -r skills/* ~/.claude/skills/`).
   - Hermes Agent: `mkdir -p .agents && cp -r skills .agents/skills`, then `hermes skills trust` in the
     repo. Or add this repo's `skills/` folder to `skills.external_dirs` in `~/.hermes/config.yaml`.
   - Others: point the agent at `skills/<name>/SKILL.md`.

## Path 1 - Report an issue

You saw a crash, a black screen, a wrong sound. This is the most useful thing a tester can do.

- Ask your agent to use the `report-device-issue` skill, or open the **Bug report** form yourself.
- We need: app version or commit, device, SoC and GPU, GPU driver, Android version, game **version
  number** (not the file), steps, `adb logcat` excerpt with no game text or key material.
- A different-GPU result is a new data point, not a duplicate.

## Path 2 - Fix a bug

1. Find or open an issue. Comment to claim it so two people do not do the same work.
2. Branch from `main`. One bounded problem per PR.
3. Read `AGENTS.md` (map, commands). Add a synthetic test where practical.
4. Open a **draft** PR with the template: problem, change, evidence (real commands and results),
   limits, device tested on or "not device-tested", and AI disclosure.
5. A different reviewer approves. Nobody merges their own PR.

## Path 3 - Add a feature

Open a **Feature request** issue first and agree on the scope, especially for the renderer hot spots
(shader translator, Vulkan submission, texture cache). Then follow Path 2. A feature that needs a game
file to be tested says so in the issue and relies on a device report from someone who owns the game.

## Path 4 - Help performance

You can contribute without writing code:

- Ask your agent to use the `perf-report` skill, or fill the **Performance report** form.
- Same scene, same protocol, same settings: that is what makes two reports comparable. Include FPS
  (average and 1% low), device, SoC/GPU, driver, build SHA, resolution scale and other settings.
- Displayed FPS is not game speed: say if the game ran too fast or too slow.
- Code changes that improve performance must come with a before/after measurement.

## AI-assisted contributions

We use agents too, so the rules are the same for everyone:

- **Disclose it.** Commits and PR descriptions carry `Agent: <name> · Model: <id>`.
- **Evidence from real runs only.** Paste commands and results; write "not executed" for what you did
  not run. A predicted result is not a measurement.
- **"Tested on device" needs a device report** from a named tester (device, SoC/GPU, driver, build).
  Passing CI is not a device test.
- You are responsible for what your agent submits. No bulk or unsolicited automated PRs.

## Licensing

AC-NativeNX is **GPL-3.0-or-later**. By contributing you agree your work is published under it and that
you have the right to submit it. Keep copyright notices; credit the source and license of any code you
bring in (and update `NOTICE.md`). Code under an incompatible license cannot be merged.
We credit igawa6 (Raitchi), the yuzu and Eden projects.

## Not here

No PRs or issues to upstream projects on our behalf. Security problems: use GitHub's private
vulnerability reporting (Security tab), not a public issue.
