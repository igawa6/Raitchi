#!/usr/bin/env python3
"""Verify the pinned upstream inputs without fetching or modifying them."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def git(path, *args):
    return subprocess.check_output(
        ["git", "-C", str(path), *args], text=True, stderr=subprocess.STDOUT
    ).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", type=Path, default=ROOT / "sources.lock.json",
                        help="Source lock to verify; paths remain relative to the workspace")
    args = parser.parse_args()
    lock = json.loads(args.lock.read_text())
    failed = False
    for source in lock["sources"]:
        path = ROOT / source["path"]
        try:
            head = git(path, "rev-parse", "HEAD")
            origin = git(path, "remote", "get-url", "origin")
            dirty = git(path, "status", "--porcelain", "--untracked-files=no")
            errors = []
            if head != source["commit"]:
                errors.append("commit differs from lock")
            if origin != source["url"]:
                errors.append("origin differs from lock")
            if dirty:
                errors.append("tracked upstream files modified")
            failed |= bool(errors)
            print(f"{source['id']}: {'FAIL: ' + ', '.join(errors) if errors else 'OK'} ({head[:12]})")
        except (OSError, subprocess.CalledProcessError) as exc:
            print(f"{source['id']}: FAIL: {exc}")
            failed = True
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
