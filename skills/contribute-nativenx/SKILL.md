---
name: contribute-nativenx
description: Use when asked to fix a bug, add a feature or improve something in AC-NativeNX (Android Switch runtime) and deliver a reviewable pull request. Covers claiming the work, the no-game-content rule, tests, evidence, AI disclosure and the PR template.
---

# Contribute to AC-NativeNX

Goal: turn one bounded problem into a draft PR that a reviewer can check without asking you anything.

## Before touching code

1. Read `AGENTS.md` (rules, map, commands) and, for game-related work, `TESTING.md`.
2. Run `tools/setup.sh --check`. If a tool is `MISSING` or `WRONG-VERSION`, report that to the
   human with the printed command. Do not guess, do not half-install.
3. Find the issue, or open one (Feature request / Bug report form). Comment to claim it with your
   scope: files you expect to touch and what is out of scope. For renderer hot spots
   (`nvn/maxwell/translate.c`, `nvn/vulkan/exec.c`, `nvn/cmdbuf.c`, texture cache), wait for a reply.

## Red lines

- Never add games, keys, firmware, saves, `romfs/`/`exefs/` or anything generated from a game.
  If the task needs a game file, stop and say so.
- No PR/issue/push to upstream repos (igawa6/Raitchi, Eden, yuzu).
- Do not merge your own PR. Never claim "tested on device" without a device report.

## Work

1. `git switch -c fix/<short-topic>` from `main`. One problem per branch.
2. Write the failing synthetic test first when practical (dummy data only). Run it, see it fail.
3. Implement the minimal change. Keep GPL notices and copyright lines.
4. Run and record, with exact command and exit status:
   - `python3 -m unittest discover -s tests -v`
   - `python3 tools/check_no_game_content.py .`
   - `(cd engine && python3 tools/run_host_checks.py)` when you touched engine code
   Missing tool = skipped or failed, never passed. Write "not executed" for what you did not run.
5. Commit with Conventional Commits. Last body line: `Agent: <name> · Model: <id>`.

## Open the PR (draft)

Fill `.github/PULL_REQUEST_TEMPLATE.md` completely: problem, change, evidence (real output),
limits, "Device tested on" (or "not device-tested"), agent name + model. Request review from a
different identity. State what you did NOT verify (other GPUs, other game versions, device).
