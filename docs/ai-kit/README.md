# AI kit

The "AI kit" makes a repository easy to work on for a contributor and their own coding agent:
`AGENTS.md`, `CLAUDE.md`, `CONTRIBUTING.md`, `skills/`, issue and PR templates, `tools/setup.sh`
and the no-game-content guard. Its **single source is this repository (AC-NativeNX)**. The sibling
project `letsgo-recomp-android` gets a copy through a script.

## What is synced

| File | Notes |
|---|---|
| `skills/*` | generic, copied as is |
| `.github/ISSUE_TEMPLATE/*`, `.github/PULL_REQUEST_TEMPLATE.md` | copied; the target keeps its own `config.yml` if it has one |
| `tools/check_no_game_content.py`, `tests/test_check_no_game_content.py`, `.github/workflows/guard.yml` | copied as is |
| `tools/setup.sh`, `tools/setup.ps1`, `tests/test_setup_check.sh` | copied as is |
| `docs/ai-kit/*` | copied |

## What is NOT synced (project specific, written by hand in each repo)

`AGENTS.md`, `CLAUDE.md`, `CONTRIBUTING.md`. They name the project, its licence, its map and its
build commands. Update them by hand when the kit changes behaviour.

## Usage

```sh
docs/ai-kit/sync-to.sh --dry-run ../letsgo-recomp-android   # show what would change
docs/ai-kit/sync-to.sh ../letsgo-recomp-android             # copy, then review `git diff` in the target
```

The script only copies the files listed above, never deletes anything, and refuses to run if the
target is not a git work tree. It leaves committing to you: open a PR in the target repo, with the
AI disclosure line.

Project-specific bits inside synced files (for example toolchain versions in `tools/setup.sh`, or
names in templates) must be adapted in the target after syncing; keep those edits small so the next
sync stays a clean diff.
