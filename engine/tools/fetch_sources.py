#!/usr/bin/env python3
"""Fetch the pinned public upstream references into sources/.

Reads sources.lock.json (and echoes-source.lock.json when present), clones each
public repository and checks out the exact pinned commit. Existing checkouts
are left alone unless they are at a different commit, in which case the script
stops instead of modifying them. Run tools/verify_sources.py afterwards.
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOCKS = ["sources.lock.json", "echoes-source.lock.json"]


def git(*args, cwd=None):
    return subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True, check=False)


def fetch(entry):
    dest = ROOT / entry["path"]
    commit, url = entry["commit"], entry["url"]
    if (dest / ".git").exists():
        head = git("rev-parse", "HEAD", cwd=dest).stdout.strip()
        if head == commit:
            return f"ok      {entry['id']} already at {commit[:12]}"
        return f"FAIL    {entry['id']} is at {head[:12]}, expected {commit[:12]}; not modified"
    dest.parent.mkdir(parents=True, exist_ok=True)
    steps = [("init", "-q", str(dest)),
             ("-C", str(dest), "remote", "add", "origin", url),
             ("-C", str(dest), "fetch", "-q", "--depth", "1", "origin", commit),
             ("-C", str(dest), "checkout", "-q", "--detach", "FETCH_HEAD")]
    for step in steps:
        r = git(*step)
        if r.returncode:
            return f"FAIL    {entry['id']}: git {' '.join(step[:3])}: {r.stderr.strip()[:200]}"
    head = git("rev-parse", "HEAD", cwd=dest).stdout.strip()
    return f"{'fetched' if head == commit else 'FAIL   '} {entry['id']} {head[:12]}"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--only", action="append", help="source id to fetch (repeatable)")
    args = ap.parse_args()
    entries = []
    for lock in LOCKS:
        path = ROOT / lock
        if path.exists():
            entries += json.loads(path.read_text(encoding="utf-8"))["sources"]
    if args.only:
        entries = [e for e in entries if e["id"] in args.only]
    lines = [fetch(e) for e in entries]
    print("\n".join(lines))
    return 1 if any(l.startswith("FAIL") for l in lines) else 0


if __name__ == "__main__":
    sys.exit(main())
