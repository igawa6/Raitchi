#!/usr/bin/env python3
"""Recompute the hashed source manifests after editing engine/ or shell/ files.

Order matters because each file hashes the previous one:
  shell-overlay.json (shell snapshot files)
  -> engine/shell.lock.json source_overlay.sha256
  -> SOURCE-MANIFEST.json (every tracked engine/ and shell/ file)
  -> RELEASE.json source_manifest_sha256

The shell overlay file list itself is not changed: adding or deleting shell files
must be reflected by hand in shell-overlay.json ("files" / "deleted_upstream_files").
Use --check in CI to fail when a manifest is stale.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def dump(path, value, indent):
    return json.dumps(value, indent=indent, ensure_ascii=False) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="report stale manifests; write nothing")
    args = parser.parse_args()
    stale = []

    def write(path, value, indent=2):
        text = dump(path, value, indent)
        if path.read_text() != text:
            stale.append(str(path.relative_to(ROOT)))
            if not args.check:
                path.write_text(text)

    overlay_path = ROOT / "shell-overlay.json"
    overlay = json.loads(overlay_path.read_text())
    for row in overlay["files"]:
        row["sha256"] = sha(ROOT / "shell" / row["file"])
    write(overlay_path, overlay)

    lock_path = ROOT / "engine/shell.lock.json"
    lock = json.loads(lock_path.read_text())
    lock["source_overlay"]["sha256"] = sha(overlay_path) if not args.check else hashlib.sha256(
        dump(overlay_path, overlay, 2).encode()).hexdigest()
    write(lock_path, lock)

    manifest_path = ROOT / "SOURCE-MANIFEST.json"
    manifest = json.loads(manifest_path.read_text())
    tracked = subprocess.run(["git", "-C", str(ROOT), "ls-files", "-z", "--", "engine", "shell"],
                             check=True, capture_output=True).stdout.decode().split("\0")
    pending = {str(lock_path.relative_to(ROOT)): dump(lock_path, lock, 2).encode()}
    rows = []
    for name in sorted(n for n in tracked if n and (ROOT / n).is_file()):
        data = pending.get(name) or (ROOT / name).read_bytes()
        rows.append({"file": name, "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)})
    manifest["files"] = rows
    manifest["overlay_manifest_sha256"] = hashlib.sha256(dump(overlay_path, overlay, 2).encode()).hexdigest()
    write(manifest_path, manifest)

    release_path = ROOT / "RELEASE.json"
    release = json.loads(release_path.read_text())
    release["source_manifest_sha256"] = hashlib.sha256(dump(manifest_path, manifest, 2).encode()).hexdigest()
    write(release_path, release)

    if stale:
        print(("STALE " if args.check else "updated ") + ", ".join(stale))
    else:
        print("source manifests up to date")
    return 1 if (args.check and stale) else 0


if __name__ == "__main__":
    sys.exit(main())
