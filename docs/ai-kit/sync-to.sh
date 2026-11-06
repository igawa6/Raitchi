#!/usr/bin/env bash
# Copy the shared AI kit from this repo into a sibling repo. See docs/ai-kit/README.md.
#   docs/ai-kit/sync-to.sh [--dry-run] <target-repo-dir>
set -eu

DRY=0
if [ "${1:-}" = "--dry-run" ]; then DRY=1; shift; fi
TARGET="${1:-}"
[ -n "$TARGET" ] || { echo "usage: $0 [--dry-run] <target-repo-dir>" >&2; exit 2; }
[ -d "$TARGET" ] || { echo "error: $TARGET is not a directory" >&2; exit 2; }
git -C "$TARGET" rev-parse --is-inside-work-tree >/dev/null 2>&1 \
  || { echo "error: $TARGET is not a git work tree" >&2; exit 2; }

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TARGET="$(cd "$TARGET" && pwd)"
[ "$SRC" != "$TARGET" ] || { echo "error: source and target are the same repo" >&2; exit 2; }

FILES=()
while IFS= read -r f; do FILES+=("$f"); done < <(
  cd "$SRC" && {
    find skills docs/ai-kit -type f
    printf '%s\n' \
      .github/PULL_REQUEST_TEMPLATE.md \
      .github/workflows/guard.yml \
      tools/check_no_game_content.py tools/setup.sh tools/setup.ps1 \
      tests/test_check_no_game_content.py tests/test_setup_check.sh
    find .github/ISSUE_TEMPLATE -type f ! -name config.yml
  } | sort)

for f in "${FILES[@]}"; do
  if [ -e "$TARGET/$f" ] && cmp -s "$SRC/$f" "$TARGET/$f"; then continue; fi
  if [ "$DRY" -eq 1 ]; then
    echo "would copy $f"
  else
    mkdir -p "$TARGET/$(dirname "$f")"
    cp -p "$SRC/$f" "$TARGET/$f"
    echo "copied $f"
  fi
done
[ "$DRY" -eq 1 ] && echo "(dry run: nothing written)" || echo "Done. Review 'git -C $TARGET status', adapt project-specific bits, then open a PR."
