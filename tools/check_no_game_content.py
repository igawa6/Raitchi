#!/usr/bin/env python3
"""Refuse game content, console keys and oversized blobs in a source tree.

    python3 tools/check_no_game_content.py [root]

Exit status 0 = clean, 1 = at least one file refused. Every refused file is named
in the message. The guard never edits or deletes anything.

Refused:
  * game packages / containers: .nsp .xci .nsz .xcz .nca .tik
  * key files: prod.keys, title.keys, anything ending in .keys
  * directories named romfs/ or exefs/ (extracted game data)
  * any single file larger than 50 MB (APKs, dumps, caches do not belong in git)

AC-NativeNX never ships or needs any of these: users bring their own game and keys.
"""
import os
import sys
from pathlib import Path

BANNED_EXTENSIONS = {".nsp", ".xci", ".nsz", ".xcz", ".nca", ".tik"}
BANNED_DIRS = {"romfs", "exefs"}
MAX_BYTES = 50 * 1024 * 1024
SKIP_DIRS = {".git"}


def scan(root: Path):
    """Yield (relative_posix_path, reason) for each offending file."""
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        rel_dir = Path(dirpath).relative_to(root)
        in_banned_dir = any(part.lower() in BANNED_DIRS for part in rel_dir.parts)
        for name in filenames:
            path = Path(dirpath) / name
            rel = (rel_dir / name).as_posix()
            lower = name.lower()
            if in_banned_dir:
                yield rel, "inside a romfs/ or exefs/ directory (extracted game data)"
            elif Path(lower).suffix in BANNED_EXTENSIONS:
                yield rel, f"game package/container ({Path(lower).suffix})"
            elif lower.endswith(".keys"):
                yield rel, "console key file"
            else:
                try:
                    size = path.stat().st_size
                except OSError:
                    continue
                if size > MAX_BYTES:
                    yield rel, f"file larger than 50 MB ({size // (1024 * 1024)} MB)"
                continue
            # banned by name/dir: size check not needed


def main(argv) -> int:
    root = Path(argv[1] if len(argv) > 1 else Path(__file__).resolve().parents[1])
    if not root.is_dir():
        print(f"error: {root} is not a directory", file=sys.stderr)
        return 2
    findings = list(scan(root))
    if findings:
        print("REFUSED: game content, keys or oversized files found:", file=sys.stderr)
        for rel, reason in findings:
            print(f"  {rel}  --  {reason}", file=sys.stderr)
        print("Please remove these files from the commit (git rm --cached, then add them to "
              "your local ignore file). This project never contains games, keys or "
              "firmware. See CONTRIBUTING.md.", file=sys.stderr)
        return 1
    print(f"OK: no game content, keys or oversized files under {root}.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
